/**
 * @file skworldcensus.cpp
 * @brief The census: every declared physics shape near the agent, as the navmesh sees it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skworldcensus.h"

#include "sknavoverlay.h"
#include "sknavpalette.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llglslshader.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llpathfindinglinkset.h"
#include "llpathfindingmanager.h"
#include "llpathfindingobjectlist.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llviewershadermgr.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "skphysicsshape.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace sknav;

    const F32 MOVED_EPSILON = 0.01f;

    LLViewerObject* rootOf(LLViewerObject* object)
    {
        LLViewerObject* root = object;
        while (!root->isRoot() && root->getParent())
        {
            root = static_cast<LLViewerObject*>(root->getParent());
        }
        return root;
    }

    // The region transform the simulator knows for a part: every link
    // contributes the rotation the sim last set. The viewer applies
    // llTargetOmega to a non-physical object or a linkset child on its own,
    // and the sim's physics never sees that spin, so the shape the sim
    // declares sits at this transform while the rendered one turns for as
    // long as the prim spins.
    void simRegionTransform(const LLViewerObject* object, LLVector3& pos, LLQuaternion& rot)
    {
        if (object->isRoot())
        {
            pos = object->getPosition();
            rot = object->getRotationSim();
        }
        else
        {
            const LLViewerObject* parent = static_cast<const LLViewerObject*>(object->getParent());
            simRegionTransform(parent, pos, rot);
            pos += object->getPosition() * rot;
            rot = object->getRotationSim() * rot;
        }
    }

    U64 mixVec(U64 h, const LLVector3& v)
    {
        h = mixF32(h, v.mV[0]);
        h = mixF32(h, v.mV[1]);
        return mixF32(h, v.mV[2]);
    }

    U64 mixQuat(U64 h, const LLQuaternion& q)
    {
        for (S32 i = 0; i < 4; ++i)
        {
            h = mixF32(h, q.mQ[i]);
        }
        return h;
    }

    NavRole fromLinksetUse(S32 use)
    {
        switch (use)
        {
            case LLPathfindingLinkset::kWalkable: return NavRole::WALKABLE;
            case LLPathfindingLinkset::kStaticObstacle: return NavRole::STATIC_OBSTACLE;
            case LLPathfindingLinkset::kMaterialVolume: return NavRole::MATERIAL_VOLUME;
            case LLPathfindingLinkset::kExclusionVolume: return NavRole::EXCLUSION_VOLUME;
            case LLPathfindingLinkset::kDynamicObstacle:
            case LLPathfindingLinkset::kDynamicPhantom:
            default: return NavRole::UNKNOWN;
        }
    }

    /// The region's object cache, as an octree of entries that have bounds but
    /// no object: everything the sim has told us about that is too far away to
    /// have been drawn. Only linkset roots are in the tree, and a root's bounds
    /// cover its children, so this yields one box per linkset.
    class CacheGather : public OctreeTraveler
    {
    public:
        CacheGather(const LLVector4a& mn, const LLVector4a& mx, std::vector<LLVOCacheEntry*>& out)
            : mMin(mn), mMax(mx), mOut(out)
        {
        }

        void traverse(const OctreeNode* node) override
        {
            const LLVector4a& c = node->getCenter();
            const LLVector4a& s = node->getSize();
            for (S32 a = 0; a < 3; ++a)
            {
                if (c[a] + s[a] < mMin[a] || c[a] - s[a] > mMax[a])
                {
                    return;
                }
            }
            OctreeTraveler::traverse(node);
        }

        void visit(const OctreeNode* node) override
        {
            for (auto i = node->getDataBegin(); i != node->getDataEnd(); ++i)
            {
                LLViewerOctreeEntry* entry = *i;
                // A drawable means the object exists and the live pass has it,
                // with its real shape rather than a box.
                if (!entry || entry->hasDrawable() || !entry->hasVOCacheEntry())
                {
                    continue;
                }
                mOut.push_back(static_cast<LLVOCacheEntry*>(entry->getVOCacheEntry()));
            }
        }

    private:
        LLVector4a mMin, mMax;
        std::vector<LLVOCacheEntry*>& mOut;
    };

    void extend(LLVector3d& mn, LLVector3d& mx, const LLVector3d& p)
    {
        for (S32 i = 0; i < 3; ++i)
        {
            mn.mdV[i] = std::min(mn.mdV[i], p.mdV[i]);
            mx.mdV[i] = std::max(mx.mdV[i], p.mdV[i]);
        }
    }
}

SKWorldCensus::SKWorldCensus()
{
}

SKWorldCensus::~SKWorldCensus()
{
}

void SKWorldCensus::cleanupSingleton()
{
    abandonScan();
}

void SKWorldCensus::addDemand(const std::string& who, F32 range_m)
{
    mDemands[who] = range_m;
}

void SKWorldCensus::removeDemand(const std::string& who)
{
    mDemands.erase(who);
}

bool SKWorldCensus::wanted() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavMeshEnabled", true);
    static LLCachedControl<bool> always(gSavedSettings, "SKNavCensusAlways", false);
    if (!enabled)
    {
        return false;
    }
    return always || !mDemands.empty() || gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH);
}

F64 SKWorldCensus::range() const
{
    static LLCachedControl<F32> setting(gSavedSettings, "SKNavRange", 512.f);
    F64 r = setting;
    for (const auto& d : mDemands)
    {
        r = std::max(r, static_cast<F64>(d.second));
    }
    return r;
}

sknav::NavRole SKWorldCensus::roleOf(const LLUUID& root) const
{
    const auto it = mRoles.find(root);
    return it == mRoles.end() ? NavRole::UNKNOWN : it->second;
}

std::vector<U64> SKWorldCensus::regionHandles()
{
    std::vector<U64> handles;
    if (LLWorld::instanceExists())
    {
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            handles.push_back(region->getHandle());
        }
        std::sort(handles.begin(), handles.end());
    }
    return handles;
}

bool SKWorldCensus::sameRegions(const std::vector<U64>& regions) const
{
    return regionHandles() == regions;
}

void SKWorldCensus::update()
{
    if (!wanted() || !LLWorld::instanceExists() || !gAgent.getRegion())
    {
        if (mScan.mActive)
        {
            abandonScan();
        }
        return;
    }

    static LLCachedControl<F32> budget(gSavedSettings, "SKNavCensusBudgetMS", 3.f);

    if (!mScan.mActive)
    {
        beginScan();
    }
    else if (!sameRegions(mScan.mRegions))
    {
        abandonScan();
        beginScan();
    }
    continueScan(llmax(0.1f, static_cast<F32>(budget)));

    if (mRolesWanted)
    {
        requestRoles();
    }

    static LLCachedControl<bool> log(gSavedSettings, "SKNavCensusLog", false);
    if (log && mSnapshot && gFrameTimeSeconds - mLastLog > 5.f)
    {
        mLastLog = gFrameTimeSeconds;
        LL_INFOS("SKNav") << statsLine() << LL_ENDL;
    }
}

void SKWorldCensus::beginScan()
{
    mScan.mPending = std::make_shared<SKCensusSnapshot>();
    mScan.mPending->mCenter = gAgent.getPositionGlobal();
    // Where you are looking matters as much as where you stand: cam across a
    // sim and the mesh should be there, while the ground under the avatar
    // stays built whatever the camera does.
    mScan.mPending->mCamera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    mScan.mPending->mRange = range();
    mScan.mObjects.clear();
    const S32 count = gObjectList.getNumObjects();
    mScan.mObjects.reserve(count);
    for (S32 i = 0; i < count; ++i)
    {
        if (LLViewerObject* object = gObjectList.getObject(i))
        {
            mScan.mObjects.push_back(object);
        }
    }
    mScan.mIndex = 0;
    mScan.mRegions = regionHandles();
    mScan.mCached.clear();
    mScan.mCachedIndex = 0;
    gatherCached();
    mScan.mElapsedMs = 0.f;
    mScan.mActive = true;
}

void SKWorldCensus::abandonScan()
{
    mScan.mPending.reset();
    mScan.mObjects.clear();
    mScan.mActive = false;
}

void SKWorldCensus::continueScan(F32 budget_ms)
{
    LLTimer timer;
    const S32 count = static_cast<S32>(mScan.mObjects.size());
    while (mScan.mIndex < count)
    {
        if (timer.getElapsedTimeF32() * 1000.f > budget_ms)
        {
            mScan.mElapsedMs += timer.getElapsedTimeF32() * 1000.f;
            return;
        }
        LLViewerObject* object = mScan.mObjects[mScan.mIndex++].get();
        if (object && !object->isDead())
        {
            visit(object);
        }
    }
    mScan.mObjects.clear();

    // Then what the region has cached but never drawn, under the same budget.
    while (mScan.mCachedIndex < mScan.mCached.size())
    {
        if (timer.getElapsedTimeF32() * 1000.f > budget_ms)
        {
            mScan.mElapsedMs += timer.getElapsedTimeF32() * 1000.f;
            return;
        }
        visitCached(mScan.mCached[mScan.mCachedIndex++]);
    }
    mScan.mCached.clear();
    mScan.mElapsedMs += timer.getElapsedTimeF32() * 1000.f;
    finishScan();
}

void SKWorldCensus::finishScan()
{
    static LLCachedControl<F32> gap(gSavedSettings, "SKNavClusterGap", 0.5f);
    std::shared_ptr<SKCensusSnapshot> done = mScan.mPending;
    mScan.mActive = false;
    mScan.mPending.reset();

    LLTimer timer;
    done->mClusters = cluster(done->mRecords, gap);
    done->mStats.mClusterMs = timer.getElapsedTimeF32() * 1000.f;
    done->mStats.mScanMs = mScan.mElapsedMs;
    done->mStats.mRecords = static_cast<U32>(done->mRecords.size());
    done->mStats.mRolesKnown = static_cast<U32>(mRoles.size());
    done->mStamp = ++mStamp;

    // Parts and roots not seen for a minute leave the caches.
    for (auto it = mParts.begin(); it != mParts.end();)
    {
        it = (gFrameTimeSeconds - it->second.mLastSeen > 60.f) ? mParts.erase(it) : std::next(it);
    }
    for (auto it = mRest.begin(); it != mRest.end();)
    {
        it = (gFrameTimeSeconds - it->second.mLastSeen > 60.f) ? mRest.erase(it) : std::next(it);
    }
    mSnapshot = done;
}

bool SKWorldCensus::classifyRest(LLViewerObject* root, const LLVector3d& pos)
{
    static LLCachedControl<F32> settle(gSavedSettings, "SKNavSettleSeconds", 4.f);
    // The baseline moves only when the root is seen to have moved, so a slow
    // drift adds up to a move instead of hiding under the epsilon each frame.
    Rest& rest = mRest[root->getID()];
    if (!rest.mKnown || dist_vec(rest.mPosition, pos) > MOVED_EPSILON)
    {
        if (rest.mKnown)
        {
            rest.mMovedAt = gFrameTimeSeconds;
        }
        rest.mPosition = pos;
    }
    rest.mLastSeen = gFrameTimeSeconds;
    rest.mKnown = true;
    return rest.mMovedAt >= 0.f && gFrameTimeSeconds - rest.mMovedAt < settle;
}

bool SKWorldCensus::isSurround(LLViewerObject* root, const LLVector3d& mn, const LLVector3d& mx) const
{
    if (root->isLocalOnly())
    {
        return true;
    }
    // How far the thing reaches outside every connected region. Asking where
    // its centre is was wrong: a sim surround is usually a megaprim rezzed
    // inside the sim whose skirt hangs a long way past the edge, and by its
    // centre it is an ordinary object - which then falls at the physics gate,
    // since a surround is phantom or shape-NONE precisely because there is no
    // physics out in the void.
    static LLCachedControl<F32> reach(gSavedSettings, "SKNavSurroundReach", 8.f);
    F64 outside = 0.0;
    const LLVector3d corners[4] = {
        mn,
        LLVector3d(mx.mdV[VX], mn.mdV[VY], 0.0),
        LLVector3d(mn.mdV[VX], mx.mdV[VY], 0.0),
        mx,
    };
    for (const LLVector3d& corner : corners)
    {
        F64 nearest = 1e9;
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            const LLVector3d& o = region->getOriginGlobal();
            const F64 w = region->getWidth();
            // How far outside this region the corner is, zero when within it.
            const F64 dx = llmax(o.mdV[VX] - corner.mdV[VX], corner.mdV[VX] - (o.mdV[VX] + w), 0.0);
            const F64 dy = llmax(o.mdV[VY] - corner.mdV[VY], corner.mdV[VY] - (o.mdV[VY] + w), 0.0);
            nearest = llmin(nearest, llmax(dx, dy));
        }
        outside = llmax(outside, nearest);
    }
    return outside > reach;
}

void SKWorldCensus::visit(LLViewerObject* object)
{
    SKCensusSnapshot& snap = *mScan.mPending;
    snap.mStats.mObjectsSeen++;

    if (object->isDead() || object->isOrphaned() || object->isAttachment() || !object->getRegion() ||
        object->getPCode() != LL_PCODE_VOLUME)
    {
        return;
    }
    LLVOVolume* volume = static_cast<LLVOVolume*>(object);
    if (!volume->getVolume())
    {
        return;
    }
    LLViewerObject* root = rootOf(object);
    if (root->isAttachment() || root->asAvatar())
    {
        return;
    }

    // Movers and things that are not shapes.
    if (root->flagUsePhysics() || root->flagCharacter() || root->flagVolumeDetect() || root->flagTemporaryOnRez())
    {
        return;
    }

    // Cull by the part's reach, not its centre: a 200 m surround whose centre
    // is far away still hangs into the range.
    const LLVector3d pos = object->getRegion()->getOriginGlobal() + LLVector3d(object->getPositionRegion());
    const F64 reach = snap.mRange + 0.5 * object->getScale().magVec();
    if (dist_vec(pos, snap.mCenter) > reach && dist_vec(pos, snap.mCamera) > reach)
    {
        return;
    }

    // What the record is signed and built from: the sim-side transform, which
    // holds still while a part spins on its own (simRegionTransform).
    LLVector3 sim_pos;
    LLQuaternion sim_rot;
    simRegionTransform(object, sim_pos, sim_rot);

    const bool dynamic = classifyRest(root, root->getRegion()->getOriginGlobal() + LLVector3d(root->getPositionRegion()));
    const NavRole navRole = roleOf(root->getID());
    if (root->flagObjectPermanent() && navRole == NavRole::UNKNOWN)
    {
        mRolesWanted = true;
    }

    // Phantom stays out unless the sim says it is an exclusion volume; a
    // surround stays in whatever its flags, as its rendered shape.
    const bool phantom = root->flagPhantom();
    // Its own extent, not its centre: that is the whole point of a surround.
    // A rotated box, not the scale's magnitude smeared over x and y: a tall
    // thin tower is not a wide one, and smearing it calls an ordinary
    // structure a surround because a corner of the smear pokes outside the
    // region.
    const LLVector3 half = object->getScale() * 0.5f;
    const LLQuaternion rot = sim_rot;
    const LLVector3 axis[3] = { LLVector3(1.f, 0.f, 0.f) * rot, LLVector3(0.f, 1.f, 0.f) * rot, LLVector3(0.f, 0.f, 1.f) * rot };
    LLVector3 extent;
    for (S32 i = 0; i < 3; ++i)
    {
        extent.mV[i] = std::fabs(axis[0].mV[i]) * half.mV[0] + std::fabs(axis[1].mV[i]) * half.mV[1]
                     + std::fabs(axis[2].mV[i]) * half.mV[2];
    }
    const LLVector3d span(extent);
    const bool surround = isSurround(root, pos - span, pos + span);
    if (phantom && navRole != NavRole::EXCLUSION_VOLUME && !surround)
    {
        return;
    }

    SKPhysicsShape shape;
    const bool hasPhysics = skPhysicsShapeClassify(volume, shape);
    if (!hasPhysics && !surround)
    {
        return;
    }

    U64 sig = SEED;
    // The root is part of the signature: relinking changes the cluster a part
    // belongs to without moving it.
    sig = mix(sig, *reinterpret_cast<const U64*>(root->getID().mData));
    sig = mix(sig, *reinterpret_cast<const U64*>(root->getID().mData + 8));
    sig = mixVec(sig, sim_pos);
    sig = mixQuat(sig, sim_rot);
    sig = mixVec(sig, object->getScale());
    sig = mix(sig, shape.mPhysicsType);
    sig = mix(sig, hasPhysics ? static_cast<U64>(shape.mSpec.getType()) : 99);
    sig = mix(sig, phantom);
    sig = mix(sig, surround);
    sig = mix(sig, static_cast<U64>(navRole));
    sig = mix(sig, object->isPhysicsShapeUnknown());
    // Only a surround is built from the rendered volume, so only a surround
    // depends on the loaded LOD. Mixing it in for everything else re-signs
    // records as the camera moves and rebuilds their tiles for nothing, which
    // is what leaves tiles in flight while you walk. The display volume's face
    // count and the hull the physics-shapes overlay caches on it are the same
    // trap and are not what any shape is built from; the build reads a fixed
    // detail or the decomposition instead (skphysicsshape.cpp).
    const bool from_render = surround;
    sig = mix(sig, from_render ? volume->getLOD() : 0);
    {
        const LLVolume* v = volume->getVolume();
        sig = mixF64(sig, object->getRegion()->getOriginGlobal().mdV[VX]);
        sig = mixF64(sig, object->getRegion()->getOriginGlobal().mdV[VY]);
        // The volume parameters: a cut, hollow or twist edit keeps position,
        // scale and face count and still changes every triangle.
        const LLVolumeParams& vp = v->getParams();
        const LLPathParams& path = vp.getPathParams();
        const LLProfileParams& profile = vp.getProfileParams();
        sig = mix(sig, path.getCurveType());
        sig = mixF32(sig, path.getBegin());
        sig = mixF32(sig, path.getEnd());
        sig = mixF32(sig, path.getScaleX());
        sig = mixF32(sig, path.getScaleY());
        sig = mixF32(sig, path.getShearX());
        sig = mixF32(sig, path.getShearY());
        sig = mixF32(sig, path.getTwist());
        sig = mixF32(sig, path.getTwistBegin());
        sig = mixF32(sig, path.getRadiusOffset());
        sig = mixF32(sig, path.getTaperX());
        sig = mixF32(sig, path.getTaperY());
        sig = mixF32(sig, path.getRevolutions());
        sig = mixF32(sig, path.getSkew());
        sig = mix(sig, profile.getCurveType());
        sig = mixF32(sig, profile.getBegin());
        sig = mixF32(sig, profile.getEnd());
        sig = mixF32(sig, profile.getHollow());
        const LLUUID& sculpt = vp.getSculptID();
        sig = mix(sig, *reinterpret_cast<const U64*>(sculpt.mData));
        sig = mix(sig, *reinterpret_cast<const U64*>(sculpt.mData + 8));
        // A mesh's decomposition arriving is what turns a placeholder box into
        // hulls; its state has to move the signature or the box stays for ever.
        if (volume->isMesh())
        {
            const LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(sculpt);
            sig = mix(sig, decomp ? 1 + decomp->mMesh.size() * 4 + decomp->mPhysicsShapeMesh.mPositions.size() + decomp->mBaseHullMesh.mPositions.size() : 0);
        }
    }

    PartCache& cache = mParts[object->getID()];
    cache.mLastSeen = gFrameTimeSeconds;
    RecordPtr rec;
    if (cache.mRecord && cache.mSignature == sig && cache.mRecord->mDynamic == dynamic)
    {
        rec = cache.mRecord;
        snap.mStats.mCached++;
    }
    else
    {
        rec = record(volume, root, surround ? Role::SURROUND : Role::NAV, dynamic, sig, sim_pos, sim_rot);
        if (!rec)
        {
            return;
        }
        cache.mRecord = rec;
        cache.mSignature = sig;
    }

    snap.mRecords.push_back(rec);
    if (rec->mDynamic) snap.mStats.mDynamic++;
    if (rec->mRole == Role::SURROUND) snap.mStats.mSurround++;
    if (rec->mProvenance == Provenance::BOX) snap.mStats.mPlaceholders++;
    if (rec->mTriangles) snap.mStats.mTriangles += static_cast<U32>(rec->mTriangles->size() / 3);
}

void SKWorldCensus::gatherCached()
{
    static LLCachedControl<bool> use_cache(gSavedSettings, "SKNavCensusObjectCache", true);
    if (!use_cache || !LLWorld::instanceExists())
    {
        return;
    }
    const SKCensusSnapshot& snap = *mScan.mPending;
    // One box around both centres for the octree, then the round test per
    // entry, the same way the live pass culls.
    LLVector3d gmin, gmax;
    for (S32 a = 0; a < 3; ++a)
    {
        gmin.mdV[a] = llmin(snap.mCenter.mdV[a], snap.mCamera.mdV[a]) - snap.mRange;
        gmax.mdV[a] = llmax(snap.mCenter.mdV[a], snap.mCamera.mdV[a]) + snap.mRange;
    }

    std::vector<LLVOCacheEntry*> found;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        LLVOCachePartition* partition = region->getVOCachePartition();
        if (!partition || !partition->mOctree)
        {
            continue;
        }
        const LLVector3d origin = region->getOriginGlobal();
        const LLVector3 lo(gmin - origin), hi(gmax - origin);
        const LLVector4a mn(lo.mV[VX], lo.mV[VY], lo.mV[VZ]), mx(hi.mV[VX], hi.mV[VY], hi.mV[VZ]);

        found.clear();
        CacheGather gather(mn, mx, found);
        gather.traverse(partition->mOctree);

        const U64 handle = region->getHandle();
        for (LLVOCacheEntry* entry : found)
        {
            if (!entry || !entry->isValid() || entry->isState(LLVOCacheEntry::ACTIVE) || !entry->getEntry())
            {
                continue;
            }
            const LLVector4a* extents = entry->getSpatialExtents();
            CachedPart part;
            part.mOrigin = origin;
            part.mCenter.set((extents[0][0] + extents[1][0]) * 0.5f, (extents[0][1] + extents[1][1]) * 0.5f,
                             (extents[0][2] + extents[1][2]) * 0.5f);
            part.mHalf.set((extents[1][0] - extents[0][0]) * 0.5f, (extents[1][1] - extents[0][1]) * 0.5f,
                           (extents[1][2] - extents[0][2]) * 0.5f);
            if (part.mHalf.mV[VX] <= 0.f || part.mHalf.mV[VY] <= 0.f || part.mHalf.mV[VZ] <= 0.f)
            {
                continue;
            }
            const LLVector3d pos = origin + LLVector3d(part.mCenter);
            const F64 reach = snap.mRange + part.mHalf.magVec();
            if (dist_vec(pos, snap.mCenter) > reach && dist_vec(pos, snap.mCamera) > reach)
            {
                continue;
            }
            // No asset id is ever going to arrive for something we have not
            // drawn, so the record is keyed by where it is instead: the region
            // and the local id, tagged so it cannot collide with a real one.
            const U32 local = entry->getLocalID();
            const U32 tag = 0x534b4e56;     // 'SKNV'
            memcpy(part.mId.mData, &handle, sizeof(handle));
            memcpy(part.mId.mData + 8, &local, sizeof(local));
            memcpy(part.mId.mData + 12, &tag, sizeof(tag));
            mScan.mCached.push_back(part);
        }
    }
}

void SKWorldCensus::visitCached(const CachedPart& part)
{
    SKCensusSnapshot& snap = *mScan.mPending;
    snap.mStats.mObjectsSeen++;

    U64 sig = SEED;
    sig = mix(sig, *reinterpret_cast<const U64*>(part.mId.mData));
    sig = mix(sig, *reinterpret_cast<const U64*>(part.mId.mData + 8));
    sig = mixVec(sig, part.mCenter);
    sig = mixVec(sig, part.mHalf);
    sig = mixF64(sig, part.mOrigin.mdV[VX]);
    sig = mixF64(sig, part.mOrigin.mdV[VY]);

    PartCache& cache = mParts[part.mId];
    cache.mLastSeen = gFrameTimeSeconds;
    RecordPtr rec;
    if (cache.mRecord && cache.mSignature == sig)
    {
        rec = cache.mRecord;
        snap.mStats.mCached++;
    }
    else
    {
        std::shared_ptr<Record> made = std::make_shared<Record>();
        made->mRoot = part.mId;
        made->mPart = part.mId;
        made->mRole = Role::NAV;
        // The same meaning BOX already carries: a stand-in until the real
        // shape turns up, which here is when you get close enough to draw it.
        made->mProvenance = Provenance::BOX;
        made->mShape = Shape::BOX;
        made->mOrigin = part.mOrigin;
        made->mCenter = part.mCenter;
        made->mAxes[0].set(1.f, 0.f, 0.f);
        made->mAxes[1].set(0.f, 1.f, 0.f);
        made->mAxes[2].set(0.f, 0.f, 1.f);
        made->mHalf = part.mHalf;
        made->mMin = part.mOrigin + LLVector3d(part.mCenter - part.mHalf);
        made->mMax = part.mOrigin + LLVector3d(part.mCenter + part.mHalf);
        made->mSignature = sig;
        rec = made;
        cache.mRecord = rec;
        cache.mSignature = sig;
    }

    snap.mRecords.push_back(rec);
    snap.mStats.mPlaceholders++;
    snap.mStats.mFromObjectCache++;
}

sknav::RecordPtr SKWorldCensus::record(LLVOVolume* volume, LLViewerObject* root, sknav::Role role, bool dynamic, U64 signature,
                                       const LLVector3& pos, const LLQuaternion& rot)
{
    SKShapeGeometry geom;
    bool ok = false;
    if (role == Role::SURROUND)
    {
        ok = skRenderShapeGeometry(volume, geom);
    }
    else
    {
        SKPhysicsShape shape;
        ok = skPhysicsShapeClassify(volume, shape) && skPhysicsShapeGeometry(volume, shape, geom);
    }
    if (!ok)
    {
        return nullptr;
    }

    auto rec = std::make_shared<Record>();
    rec->mRoot = root->getID();
    rec->mPart = volume->getID();
    rec->mRole = role;
    rec->mProvenance = geom.mProvenance;
    rec->mNavRole = roleOf(root->getID());
    rec->mShape = geom.mShape;
    rec->mDynamic = dynamic;
    rec->mSignature = signature;
    rec->mOrigin = volume->getRegion()->getOriginGlobal();

    const LLVector3 scale = volume->getScale();

    rec->mMin.set(1e30, 1e30, 1e30);
    rec->mMax.set(-1e30, -1e30, -1e30);

    if (geom.mShape == Shape::TRIANGLES)
    {
        auto tris = std::make_shared<std::vector<LLVector3>>();
        tris->reserve(geom.mTriangles.size());
        for (const LLVector3& p : geom.mTriangles)
        {
            LLVector3 q = p;
            q.scaleVec(scale);
            q = q * rot + pos;
            tris->push_back(q);
            extend(rec->mMin, rec->mMax, rec->mOrigin + LLVector3d(q));
        }
        rec->mTriangles = tris;
        rec->mConvexTris = geom.mConvexTris;
    }
    else
    {
        LLVector3 c = geom.mCenter;
        c.scaleVec(scale);
        rec->mCenter = c * rot + pos;
        rec->mHalf.set(geom.mHalf[0] * scale[0], geom.mHalf[1] * scale[1], geom.mHalf[2] * scale[2]);
        rec->mAxes[0] = LLVector3(1, 0, 0) * rot;
        rec->mAxes[1] = LLVector3(0, 1, 0) * rot;
        rec->mAxes[2] = LLVector3(0, 0, 1) * rot;
        LLVector3 extent;
        for (S32 i = 0; i < 3; ++i)
        {
            extent.mV[i] = std::fabs(rec->mAxes[0][i]) * rec->mHalf[0] + std::fabs(rec->mAxes[1][i]) * rec->mHalf[1] + std::fabs(rec->mAxes[2][i]) * rec->mHalf[2];
        }
        rec->mMin = rec->mOrigin + LLVector3d(rec->mCenter - extent);
        rec->mMax = rec->mOrigin + LLVector3d(rec->mCenter + extent);
    }
    return rec;
}

void SKWorldCensus::requestRoles()
{
    static LLCachedControl<F32> every(gSavedSettings, "SKNavRoleRequestSeconds", 60.f);
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || !region->capabilitiesReceived())
    {
        return;
    }
    const bool sameRegion = region->getHandle() == mRolesRegion;
    if (sameRegion && mRolesRequestedAt >= 0.f && gFrameTimeSeconds - mRolesRequestedAt < every)
    {
        return;
    }
    if (!sameRegion)
    {
        // Roles are per region; the old region's linksets are gone with it.
        mRoles.clear();
    }
    mRolesWanted = false;
    mRolesRequestedAt = gFrameTimeSeconds;
    mRolesRegion = region->getHandle();
    const U32 id = ++mRolesRequestId;
    LLPathfindingManager::getInstance()->requestGetLinksets(id, [this](U32 request, LLPathfindingManager::ERequestStatus status, LLPathfindingObjectListPtr list) {
        onRoles(request, status, list);
    });
}

void SKWorldCensus::onRoles(U32 request, S32 status, std::shared_ptr<LLPathfindingObjectList> list)
{
    if (request != mRolesRequestId || status != LLPathfindingManager::kRequestCompleted || !list)
    {
        return;
    }
    U32 changed = 0;
    for (auto it = list->begin(); it != list->end(); ++it)
    {
        const LLPathfindingLinkset* linkset = dynamic_cast<const LLPathfindingLinkset*>(it->second.get());
        if (!linkset || linkset->isTerrain())
        {
            continue;
        }
        const NavRole role = fromLinksetUse(linkset->getLinksetUse());
        NavRole& slot = mRoles[linkset->getUUID()];
        if (slot != role)
        {
            slot = role;
            changed++;
        }
    }
    if (changed)
    {
        // Roles are part of the part signature, so the next scan re-records.
        LL_INFOS("SKNav") << "navmesh roles: " << changed << " linksets changed" << LL_ENDL;
    }
}

std::string SKWorldCensus::statsLine() const
{
    if (!mSnapshot)
    {
        return "census: none";
    }
    const SKCensusSnapshot::Stats& s = mSnapshot->mStats;
    size_t largest = 0;
    for (const ClusterPtr& c : mSnapshot->mClusters.mClusters)
    {
        largest = std::max(largest, c->mRecords.size());
    }
    return llformat("census #%llu: %u objects, %u records (%u cached, %u dynamic, %u surround, %u placeholders, %u from object cache), %u tris, %u roles; %zu clusters, largest %zu; scan %.1f ms, cluster %.1f ms",
                    static_cast<unsigned long long>(mSnapshot->mStamp), s.mObjectsSeen, s.mRecords, s.mCached, s.mDynamic, s.mSurround,
                    s.mPlaceholders, s.mFromObjectCache, s.mTriangles, s.mRolesKnown, mSnapshot->mClusters.mClusters.size(), largest, s.mScanMs, s.mClusterMs);
}

void SKWorldCensus::renderDebug()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKNavOverlayCensus", false);
    SKCensusSnapshotPtr snap = mSnapshot;
    if (!show || !snap)
    {
        return;
    }

    SKNavOverlayGeom geom;
    for (const RecordPtr& r : snap->mRecords)
    {
        SKNavSwatch swatch = SKNavSwatch::SHAPE_BOX;
        if (r->mDynamic)
        {
            swatch = SKNavSwatch::SHAPE_DYNAMIC;
        }
        else
        {
            switch (r->mProvenance)
            {
                case Provenance::EXACT: swatch = SKNavSwatch::SHAPE_EXACT; break;
                case Provenance::HULL: swatch = SKNavSwatch::SHAPE_HULL; break;
                case Provenance::BOX: swatch = SKNavSwatch::SHAPE_BOX; break;
                case Provenance::RENDER: swatch = SKNavSwatch::SHAPE_RENDER; break;
                case Provenance::BASE_HULL: swatch = SKNavSwatch::SHAPE_HULL; break;
            }
        }
        geom.box(gAgent.getPosAgentFromGlobal(r->mMin), gAgent.getPosAgentFromGlobal(r->mMax), skNavSwatchColor(swatch));
    }
    for (const ClusterPtr& c : snap->mClusters.mClusters)
    {
        // A colour per structure, from its id, so the same structure keeps it
        // between scans.
        const U32 h = static_cast<U32>(mix(SEED, c->mId.mData[0] | (c->mId.mData[1] << 8) | (c->mId.mData[2] << 16)));
        const LLColor4 color(0.3f + 0.7f * ((h & 255) / 255.f), 0.3f + 0.7f * (((h >> 8) & 255) / 255.f),
                             0.3f + 0.7f * (((h >> 16) & 255) / 255.f), skNavSwatchColor(SKNavSwatch::CLUSTER).mV[VALPHA]);
        geom.box(gAgent.getPosAgentFromGlobal(c->mMin), gAgent.getPosAgentFromGlobal(c->mMax), color);
    }
    if (geom.empty())
    {
        return;
    }

    SKNavOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        geom.draw();
    }
    SKNavOverlayGeom::unbindShader();
}
