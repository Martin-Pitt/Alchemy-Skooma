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

#include <set>

#include "llframetimer.h"

#include "sknavoverlay.h"
#include "sknavpalette.h"

#include "alobjectproperties.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llcharacter.h"
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
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llviewershadermgr.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "skcaptureexport.h"
#include "sknavmarkobjects.h"
#include "sknavfloors.h"
#include "sknavmesh.h"
#include "sknavrooms.h"
#include "skphysicsshape.h"
#include "threadpool.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace
{
    using namespace sknav;

    const F32 MOVED_EPSILON = 0.01f;
    const F32 MOVED_ANGLE = 2.f * DEG_TO_RAD;
    const F32 HISTORY_KEEP_SECONDS = 600.f;
    // A mover is told by its habits, not by one move: moves this close are one burst,
    const F64 BURST_GAP_SECONDS = 2.0;
    // and this many bursts in the window, or one burst lasting this long, make it a mover for the window.
    const size_t MOVER_BURSTS = 3;
    const F64 MOVER_WINDOW_SECONDS = 120.0;
    const F64 MOVER_CONTINUOUS_SECONDS = 3.0;

    // The share of a part's faces that can be seen through: a pane of glass
    // is solid to the sim and told apart from a door only here.
    F32 seeThrough(const LLViewerObject* object)
    {
        return skcapture::seeThroughShare(object);
    }

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
    // Join the pool now; a singleton destroyed at static teardown outlives the fiber machinery.
    if (mClusterWorkers)
    {
        mClusterWorkers->close();
        mClusterWorkers.reset();
    }
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
    static LLCachedControl<bool> enabled(gSavedSettings, "SKWorldCensusEnabled", true);
    static LLCachedControl<bool> always(gSavedSettings, "SKNavCensusAlways", false);
    static LLCachedControl<bool> show(gSavedSettings, "SKNavOverlayCensus", false);
    static LLCachedControl<bool> show_features(gSavedSettings, "SKNavOverlayFeatures", false);
    if (!enabled)
    {
        return false;
    }
    // The census overlays draw without the navmesh, so they bring the census themselves.
    return always || show || show_features || !mDemands.empty() || gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH);
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

const SKWorldCensus::LinksetFacts* SKWorldCensus::linksetFactsOf(const LLUUID& root) const
{
    const auto it = mLinksets.find(root);
    return it == mLinksets.end() ? nullptr : &it->second;
}

const SKWorldCensus::RootHistory* SKWorldCensus::historyOf(const LLUUID& root) const
{
    const auto it = mHistory.find(root);
    return it == mHistory.end() ? nullptr : &it->second;
}

F32 SKWorldCensus::landUnder(const LLVector3d& min, const LLVector3d& max)
{
    if (!LLWorld::instanceExists())
    {
        return -1e9f;
    }
    LLWorld* world = LLWorld::getInstance();
    return sknav::footprintMax(min, max, [world](const LLVector3d& p) { return static_cast<F32>(world->resolveLandHeightGlobal(p)); });
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
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - update");
    static LLCachedControl<bool> log(gSavedSettings, "SKNavCensusLog", false);
    const bool enabled = wanted() && LLWorld::instanceExists() && gAgent.getRegion();
    if (mClusterJob.valid())
    {
        mDiscardClusterJob = mDiscardClusterJob || !enabled || !sameRegions(mClusterRegions);
        if (mClusterJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            if (enabled && mRolesWanted) requestRoles();
            return;
        }
        auto done = mClusterJob.get();
        SKCensusSnapshotPtr retired;
        if (!mDiscardClusterJob)
        {
            // The stamp says the content changed: a scan that found everything as it was keeps the last one's.
            const bool same = mSnapshot && done->mHash == mSnapshot->mHash;
            done->mStamp = same ? mSnapshot->mStamp : ++mStamp;
            if (!same && log)
            {
                LL_INFOS("SKNav") << "census stamp " << done->mStamp << ": +" << done->mAdded.size() << " -" << done->mRemoved.size()
                                  << " ~" << done->mChanged.size() << " of " << done->mRecords.size() << " records" << LL_ENDL;
            }
            retired = std::move(mSnapshot);
            mSnapshot = std::move(done);
        }
        else
        {
            retired = std::move(done);
        }
        // Large tile maps must also be destroyed off-thread. Explicitly reset
        // the payload there even if the queue retains a copy of the callable.
        auto held = std::make_shared<SKCensusSnapshotPtr>(std::move(retired));
        LL::WorkQueue::getInstance("CensusClusters")->post([held]() { held->reset(); });
        mClusterRegions.clear();
        mDiscardClusterJob = false;
    }
    if (!enabled)
    {
        if (mScan.mActive)
        {
            abandonScan();
        }
        return;
    }

    static LLCachedControl<F32> budget(gSavedSettings, "SKNavCensusBudgetMS", 3.f);
    constexpr F64 SLOW_SCAN_INTERVAL_S = 1.0;
    constexpr F32 SLOW_SLICE_MS = 1.f;

    if (!mScan.mActive)
    {
        // Slow cadence: the next scan waits until a second after the last began.
        if (mSlow && LLFrameTimer::getUptimeSeconds().value() - mLastScanBegun < SLOW_SCAN_INTERVAL_S)
        {
            if (mRolesWanted)
            {
                requestRoles();
            }
            return;
        }
        beginScan();
    }
    else if (!sameRegions(mScan.mRegions))
    {
        abandonScan();
        beginScan();
    }
    const F32 slice = llmax(0.1f, static_cast<F32>(budget));
    continueScan(mSlow ? llmin(slice, SLOW_SLICE_MS) : slice);

    if (mRolesWanted)
    {
        requestRoles();
    }

    if (log && mSnapshot && gFrameTimeSeconds - mLastLog > 5.f)
    {
        mLastLog = gFrameTimeSeconds;
        LL_INFOS("SKNav") << statsLine() << LL_ENDL;
    }
}

void SKWorldCensus::beginScan()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - begin scan");
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
    mScan.mFeatureParts.clear();
    gatherCached();
    mScan.mElapsedMs = 0.f;
    mScan.mActive = true;
    mLastScanBegun = LLFrameTimer::getUptimeSeconds().value();
    ++mScansBegun;
}

void SKWorldCensus::abandonScan()
{
    mDiscardClusterJob = true;
    mScan.mPending.reset();
    mScan.mObjects.clear();
    mScan.mActive = false;
}

void SKWorldCensus::continueScan(F32 budget_ms)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - scan slice");
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
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - finish scan");
    static LLCachedControl<F32> gap(gSavedSettings, "SKNavClusterGap", 0.5f);
    std::shared_ptr<SKCensusSnapshot> done = mScan.mPending;
    mScan.mActive = false;
    mScan.mPending.reset();

    // Clusters stop where the region meshes' void does: a kilometre megaprim
    // would otherwise tile kilometres and join whatever it overhangs.
    static LLCachedControl<F32> void_margin(gSavedSettings, "SKNavVoidMargin", 256.f);
    std::vector<sknav::RegionSquare> squares;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        squares.push_back({ region->getOriginGlobal(), region->getWidth() });
    }
    const auto window = sknav::regionWindow(squares, void_margin);
    LLTimer timer;
    done->mStats.mScanMs = mScan.mElapsedMs;
    done->mStats.mRecords = static_cast<U32>(done->mRecords.size());
    done->mStats.mRolesKnown = static_cast<U32>(mRoles.size());
    timer.reset();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - finish crew");
        gatherCrew();
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - finish classify");
        classify(*done);
    }
    done->mStats.mFeatureMs = timer.getElapsedTimeF32() * 1000.f;

    // Parts and roots not seen for a minute leave the caches, and a root's
    // history ten minutes out of sight unless it is a vehicle or a working
    // car; a seat is cheap to find again.
    for (auto it = mHistory.begin(); it != mHistory.end();)
    {
        const bool keep = it->second.mVehicle || it->second.mWorkingElevator
            || gFrameTimeSeconds - it->second.mLastSeen <= HISTORY_KEEP_SECONDS;
        it = keep ? std::next(it) : mHistory.erase(it);
    }
    for (auto it = mLeaves.begin(); it != mLeaves.end();)
    {
        it = (gFrameTimeSeconds - it->second.mLastSeen > HISTORY_KEEP_SECONDS) ? mLeaves.erase(it) : std::next(it);
    }
    for (auto it = mDoorways.begin(); it != mDoorways.end();)
    {
        it = mLeaves.count(it->first) ? std::next(it) : mDoorways.erase(it);
    }
    for (auto it = mParts.begin(); it != mParts.end();)
    {
        it = (gFrameTimeSeconds - it->second.mLastSeen > 60.f) ? mParts.erase(it) : std::next(it);
    }
    for (auto it = mUnknownSince.begin(); it != mUnknownSince.end();)
    {
        it = mParts.count(it->first) ? std::next(it) : mUnknownSince.erase(it);
    }
    for (auto it = mRest.begin(); it != mRest.end();)
    {
        it = (gFrameTimeSeconds - it->second.mLastSeen > 60.f) ? mRest.erase(it) : std::next(it);
    }
    if (!mClusterWorkers)
    {
        mClusterWorkers = std::make_unique<LL::ThreadPool>("CensusClusters", 1);
        mClusterWorkers->start();
    }
    // All viewer-derived data is now detached. The worker owns this unpublished
    // snapshot and never accesses the census, settings or live viewer objects.
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<SKCensusSnapshot>()>>(
        [done, prev = mSnapshot, cluster_gap = static_cast<F32>(gap), window]() mutable
        {
            {
                // The delta is against the snapshot published when this scan finished: one job is in flight at a time.
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - delta worker");
                done->mHash = sknav::censusHash(done->mRecords, done->mFeatures, done->mDoors, window, cluster_gap);
                done->mByPart = sknav::recordsByPart(done->mRecords);
                static const sknav::RecordsByPart none;
                sknav::diffRecords(prev ? prev->mByPart : none, done->mByPart, done->mAdded, done->mRemoved, done->mChanged);
                prev.reset();
            }
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - cluster worker");
            LLTimer timer;
            done->mClusters = cluster(done->mRecords, cluster_gap, sknav::TILE_M, window);
            done->mStats.mClusterMs = timer.getElapsedTimeF32() * 1000.f;
            return done;
        });
    auto future = task->get_future();
    if (!LL::WorkQueue::getInstance("CensusClusters")->tryPost([task]() { (*task)(); }))
    {
        // A refused post would drop the finished scan, so it is clustered here instead.
        LL_WARNS("SKNav") << "Census cluster queue refused the scan; clustering on this thread" << LL_ENDL;
        (*task)();
    }
    mClusterJob = std::move(future);
    mClusterRegions = mScan.mRegions;
    mDiscardClusterJob = false;
}

bool SKWorldCensus::classifyRest(LLViewerObject* root, const LLVector3d& pos, const LLQuaternion& rot)
{
    static LLCachedControl<F32> settle(gSavedSettings, "SKNavSettleSeconds", 4.f);
    // The baseline moves only when the root is seen to have moved, so a slow
    // drift adds up to a move instead of hiding under the epsilon each frame.
    // A door swinging on its hinge by steps turns without going anywhere, so
    // the turn counts as a move too.
    Rest& rest = mRest[root->getID()];
    const F32 turned = 2.f * std::acos(llclamp(std::fabs(dot(rest.mRotation, rot)), 0.f, 1.f));
    const F64 shifted = rest.mKnown ? dist_vec(rest.mPosition, pos) : 0.0;
    if (!rest.mKnown || shifted > MOVED_EPSILON || turned > MOVED_ANGLE)
    {
        if (rest.mKnown)
        {
            const F64 now = gFrameTimeSeconds;
            if (rest.mMovedAt < 0.f || now - rest.mMovedAt > BURST_GAP_SECONDS)
            {
                rest.mBurstStart = now;
                rest.mBursts.push_back(now);
            }
            while (!rest.mBursts.empty() && now - rest.mBursts.front() > MOVER_WINDOW_SECONDS)
            {
                rest.mBursts.erase(rest.mBursts.begin());
            }
            if (rest.mBursts.size() >= MOVER_BURSTS || now - rest.mBurstStart >= MOVER_CONTINUOUS_SECONDS)
            {
                rest.mMoverUntil = now + MOVER_WINDOW_SECONDS;
            }
            rest.mMovedAt = now;
            RootHistory& history = mHistory[root->getID()];
            history.mEverMoved = true;
            history.mTravel += static_cast<F32>(shifted);
        }
        rest.mPosition = pos;
        rest.mRotation = rot;
    }
    rest.mLastSeen = gFrameTimeSeconds;
    rest.mKnown = true;
    if (const auto it = mHistory.find(root->getID()); it != mHistory.end())
    {
        it->second.mLastSeen = gFrameTimeSeconds;
    }
    // A one-off move, an edit or a rez, is landscape at once where it now stands; only a mover waits out the settle.
    return gFrameTimeSeconds < rest.mMoverUntil && rest.mMovedAt >= 0.f && gFrameTimeSeconds - rest.mMovedAt < settle;
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
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - visit");
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

    // <SK:Nexii> navmesh annotation bodies are survey instruments, not world
    // content: a mark's beam would otherwise be built into the very mesh it
    // measures, and the handles would count as surrounds. The annotator
    // owns them and drops them with the session.
    if (SKNavMarkObjects::instanceExists() && SKNavMarkObjects::getInstance()->isMarkProxy(object))
    {
        return;
    }
    // </SK:Nexii>

    // Movers and things that are not shapes.
    if (root->flagUsePhysics() || root->flagCharacter() || root->flagTemporaryOnRez())
    {
        return;
    }
    // Nothing collides with a volume detect linkset: recorded visible only, as phantom is below.
    bool collides = !root->flagVolumeDetect();

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
    // A door leaf whose doorway is established is recorded shut wherever it
    // swings, so opening a door re-signs no tile and refloods no air.
    bool held_shut = false;
    if (const auto lit = mLeaves.find(object->getID()); lit != mLeaves.end())
    {
        const U64 handle = object->getRegion()->getHandle();
        mLeafTransforms[object->getID()] = { sim_pos, sim_rot, handle };
        const auto dit = mDoorways.find(object->getID());
        FeaturePart doorway;
        if (dit != mDoorways.end() && dit->second.mRegion == handle && establishedDoorway(lit->second, doorway))
        {
            sim_pos = dit->second.mPos;
            sim_rot = dit->second.mRot;
            held_shut = true;
        }
    }
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

    const NavRole navRole = roleOf(root->getID());
    if (root->flagObjectPermanent() && navRole == NavRole::UNKNOWN)
    {
        mRolesWanted = true;
    }

    // A part under the agent's erosion radius in every world axis cannot
    // leave a surface an agent could stand on, and erosion takes back any
    // dent it makes: nothing it does to a tile survives to the built mesh,
    // so it never becomes a record and never re-signs the tile it hangs
    // over. Small parts appear in clouds as content streams in, and each
    // one arriving was a whole tile rebuild for nothing. An exclusion volume
    // keeps: the sim takes the navmesh out of it whatever its size.
    static LLCachedControl<F32> min_shape(gSavedSettings, "SKNavMinShape", 0.3f);
    if (navRole != NavRole::EXCLUSION_VOLUME
        && extent.mV[VX] < min_shape && extent.mV[VY] < min_shape && extent.mV[VZ] < min_shape)
    {
        snap.mStats.mTiny++;
        return;
    }

    // The sim's rotation, not the rendered one: llTargetOmega spin is not motion.
    const bool moving = classifyRest(root, root->getRegion()->getOriginGlobal() + LLVector3d(root->getPositionRegion()),
                                     root->getRotationSim());
    // The kind the feature rules gave the part at the last scan. A vehicle is
    // out of every class parked or driving, so its rest state says nothing.
    PartFeature feature;
    if (const auto fit = mPartFeatures.find(object->getID()); fit != mPartFeatures.end())
    {
        feature = fit->second;
    }
    const bool excluded = (feature.mExclude & EXCLUDE_ALL) == EXCLUDE_ALL;
    const bool dynamic = moving && !excluded && !held_shut;

    // Phantom is visible only unless the sim says it is an exclusion volume; a
    // surround stays in whatever its flags, as its rendered shape. A part with
    // no physics shape is visible only as well: the overview's cover test asks
    // whether what is seen collides, and nav never sees such a record.
    const bool phantom = root->flagPhantom();
    const LLVector3d span(extent);
    const bool surround = isSurround(root, pos - span, pos + span);
    if (phantom && navRole != NavRole::EXCLUSION_VOLUME && !surround)
    {
        collides = false;
    }

    SKPhysicsShape shape;
    const bool hasPhysics = skPhysicsShapeClassify(volume, shape);
    if (!hasPhysics && !surround)
    {
        collides = false;
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
    // A newly classified leaf must re-record, or its tile never hears of it.
    sig = mix(sig, static_cast<U64>(feature.mKind));
    sig = mix(sig, feature.mExclude);
    sig = mix(sig, object->isPhysicsShapeUnknown());
    if (!collides)
    {
        sig = mix(sig, 0x76697369626c65ull); // volume detect turning on or off re-signs the part
    }
    // No shape depends on the loaded LOD: a surround is built from the top LOD
    // (skCensusRenderGeometry), the rest from a fixed detail or the
    // decomposition. Mixing the LOD in re-signs records as the camera moves and
    // rebuilds their tiles for nothing, which is what leaves tiles in flight
    // while you walk; the display volume's face count and the hull the
    // physics-shapes overlay caches on it are the same trap.
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
            // The simulator's triangles arriving replace the physics mesh once, and stay held after.
            PartCache& held = mParts[object->getID()];
            if (held.mSimMesh != sculpt || object->getPhysicsShapeType() != LLViewerObject::PHYSICS_SHAPE_PRIM)
            {
                held.mSim.reset();
                held.mSimMesh = sculpt;
            }
            if (!held.mSim && collides && (!surround || (hasPhysics && !phantom)))
            {
                held.mSim = skPhysicsShapeSimTriangles(volume);
            }
            sig = mix(sig, held.mSim ? 1 + held.mSim->mTriangles.size() : 0);
        }
        // A sculpt's texture arriving replaces the stretched placeholder, so
        // its level has to move the signature or the placeholder never leaves.
        else if (volume->isSculpted())
        {
            sig = mix(sig, static_cast<U64>(v->getSculptLevel() + 3));
        }
    }

    // A record every class leaves out shapes nothing and stays out of every
    // cluster and tile, so it is signed by what it is and not where: a
    // vehicle driving about keeps its record for the mark and the overlay.
    U64 record_sig = sig;
    if (excluded)
    {
        record_sig = mix(SEED, *reinterpret_cast<const U64*>(object->getID().mData));
        record_sig = mix(record_sig, *reinterpret_cast<const U64*>(object->getID().mData + 8));
        record_sig = mix(record_sig, static_cast<U64>(feature.mKind));
        record_sig = mix(record_sig, feature.mExclude);
    }

    PartCache& cache = mParts[object->getID()];
    cache.mLastSeen = gFrameTimeSeconds;
    const bool fresh = cache.mRecord && cache.mSignature == sig && cache.mRecord->mDynamic == (collides && dynamic);
    RecordPtr rec;
    if (fresh)
    {
        rec = cache.mRecord;
        snap.mStats.mCached++;
    }
    else if (!collides)
    {
        // Visible only: its box for the overview's cover test, never a shape nav builds with.
        auto box = std::make_shared<Record>();
        box->mRoot = root->getID();
        box->mPart = object->getID();
        box->mProvenance = Provenance::RENDER;
        box->mNavRole = navRole;
        box->mShape = Shape::BOX;
        box->mCollides = false;
        box->mPhysicsKnown = !object->isPhysicsShapeUnknown();
        box->mSignature = sig;
        box->mOrigin = object->getRegion()->getOriginGlobal();
        box->mCenter = sim_pos;
        for (S32 k = 0; k < 3; ++k)
        {
            box->mAxes[k] = axis[k];
        }
        box->mHalf = half;
        box->mMin = box->mOrigin + LLVector3d(sim_pos - extent);
        box->mMax = box->mOrigin + LLVector3d(sim_pos + extent);
        rec = box;
        cache.mRecord = rec;
        cache.mPhysics.reset();
        cache.mSignature = sig;
    }
    else
    {
        rec = record(volume, root, surround ? Role::SURROUND : Role::NAV, dynamic, record_sig, sim_pos, sim_rot, feature.mKind,
                     feature.mExclude, cache.mSim);
        if (!rec)
        {
            return;
        }
        cache.mRecord = rec;
        cache.mPhysics.reset();
        cache.mSignature = sig;
    }
    take(snap, rec);
    snap.mStats.mVisibleOnly += rec->mCollides ? 0 : 1;
    // Only a mesh has its type asked for above; a prim's stays unknown until someone edits it.
    if (volume->isMesh())
    {
        snap.mStats.mPhysicsMeshes++;
        if (object->isPhysicsShapeUnknown())
        {
            snap.mStats.mPhysicsUnknown++;
            mUnknownSince.try_emplace(object->getID(), gFrameTimeSeconds);
        }
        else if (const auto since = mUnknownSince.find(object->getID()); since != mUnknownSince.end())
        {
            const F64 waited = gFrameTimeSeconds - since->second;
            mKnownAfterCount++;
            mKnownAfterSumS += waited;
            mKnownAfterMaxS = std::max(mKnownAfterMaxS, waited);
            mUnknownSince.erase(since);
        }
    }

    if (collides && !surround)
    {
        FeaturePart fp;
        fp.mPart = object->getID();
        fp.mRoot = root->getID();
        fp.mCenter = object->getRegion()->getOriginGlobal() + LLVector3d(sim_pos);
        fp.mMin = fp.mCenter - span;
        fp.mMax = fp.mCenter + span;
        for (S32 k = 0; k < 3; ++k)
        {
            fp.mAxes[k] = axis[k];
        }
        fp.mHalf = half;
        if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(object->getID()))
        {
            fp.mName = props->mName;
        }
        fp.mScripted = object->flagScripted();
        fp.mTouch = object->flagHandleTouch();
        fp.mSeeThrough = seeThrough(object);
        mScan.mFeatureParts.push_back(std::move(fp));
    }

    // A surround's rendered shape is what weather lands on, but where the
    // thing reaches over land it collides like anything else: a 128 m cube
    // rezzed across the region edge is solid over the strip of ground it
    // covers. Its physics shape comes in beside the rendered one, and is the
    // record the fill makes solid.
    if (collides && surround && hasPhysics && !phantom)
    {
        if (!fresh || !cache.mPhysics)
        {
            cache.mPhysics = record(volume, root, Role::NAV, dynamic, record_sig, sim_pos, sim_rot, feature.mKind, feature.mExclude, cache.mSim);
        }
        if (cache.mPhysics)
        {
            take(snap, cache.mPhysics);
        }
    }
}

void SKWorldCensus::take(SKCensusSnapshot& snap, const sknav::RecordPtr& rec)
{
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
        made->mSolid = true;
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
                                       const LLVector3& pos, const LLQuaternion& rot, sknav::FeatureKind feature, U8 exclude,
                                       const std::shared_ptr<const SKPhysicsHavokShape>& sim)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - shape");
    // An unloaded sculpt is a placeholder sphere stretched over its whole
    // scale; a megasculpty's would floor the region, so it waits for level 0.
    if (volume->isSculpted() && !volume->isMesh() && (!volume->getVolume() || volume->getVolume()->getSculptLevel() != 0))
    {
        return nullptr;
    }
    SKShapeGeometry geom;
    bool ok = false;
    if (role == Role::SURROUND)
    {
        ok = skCensusRenderGeometry(volume, geom);
    }
    else
    {
        SKPhysicsShape shape;
        ok = skPhysicsShapeClassify(volume, shape) && skPhysicsShapeGeometry(volume, shape, geom, sim);
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
    rec->mSolid = geom.mSolid;
    rec->mDynamic = dynamic;
    rec->mFeature = feature;
    rec->mExclude = exclude;
    rec->mSignature = signature;
    rec->mPhysicsKnown = !volume->isPhysicsShapeUnknown();
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
        }
        for (const LLVector3& q : *tris)
        {
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

void SKWorldCensus::gatherCrew()
{
    // Avatars never pass the census gate, so crew are read here, once a scan:
    // whoever sits, on the linkset root of what they sit on.
    for (auto& entry : mHistory)
    {
        entry.second.mCrew.clear();
    }
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead() || !avatar->isSitting() || !avatar->getParent())
        {
            continue;
        }
        const LLViewerObject* seat = static_cast<const LLViewerObject*>(avatar->getParent());
        const LLViewerObject* root = seat->getRootEdit();
        if (!root || root->isAvatar())
        {
            continue;
        }
        RootHistory& history = mHistory[root->getID()];
        history.mEverCrewed = true;
        history.mLastSeen = gFrameTimeSeconds;
        history.mCrew.push_back(avatar->getID());
    }
}

void SKWorldCensus::classify(SKCensusSnapshot& snap)
{
    FeatureParams params;
    params.mClassHeight = isAgentAvatarValid() ? llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f) : 2.f;

    // Every root with a part in the scan, and every root that has had crew:
    // a physical vehicle never passes the gate, and is still one.
    std::vector<LLUUID> ids;
    for (const FeaturePart& p : mScan.mFeatureParts)
    {
        // A linkset's parts come together, so most repeats are dropped before the sort.
        if (ids.empty() || ids.back() != p.mRoot)
        {
            ids.push_back(p.mRoot);
        }
    }
    for (const auto& entry : mHistory)
    {
        if (entry.second.mEverCrewed)
        {
            ids.push_back(entry.first);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::vector<FeatureRoot> roots;
    roots.reserve(ids.size());
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - classify roots");
        for (const LLUUID& id : ids)
        {
            LLViewerObject* object = gObjectList.findObject(id);
            if (!object || object->isDead())
            {
                continue;
            }
            FeatureRoot r;
            r.mRoot = id;
            // The linkset reply first, the property cache where the reply has
            // nothing; a name neither has is simply not known.
            if (const LinksetFacts* facts = linksetFactsOf(id))
            {
                r.mName = facts->mName;
                r.mDescription = facts->mDescription;
                if (facts->mScriptedKnown)
                {
                    r.mScripted = facts->mScripted ? TriState::YES : TriState::NO;
                }
            }
            if (r.mName.empty() || r.mDescription.empty())
            {
                if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(id))
                {
                    if (r.mName.empty())
                    {
                        r.mName = props->mName;
                    }
                    if (r.mDescription.empty())
                    {
                        r.mDescription = props->mDescription;
                    }
                }
            }
            if (r.mScripted == TriState::UNKNOWN && object->flagScripted())
            {
                r.mScripted = TriState::YES;
            }
            r.mHover = object->mHudText;
            r.mPhysical = object->flagUsePhysics();
            if (const auto h = mHistory.find(id); h != mHistory.end())
            {
                RootHistory& history = h->second;
                r.mEverMoved = history.mEverMoved;
                r.mTravel = history.mTravel;
                r.mEverCrewed = history.mEverCrewed;
                r.mWorkingElevator = history.mWorkingElevator;
                // A physical root never passes the gate, so its travel stays
                // nil; latched here it stays a vehicle once parked non-physical.
                if (r.mEverCrewed && (r.mPhysical || r.mTravel > params.mVehicleTravel))
                {
                    history.mVehicle = true;
                }
                r.mEverVehicle = history.mVehicle;
            }
            if (r.mEverCrewed && object->getRegion())
            {
                // The linkset's own box, and how far its bottom is off the floor.
                LLVector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
                std::vector<LLViewerObject*> prims { object };
                for (const LLPointer<LLViewerObject>& child : object->getChildren())
                {
                    if (!child->isAvatar())
                    {
                        prims.push_back(child.get());
                    }
                }
                for (LLViewerObject* prim : prims)
                {
                    const LLBBox box = prim->getBoundingBoxAgent();
                    update_min_max(lo, hi, box.getMinAgent());
                    update_min_max(lo, hi, box.getMaxAgent());
                }
                r.mHasBounds = true;
                r.mMin = gAgent.getPosGlobalFromAgent(lo);
                r.mMax = gAgent.getPosGlobalFromAgent(hi);
                LLVector3d foot = (r.mMin + r.mMax) * 0.5;
                foot.mdV[VZ] = r.mMin.mdV[VZ];
                F32 floor = landUnder(r.mMin, r.mMax);
                // The built floor is read at the foot only: the navmesh's when it has polys, else the air's.
                F32 built = 0.f;
                const F32 reach = static_cast<F32>(foot.mdV[VZ]) + 0.5f;
                if (SKNavMesh::instanceExists())
                {
                    const SKNavMesh& nav = SKNavMesh::instance();
                    if (SKNavMesh::polysEnabled())
                    {
                        if (nav.floorUnder(foot, AgentClass::AVATAR, built) && built <= reach)
                        {
                            floor = llmax(floor, built);
                        }
                    }
                    else if (const auto view = nav.view())
                    {
                        std::vector<sknav::Enclosure> gaps;
                        view->gapsAt(foot, [&gaps](const sknav::Enclosure& e) { gaps.push_back(e); });
                        if (sknav::pickFloor(gaps, -1e9f, reach, built))
                        {
                            floor = llmax(floor, built);
                        }
                    }
                }
                r.mClearance = static_cast<F32>(foot.mdV[VZ]) - floor;
            }
            roots.push_back(std::move(r));
        }
    }

    // Only a door-shaped part with something to say for it is worth the rays.
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - classify face cover");
        const sknav::RecordIndex index(snap.mRecords);
        for (FeaturePart& p : mScan.mFeatureParts)
        {
            if ((p.mScripted || p.mTouch || !p.mName.empty()) && doorShaped(p, params))
            {
                p.mFaceCover = mFaceCover.measure(p, index, params);
            }
        }
        mFaceCover.prune();
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - classify features");
        snap.mFeatures = classifyFeatures(mScan.mFeatureParts, roots, params);
    }
    snap.mStats.mFeatures = static_cast<U32>(snap.mFeatures.size());
    // A leaf is watched from the scan it is first a door, and stays watched
    // while it swings, when the rules may not call it one.
    std::unordered_set<LLUUID> leaves;
    for (const Feature& f : snap.mFeatures)
    {
        if (f.mKind == FeatureKind::DOOR)
        {
            leaves.insert(f.mParts.begin(), f.mParts.end());
        }
    }
    for (const FeaturePart& p : mScan.mFeatureParts)
    {
        if (leaves.count(p.mPart) || mLeaves.count(p.mPart))
        {
            LeafHistory& history = mLeaves[p.mPart];
            observeLeaf(history, p, static_cast<F32>(gFrameTimeSeconds.value()));
            if (const auto tit = mLeafTransforms.find(p.mPart); tit != mLeafTransforms.end() && atEstablishedDoorway(history))
            {
                mDoorways[p.mPart] = tit->second;
            }
        }
    }
    mLeafTransforms.clear();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skcensus - classify doors");
        snap.mDoors = buildDoorEntities(snap.mFeatures, mScan.mFeatureParts, roots, &mLeaves);
    }

    // What the next scan records each part with, and what stays with a root
    // for the session: a vehicle stays one, a working car stays working.
    mPartFeatures.clear();
    for (const Feature& f : snap.mFeatures)
    {
        const U8 exclude = excludeFor(f.mKind);
        for (const LLUUID& part : f.mParts)
        {
            mPartFeatures[part] = { f.mKind, exclude };
        }
        if (f.mKind == FeatureKind::VEHICLE || f.mKind == FeatureKind::SEAT
            || (f.mKind == FeatureKind::ELEVATOR && f.mWorking))
        {
            for (const LLUUID& root : f.mRoots)
            {
                RootHistory& history = mHistory[root];
                history.mKind = f.mKind;
                history.mWorkingElevator = history.mWorkingElevator || f.mKind == FeatureKind::ELEVATOR;
            }
        }
    }
    mScan.mFeatureParts.clear();
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
        mLinksets.clear();
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
        // The reply is the one free source of a linkset's name and whether
        // its root is scripted, which is what the feature rules read.
        LinksetFacts& facts = mLinksets[linkset->getUUID()];
        facts.mName = linkset->getName();
        facts.mDescription = linkset->getDescription();
        facts.mScriptedKnown = linkset->hasIsScripted();
        facts.mScripted = facts.mScriptedKnown && linkset->isScripted();
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
    return llformat("census #%llu: %u objects, %u records (%u cached, %u dynamic, %u surround, %u placeholders, %u from object cache, %u tiny, %u visible-only), %u tris, %u roles, %u features; %zu clusters, largest %zu; scan %.1f ms, cluster %.1f ms, features %.1f ms",
                    static_cast<unsigned long long>(mSnapshot->mStamp), s.mObjectsSeen, s.mRecords, s.mCached, s.mDynamic, s.mSurround,
                    s.mPlaceholders, s.mFromObjectCache, s.mTiny, s.mVisibleOnly, s.mTriangles, s.mRolesKnown, s.mFeatures, mSnapshot->mClusters.mClusters.size(), largest, s.mScanMs, s.mClusterMs,
                    s.mFeatureMs) +
           llformat("; physics unknown %u / %u meshes, known after mean %.1f s, max %.1f s over %u parts", s.mPhysicsUnknown,
                    s.mPhysicsMeshes, mKnownAfterCount ? mKnownAfterSumS / mKnownAfterCount : 0.0, mKnownAfterMaxS, mKnownAfterCount);
}

void SKWorldCensus::renderDebug()
{
    static LLCachedControl<bool> show(gSavedSettings, "SKNavOverlayCensus", false);
    static LLCachedControl<bool> show_features(gSavedSettings, "SKNavOverlayFeatures", false);
    static LLCachedControl<bool> show_clusters(gSavedSettings, "SKNavOverlayClusters", false);
    SKCensusSnapshotPtr snap = mSnapshot;
    if (!show_clusters || !snap)
    {
        for (auto& label : mClusterLabels)
        {
            label.second->markDead();
        }
        mClusterLabels.clear();
    }
    if ((!show && !show_features && !show_clusters) || !snap)
    {
        return;
    }

    // The census covers the whole sim, but the shapes only draw around the
    // camera, falling away over the last fifth of the range like the mesh
    // does.
    static LLCachedControl<F32> range(gSavedSettings, "SKNavOverlayRange", 96.f);
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 reach = range;
    auto within = [&](const LLVector3d& p) {
        return dist_vec_squared2D(p, camera) <= reach * reach;
    };

    SKNavOverlayGeom geom;
    for (const RecordPtr& r : snap->mRecords)
    {
        if (!show)
        {
            break;
        }
        if (!within((r->mMin + r->mMax) * 0.5))
        {
            continue;
        }
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
                case Provenance::SIM: swatch = SKNavSwatch::SHAPE_EXACT; break;
            }
        }
        geom.box(gAgent.getPosAgentFromGlobal(r->mMin), gAgent.getPosAgentFromGlobal(r->mMax), skNavSwatchColor(swatch));
    }
    for (const ClusterPtr& c : snap->mClusters.mClusters)
    {
        if (!show)
        {
            break;
        }
        if (!within((c->mMin + c->mMax) * 0.5))
        {
            continue;
        }
        // A colour per structure, from its id, so the same structure keeps it
        // between scans.
        const U32 h = static_cast<U32>(mix(SEED, c->mId.mData[0] | (c->mId.mData[1] << 8) | (c->mId.mData[2] << 16)));
        const LLColor4 color(0.3f + 0.7f * ((h & 255) / 255.f), 0.3f + 0.7f * (((h >> 8) & 255) / 255.f),
                             0.3f + 0.7f * (((h >> 16) & 255) / 255.f), skNavSwatchColor(SKNavSwatch::CLUSTER).mV[VALPHA]);
        geom.box(gAgent.getPosAgentFromGlobal(c->mMin), gAgent.getPosAgentFromGlobal(c->mMax), color);
    }
    // What the feature rules recognised, as boxes, and a tick at each floor
    // a span can be boarded or left at. Seats are a label, not a glyph.
    for (const Feature& f : snap->mFeatures)
    {
        if (!show_features || !within((f.mMin + f.mMax) * 0.5))
        {
            continue;
        }
        SKNavSwatch swatch;
        switch (f.mKind)
        {
            case FeatureKind::DOOR: swatch = SKNavSwatch::FEATURE_DOOR; break;
            case FeatureKind::LADDER: swatch = SKNavSwatch::FEATURE_LADDER; break;
            case FeatureKind::ELEVATOR: swatch = SKNavSwatch::FEATURE_SHAFT; break;
            case FeatureKind::VEHICLE: swatch = SKNavSwatch::FEATURE_VEHICLE; break;
            default: continue;
        }
        const LLColor4 color = skNavSwatchColor(swatch);
        geom.box(gAgent.getPosAgentFromGlobal(f.mMin), gAgent.getPosAgentFromGlobal(f.mMax), color);
        if (f.mKind != FeatureKind::LADDER && f.mKind != FeatureKind::ELEVATOR)
        {
            continue;
        }
        if (const SKNavMesh::FeatureSpan* span = SKNavMesh::instance().featureSpan(SKNavMesh::featureId(f)))
        {
            for (const LLVector3d& exit : span->mExits[static_cast<S32>(AgentClass::AVATAR)])
            {
                const LLVector3 p = gAgent.getPosAgentFromGlobal(exit) + LLVector3(0.f, 0.f, 0.05f);
                geom.line(p - LLVector3(0.3f, 0.f, 0.f), p + LLVector3(0.3f, 0.f, 0.f), color);
                geom.line(p - LLVector3(0.f, 0.3f, 0.f), p + LLVector3(0.f, 0.3f, 0.f), color);
            }
        }
    }
    if (show_clusters)
    {
        renderClusters(*snap, geom, camera, reach);
    }
    if (geom.empty())
    {
        return;
    }
    geom.setFade(gAgent.getPosAgentFromGlobal(camera), static_cast<F32>(reach), static_cast<F32>(reach * 0.8));

    SKNavOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        geom.draw();
    }
    SKNavOverlayGeom::unbindShader();
}

bool SKWorldCensus::grounded(const LLVector3d& min, const LLVector3d& max)
{
    const F64 GROUND_BASE_M = 4.0;
    return min.mdV[VZ] <= landUnder(min, max) + GROUND_BASE_M;
}

void SKWorldCensus::renderClusters(const SKCensusSnapshot& snap, SKNavOverlayGeom& geom, const LLVector3d& camera, F64 reach)
{
    LL_PROFILE_ZONE_SCOPED;
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    const bool relabel = now - mClusterLabelsAt >= 0.5;
    if (relabel)
    {
        mClusterLabelsAt = now;
    }
    std::set<LLUUID> labelled;
    for (const ClusterPtr& c : snap.mClusters.mClusters)
    {
        if (dist_vec_squared2D((c->mMin + c->mMax) * 0.5, camera) > reach * reach)
        {
            continue;
        }
        const bool selected = c->mId == mSelectedCluster;
        const U32 h = static_cast<U32>(mix(SEED, c->mId.mData[0] | (c->mId.mData[1] << 8) | (c->mId.mData[2] << 16)));
        LLColor4 color(0.3f + 0.7f * ((h & 255) / 255.f), 0.3f + 0.7f * (((h >> 8) & 255) / 255.f),
                       0.3f + 0.7f * (((h >> 16) & 255) / 255.f), 0.9f);
        if (selected)
        {
            color = LLColor4::white;
        }
        geom.box(gAgent.getPosAgentFromGlobal(c->mMin), gAgent.getPosAgentFromGlobal(c->mMax), color);
        // Its parts in its colour, fainter, so neighbouring structures read apart.
        LLColor4 part = color;
        part.mV[VALPHA] = selected ? 0.8f : 0.45f;
        std::set<LLUUID> roots;
        for (const RecordPtr& r : c->mRecords)
        {
            geom.box(gAgent.getPosAgentFromGlobal(r->mMin), gAgent.getPosAgentFromGlobal(r->mMax), part);
            roots.insert(r->mRoot.notNull() ? r->mRoot : r->mPart);
        }
        labelled.insert(c->mId);
        LLPointer<LLHUDText>& label = mClusterLabels[c->mId];
        const bool fresh = label.isNull();
        if (fresh)
        {
            label = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            label->setDoFade(false);
            label->setZCompare(false);
        }
        if (relabel || fresh)
        {
            label->setPositionGlobal(LLVector3d((c->mMin.mdV[VX] + c->mMax.mdV[VX]) * 0.5, (c->mMin.mdV[VY] + c->mMax.mdV[VY]) * 0.5,
                                                c->mMax.mdV[VZ] + 0.5));
            label->setColor(color);
            label->setString(llformat("%s, %zu linkset%s", grounded(c->mMin, c->mMax) ? "grounded" : "floating", roots.size(),
                                      roots.size() == 1 ? "" : "s"));
        }
    }
    for (auto it = mClusterLabels.begin(); it != mClusterLabels.end();)
    {
        if (!labelled.count(it->first))
        {
            it->second->markDead();
            it = mClusterLabels.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
