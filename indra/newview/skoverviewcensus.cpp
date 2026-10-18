/**
 * @file skoverviewcensus.cpp
 * @brief The census's physical shapes as vertex buffers, a range per map class, for the overview.
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

#include "skoverviewcensus.h"

#include "llframetimer.h"
#include "llimagegl.h"
#include "llmaterial.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "llvovolume.h"
#include "skphysicsshape.h"
#include "sknavbuild.h"
#include "sknavtypes.h"
#include "skworldcensus.h"
#include "lltextureentry.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llgltfmaterial.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

namespace skoverview
{
    const size_t MAX_BATCH_VERTICES = 16u * 1024u * 1024u;

    U8 codeOf(CensusClass cls)
    {
        static const U8 codes[NUM_CENSUS_CLASSES] = { CODE_FLOOR, CODE_WALL,   CODE_COVER,       CODE_RAMP,
                                                      CODE_DOOR,  CODE_LADDER, CODE_VEHICLE,     CODE_PLACEHOLDER,
                                                      CODE_NONE };
        return cls < NUM_CENSUS_CLASSES ? codes[cls] : CODE_NONE;
    }

    bool keepRecord(const sknav::Record& rec, CensusClass& fixed)
    {
        fixed = NUM_CENSUS_CLASSES;
        if (rec.mRole != sknav::Role::NAV || rec.mNavRole == sknav::NavRole::MATERIAL_VOLUME ||
            rec.mNavRole == sknav::NavRole::EXCLUSION_VOLUME)
        {
            return false;
        }
        switch (rec.mFeature)
        {
            case sknav::FeatureKind::DOOR: fixed = DOOR; return true;
            case sknav::FeatureKind::LADDER: fixed = LADDER; return true;
            case sknav::FeatureKind::VEHICLE:
            case sknav::FeatureKind::ELEVATOR: return false; // drawn live, at their current pose (skoverviewmovers.h)
            default: break;
        }
        if (rec.mExclude & sknav::EXCLUDE_AVATAR)
        {
            fixed = DOOR; // door leaves are the only parts left out of the avatar class alone
            return true;
        }
        if (rec.mProvenance == sknav::Provenance::BOX)
        {
            fixed = PLACEHOLDER; // only its bounds are known: its own code, so the debug view can tell it apart
        }
        // Movers' census poses are a scan old; the markers show who is where.
        return !rec.mDynamic && !(rec.mExclude & sknav::EXCLUDE_SOLID);
    }

    CensusClass classifyTriangle(F32 nz, F32 part_height)
    {
        const F32 a = std::fabs(nz);
        if (a >= FLOOR_NZ)
        {
            return FLOOR;
        }
        if (a >= WALL_NZ)
        {
            return RAMP;
        }
        return part_height <= COVER_TOP_M ? COVER : WALL;
    }
}

SKOverviewCensus::SKOverviewCensus() = default;

SKOverviewCensus::~SKOverviewCensus()
{
    release();
}

void SKOverviewCensus::release()
{
    mBatches.clear();
    mStamp = 0;
    mSignature = 0;
    mTriangles = 0;
}

namespace skoverview
{
    bool transparentTexture(const LLUUID& id)
    {
        static const LLUUID transparent("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");
        static const LLUUID invisiprim_a("38b86f85-2575-52a9-a531-23108d8da837");
        static const LLUUID invisiprim_b("e97cf410-8e61-7005-ec06-629eba4cd1fb");
        return id == transparent || id == invisiprim_a || id == invisiprim_b;
    }
}

namespace
{
    using skoverview::transparentTexture;

    enum class Seen : U8 { VISIBLE, INVISIBLE, UNKNOWN };

    // Whether the viewer draws a face so anyone sees it, from its own render inputs: colour alpha 0, an alpha-only
    // image (the invisiprim path), the default transparent or an invisiprim texture, an image whose alpha never rises
    // above the pick mask's threshold (32/255, below any mask cutoff), or a PBR base colour that is fully transparent.
    // UNKNOWN while the object, its face or its image is not loaded: never a verdict from data still on its way.
    Seen faceSeen(LLViewerObject* object, U8 i, std::string& detail)
    {
        const LLTextureEntry* te = object->getTE(i);
        if (!te)
        {
            detail = "no texture entry";
            return Seen::UNKNOWN;
        }
        const F32 alpha = te->getColor().mV[VALPHA];
        detail = llformat("alpha %.3f texture %s", alpha, te->getID().asString().c_str());
        const LLMaterial* material = te->getMaterialParams().get();
        if (material)
        {
            detail += llformat(" material %s alpha mode %d cutoff %d", te->getMaterialID().asString().c_str(),
                               (S32)material->getDiffuseAlphaMode(), (S32)material->getAlphaMaskCutoff());
        }
        if (const LLGLTFMaterial* pbr = te->getGLTFRenderMaterial())
        {
            detail += llformat(" PBR base alpha %.3f mode %d", pbr->mBaseColor.mV[VALPHA], (S32)pbr->mAlphaMode);
            if (pbr->mBaseColor.mV[VALPHA] <= 0.004f ||
                (pbr->mAlphaMode != LLGLTFMaterial::ALPHA_MODE_OPAQUE &&
                 transparentTexture(pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR])))
            {
                detail += " -> invisible (PBR)";
                return Seen::INVISIBLE;
            }
            detail += " -> visible (PBR)";
            return Seen::VISIBLE;
        }
        if (alpha <= 0.004f)
        {
            detail += " -> invisible (alpha 0)";
            return Seen::INVISIBLE;
        }
        if (transparentTexture(te->getID()))
        {
            detail += " -> invisible (transparent texture)";
            return Seen::INVISIBLE;
        }
        LLViewerTexture* image = object->getTEImage(i);
        LLImageGL* gl = image ? image->getGLTexture() : nullptr;
        if (!gl || !gl->getHasGLTexture())
        {
            detail += " image not loaded -> unknown";
            return Seen::UNKNOWN;
        }
        detail += llformat(" components %d alpha-only %d mask %d", (S32)gl->getComponents(), gl->isAlphaOnly() ? 1 : 0,
                           gl->getIsAlphaMask() ? 1 : 0);
        if (gl->isAlphaOnly())
        {
            detail += " -> invisible (alpha-only image)";
            return Seen::INVISIBLE;
        }
        if (gl->getComponents() == 4)
        {
            // The pick mask holds a bit per cell for alpha above 32/255; none set means nothing shows at any cutoff.
            bool any = false;
            for (S32 y = 0; y < 16 && !any; ++y)
            {
                for (S32 x = 0; x < 16 && !any; ++x)
                {
                    any = gl->getMask(LLVector2((x + 0.5f) / 16.f, (y + 0.5f) / 16.f));
                }
            }
            if (!any)
            {
                detail += " -> invisible (alpha below the mask threshold everywhere)";
                return Seen::INVISIBLE;
            }
        }
        detail += " -> visible";
        return Seen::VISIBLE;
    }

    // A part is invisible when every face is; UNKNOWN while any face is still unknown and none is visible.
    Seen partSeen(const LLUUID& part, std::string* report)
    {
        LLViewerObject* object = gObjectList.findObject(part);
        if (!object || object->getNumTEs() == 0)
        {
            if (report)
            {
                *report = object ? "no faces" : "object not loaded";
            }
            return Seen::UNKNOWN;
        }
        Seen seen = Seen::INVISIBLE;
        for (U8 i = 0; i < object->getNumTEs(); ++i)
        {
            std::string detail;
            const Seen face = faceSeen(object, i, detail);
            if (report)
            {
                *report += llformat("%s[face %d: %s]", report->empty() ? "" : " ", i, detail.c_str());
            }
            if (face == Seen::VISIBLE)
            {
                seen = Seen::VISIBLE;
                if (!report)
                {
                    break;
                }
            }
            else if (face == Seen::UNKNOWN && seen == Seen::INVISIBLE)
            {
                seen = Seen::UNKNOWN;
            }
        }
        return seen;
    }
}

bool SKOverviewCensus::update()
{
    if (!SKWorldCensus::instanceExists())
    {
        return !mBatches.empty();
    }
    SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        return !mBatches.empty();
    }
    // Work only on change. The census rescans every few frames, but a part it did not see change keeps its record
    // object, so the records' identities say whether anything moved. Parts whose look was not known yet are asked
    // again every couple of seconds, and sculpts (which have no record when phantom) every few.
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    if (snap->mStamp != mStamp)
    {
        mStamp = snap->mStamp;
        U64 records = 1469598103934665603ull;
        for (const sknav::RecordPtr& rec : snap->mRecords)
        {
            records = (records ^ (U64)(uintptr_t)rec.get()) * 1099511628211ull;
        }
        mRecordsChanged |= records != mRecordsKey;
        mRecordsKey = records;
    }
    const bool recheck = mUnknownLook && now >= mRecheckAt;
    const bool sculpts_due = now >= mSculptsAt;
    // The surrounds are looked at least often: large rendered-only parts anywhere in the loaded regions.
    static LLCachedControl<F32> surrounds_every(gSavedSettings, "SKOverviewSurroundsRefresh", 15.f);
    const bool surrounds_due = now >= mSurroundsAt;
    if (surrounds_due)
    {
        mSurroundsAt = now + llmax((F32)surrounds_every, 1.f);
        const U64 before = mSurroundsSig;
        gatherSurrounds(*snap);
        mRecordsChanged |= mSurroundsSig != before;
    }
    if (!mRecordsChanged && !recheck && !sculpts_due && !mBatches.empty())
    {
        return true;
    }
    mRecordsChanged = false;
    mUnknownLook = false;
    mRecheckAt = now + 2.0;
    mSculptsAt = now + 5.0;

    // A new scan most often holds the same parts where they were: rebuild only when the geometry moved.
    U64 sig = 1469598103934665603ull;
    auto mix = [&sig](U64 v)
    {
        sig ^= v + 0x9e3779b97f4a7c15ull + (sig << 6) + (sig >> 2);
    };
    // Sculpted parts draw from what is rendered, over land and void alike: a megasculpt's physics is a poor stand-in
    // for the ground it shows, and a phantom one has no physics record at all. Their census records are not drawn.
    std::vector<RenderedPart> sculpts;
    std::set<LLUUID> sculpted;
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
        if (!volume || volume->isDead() || volume->isAttachment() || volume->isMesh() || !volume->isSculpted() ||
            !volume->getRegion() || !volume->getVolume())
        {
            continue;
        }
        sculpted.insert(volume->getID());
        // A sculpt whose map has not loaded is a stretched placeholder sphere: nothing until it has.
        if (volume->getVolume()->getSculptLevel() != 0)
        {
            mUnknownLook = true;
            continue;
        }
        const LLVector3d centre = volume->getPositionGlobal();
        const F64 reach = snap->mRange + volume->getScale().magVec();
        if ((centre - snap->mCenter).magVec() > reach && (centre - snap->mCamera).magVec() > reach)
        {
            continue;
        }
        SKShapeGeometry geom;
        if (!skRenderShapeGeometry(volume, geom))
        {
            continue;
        }
        RenderedPart sc;
        sc.mPart = volume->getID();
        sc.mOrigin = volume->getRegion()->getOriginGlobal();
        sc.mAnchor = volume->getRootEdit()->getPositionGlobal();
        sc.mRoot = volume->getRootEdit()->getID();
        const LLVector3 region_agent = volume->getRegion()->getOriginAgent();
        sc.mTris.reserve(geom.mTriangles.size());
        F32 lo = 1e30f, hi = -1e30f;
        for (const LLVector3& v : geom.mTriangles)
        {
            const LLVector3 local = volume->volumePositionToAgent(v) - region_agent;
            lo = llmin(lo, local.mV[VZ]);
            hi = llmax(hi, local.mV[VZ]);
            sc.mTris.push_back(local);
        }
        sc.mHeight = hi - lo;
        const LLVector3 scale = volume->getScale();
        const bool large = llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ])) > skoverview::INVISIBLE_WALL_M;
        const Seen seen = partSeen(sc.mPart, nullptr);
        mUnknownLook |= seen == Seen::UNKNOWN;
        if (seen == Seen::INVISIBLE)
        {
            if (large)
            {
                continue;
            }
            sc.mFixed = skoverview::INVISIBLE;
        }
        mix(sc.mPart.getCRC32());
        mix(seen == Seen::INVISIBLE ? 0x7a11u : 0x0u);
        mix((U64)sc.mTris.size());
        mix((U64)(S64)std::llround(centre.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(centre.mdV[VY] * 64.0) << 21) ^
            ((U64)(S64)std::llround(centre.mdV[VZ] * 64.0) << 42));
        sculpts.push_back(std::move(sc));
    }

    // The surrounds the near look did not already take.
    for (const RenderedPart& part : mSurrounds)
    {
        if (std::none_of(sculpts.begin(), sculpts.end(),
                         [&part](const RenderedPart& sc) { return sc.mPart == part.mPart; }))
        {
            sculpts.push_back(part);
        }
    }
    mix(mSurroundsSig);

    std::vector<const sknav::Record*> kept;
    std::vector<U8> fixed_classes;
    std::vector<bool> void_only;   // a surround record: only its triangles over no connected region are drawn
    kept.reserve(snap->mRecords.size());
    for (const sknav::RecordPtr& rec : snap->mRecords)
    {
        skoverview::CensusClass fixed;
        if (!rec || sculpted.count(rec->mPart))
        {
            continue;
        }
        if (rec->mRole == sknav::Role::SURROUND)
        {
            // A sim surround's rendered shape, in the void only: over land a phantom megasculpt would roof the region.
            if (std::any_of(sculpts.begin(), sculpts.end(), [&rec](const RenderedPart& sc) { return sc.mPart == rec->mPart; }) ||
                partSeen(rec->mPart, nullptr) == Seen::INVISIBLE)
            {
                continue;
            }
            kept.push_back(rec.get());
            fixed_classes.push_back(skoverview::NUM_CENSUS_CLASSES);
            void_only.push_back(true);
            mix(rec->mSignature);
            mix(rec->mPart.getCRC32());
            continue;
        }
        if (!skoverview::keepRecord(*rec, fixed))
        {
            continue;
        }
        const LLVector3d extent = rec->mMax - rec->mMin;
        const bool large = llmax(extent.mdV[VX], llmax(extent.mdV[VY], extent.mdV[VZ])) > skoverview::INVISIBLE_WALL_M;
        std::string report;
        const Seen seen = partSeen(rec->mPart, large ? &report : nullptr);
        mUnknownLook |= seen == Seen::UNKNOWN;
        const bool transparent = seen == Seen::INVISIBLE;
        if (large)
        {
            // Once per part and verdict: the large ones are those that cover the view when misjudged.
            static std::map<LLUUID, Seen> logged;
            auto it = logged.find(rec->mPart);
            if (it == logged.end() || it->second != seen)
            {
                logged[rec->mPart] = seen;
                LL_INFOS("SKOverview") << "large census part " << rec->mPart << " " << extent << " m: "
                                       << (seen == Seen::INVISIBLE ? "invisible, dropped" : seen == Seen::VISIBLE ? "visible, drawn" : "not known yet, drawn")
                                       << " " << report << LL_ENDL;
            }
        }
        mix(transparent ? 0x7a11u : 0x0u);
        if (transparent)
        {
            // Small, a plausible invisible wall: hatched. Larger: an invisible shell nobody plays against.
            const LLVector3d size = rec->mMax - rec->mMin;
            if (size.mdV[VX] > skoverview::INVISIBLE_WALL_M || size.mdV[VY] > skoverview::INVISIBLE_WALL_M ||
                size.mdV[VZ] > skoverview::INVISIBLE_WALL_M)
            {
                continue;
            }
            fixed = skoverview::INVISIBLE;
        }
        kept.push_back(rec.get());
        fixed_classes.push_back(fixed);
        void_only.push_back(false);
        mix(rec->mSignature);
        mix(rec->mPart.getCRC32());
        mix((U64)(S64)std::llround(rec->mMin.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(rec->mMin.mdV[VY] * 64.0) << 21) ^
            ((U64)(S64)std::llround(rec->mMin.mdV[VZ] * 64.0) << 42));
        mix((U64)rec->mFeature | ((U64)rec->mExclude << 8));
    }
    // Each linkset is a group, so the tactical cut can take a building's linksets and leave its neighbours whole: a
    // cluster joins whatever touches, often a whole block. A level still keeps whole clusters, by the heights of the
    // cluster a linkset is in.
    std::map<const sknav::Record*, U32> cluster_of;
    const U32 clusters = (U32)snap->mClusters.mClusters.size();
    for (U32 c = 0; c < clusters; ++c)
    {
        for (const sknav::RecordPtr& rec : snap->mClusters.mClusters[c]->mRecords)
        {
            cluster_of[rec.get()] = c;
        }
    }
    std::map<LLUUID, U32> linkset_ids;
    std::vector<LLUUID> roots;
    std::vector<U32> group_of(kept.size() + sculpts.size());
    std::map<U32, U32> group_cluster;
    for (size_t i = 0; i < group_of.size(); ++i)
    {
        const LLUUID root = i >= kept.size() ? sculpts[i - kept.size()].mRoot
                                             : kept[i]->mRoot.notNull() ? kept[i]->mRoot : kept[i]->mPart;
        const auto ins = linkset_ids.emplace(root, (U32)linkset_ids.size());
        if (ins.second)
        {
            roots.push_back(root);
        }
        group_of[i] = ins.first->second;
        if (i < kept.size())
        {
            const auto it = cluster_of.find(kept[i]);
            if (it != cluster_of.end())
            {
                group_cluster.emplace(group_of[i], it->second);
            }
        }
    }
    auto groupOf = [&](size_t i) -> U32 { return group_of[i]; };
    for (size_t i = 0; i < kept.size(); ++i)
    {
        mix(groupOf(i));
        const auto cluster = group_cluster.find(groupOf(i));
        mix(cluster != group_cluster.end() ? cluster->second : 0xffffffffu);
    }
    // Which land is connected decides what of a surround is in the void.
    for (const LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        mix(region->getHandle());
    }
    // Where each linkset is, for whose region it is in: its root's position, so a linkset straddling a border has
    // one answer.
    auto anchorOf = [&](size_t i) -> LLVector3d
    {
        if (i >= kept.size())
        {
            return sculpts[i - kept.size()].mAnchor;
        }
        const LLViewerObject* root = gObjectList.findObject(kept[i]->mRoot);
        return root && !root->isDead() ? root->getPositionGlobal() : (kept[i]->mMin + kept[i]->mMax) * 0.5;
    };
    std::map<U32, std::pair<LLVector3d, bool>> group_anchor;
    if (sig == mSignature && !mBatches.empty())
    {
        return true;
    }
    mSignature = sig;
    LL_PROFILE_ZONE_NAMED("sk overview census upload");

    // Per region, per class, per group, region-local triangles; and each group's bounds.
    struct Bounds
    {
        LLVector3 mMin{ 1e30f, 1e30f, 1e30f };
        LLVector3 mMax{ -1e30f, -1e30f, -1e30f };
    };
    struct Bins
    {
        LLVector3d mOrigin;
        std::map<U32, std::vector<LLVector3>> mClass[skoverview::NUM_CENSUS_CLASSES];
        std::map<U32, Bounds> mBounds;
    };
    std::map<std::pair<S64, S64>, Bins> regions;
    std::vector<LLVector3> tris;
    for (size_t i = 0; i < kept.size() + sculpts.size(); ++i)
    {
        const bool is_sculpt = i >= kept.size();
        const LLVector3d origin = is_sculpt ? sculpts[i - kept.size()].mOrigin : kept[i]->mOrigin;
        const std::pair<S64, S64> key((S64)std::llround(origin.mdV[VX]), (S64)std::llround(origin.mdV[VY]));
        Bins& bins = regions[key];
        bins.mOrigin = origin;
        F32 part_height = 0.f;
        U8 fixed_class = skoverview::NUM_CENSUS_CLASSES;
        if (is_sculpt)
        {
            const RenderedPart& sc = sculpts[i - kept.size()];
            tris = sc.mTris;
            part_height = sc.mHeight;
            fixed_class = sc.mFixed;
        }
        else
        {
            const sknav::Record& rec = *kept[i];
            tris.clear();
            sknav::emitRecord(rec, rec.mOrigin, tris);
            part_height = (F32)(rec.mMax.mdV[VZ] - rec.mMin.mdV[VZ]);
            fixed_class = fixed_classes[i];
        }
        const bool in_void_only = i < kept.size() && void_only[i];
        group_anchor.emplace(groupOf(i), std::make_pair(anchorOf(i), in_void_only));
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            const LLVector3& a = tris[t];
            const LLVector3& b = tris[t + 1];
            const LLVector3& c = tris[t + 2];
            if (!a.isFinite() || !b.isFinite() || !c.isFinite())
            {
                continue;
            }
            LLVector3 n = (b - a) % (c - a);
            const F32 len = n.length();
            if (len <= 1.0e-8f)
            {
                continue;
            }
            if (in_void_only && LLWorld::getInstance()->getRegionFromPosGlobal(origin + LLVector3d((a + b + c) / 3.f)))
            {
                continue;
            }
            const skoverview::CensusClass cls = fixed_class < skoverview::NUM_CENSUS_CLASSES
                                                    ? (skoverview::CensusClass)fixed_class
                                                    : skoverview::classifyTriangle(n.mV[VZ] / len, part_height);
            const U32 group = groupOf(i);
            Bounds& bounds = bins.mBounds[group];
            for (const LLVector3* v : { &a, &b, &c })
            {
                update_min_max(bounds.mMin, bounds.mMax, *v);
            }
            std::vector<LLVector3>& out = bins.mClass[cls][group];
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        }
    }

    mBatches.clear();
    mTriangles = 0;
    for (auto& entry : regions)
    {
        Bins& bins = entry.second;
        size_t total = 0;
        for (const auto& groups : bins.mClass)
        {
            for (const auto& g : groups)
            {
                total += g.second.size();
            }
        }
        if (!total)
        {
            continue;
        }
        // A buffer's byte size is a U32 of 16-byte vertices: far beyond anything a region holds, but a runaway
        // source must not wrap it into a short buffer.
        if (total > skoverview::MAX_BATCH_VERTICES)
        {
            LL_WARNS("SKOverview") << "census batch of " << total << " vertices over the " << skoverview::MAX_BATCH_VERTICES
                                   << " limit; region left out" << LL_ENDL;
            continue;
        }
        Batch batch;
        batch.mOrigin = bins.mOrigin;
        batch.mBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_WEIGHT); // weight: the cut height
        if (!batch.mBuffer->allocateBuffer((U32)total, 0))
        {
            LL_WARNS("SKOverview") << "census buffer of " << total << " vertices could not be allocated" << LL_ENDL;
            continue;
        }
        LLStrider<LLVector3> verts;
        if (!batch.mBuffer->getVertexStrider(verts) || !verts.get() || batch.mBuffer->getNumVerts() < (U32)total)
        {
            LL_WARNS("SKOverview") << "census batch of " << total << " vertices could not be mapped (buffer holds "
                                   << batch.mBuffer->getNumVerts() << ")" << LL_ENDL;
            continue;
        }
        // Never past what was allocated, whatever the bins hold.
        std::map<U32, size_t> group_index;
        for (const auto& b : bins.mBounds)
        {
            group_index[b.first] = batch.mGroups.size();
            Group group;
            group.mMinZ = b.second.mMin.mV[VZ];
            group.mMaxZ = b.second.mMax.mV[VZ];
            LLVector3d middle = bins.mOrigin + LLVector3d((b.second.mMin.mV[VX] + b.second.mMax.mV[VX]) * 0.5,
                                                          (b.second.mMin.mV[VY] + b.second.mMax.mV[VY]) * 0.5, 0.0);
            const auto cluster = group_cluster.find(b.first);
            if (cluster != group_cluster.end())
            {
                const sknav::Cluster& c = *snap->mClusters.mClusters[cluster->second];
                group.mMinZ = (F32)c.mMin.mdV[VZ];
                group.mMaxZ = (F32)c.mMax.mdV[VZ];
                middle = LLVector3d((c.mMin.mdV[VX] + c.mMax.mdV[VX]) * 0.5, (c.mMin.mdV[VY] + c.mMax.mdV[VY]) * 0.5, 0.0);
            }
            // On the ground when its cluster's base is no higher above the land under its middle than a step or two.
            group.mGround = group.mMinZ <= LLWorld::getInstance()->resolveLandHeightGlobal(middle) + skoverview::GROUND_BASE_M;
            group.mId = b.first;
            group.mRoot = b.first < roots.size() ? roots[b.first] : LLUUID::null;
            group.mMin = bins.mOrigin + LLVector3d(b.second.mMin);
            group.mMax = bins.mOrigin + LLVector3d(b.second.mMax);
            const auto anchor = group_anchor.find(b.first);
            if (anchor != group_anchor.end())
            {
                group.mAnchor = anchor->second.first;
                group.mNeverCut = anchor->second.second;
            }
            batch.mGroups.push_back(group);
        }
        U32 at = 0;
        for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
        {
            batch.mStart[c] = at;
            for (const auto& g : bins.mClass[c])
            {
                Group& group = batch.mGroups[group_index[g.first]];
                group.mStart[c] = at;
                for (const LLVector3& p : g.second)
                {
                    if (at >= (U32)total)
                    {
                        break;
                    }
                    *verts++ = p;
                    batch.mPositions.push_back(p);
                    ++at;
                }
                group.mCount[c] = at - group.mStart[c];
            }
        }
        batch.mStart[skoverview::NUM_CENSUS_CLASSES] = at;
        LLStrider<F32> cuts;
        if (batch.mBuffer->getWeightStrider(cuts) && cuts.get())
        {
            for (U32 v = 0; v < (U32)total; ++v)
            {
                *cuts++ = skoverview::NO_CUT;
            }
        }
        batch.mBuffer->unmapBuffer();
        mTriangles += at / 3;
        mBatches.push_back(std::move(batch));
    }
    mCuts.clear();
    mCutSignature = 0;
    ++mCutStamp;
    LL_DEBUGS("SKOverview") << "census upload: " << mBatches.size() << " regions, " << mTriangles << " triangles" << LL_ENDL;
    return !mBatches.empty();
}

F32 SKOverviewCensus::levelTop(const Level& level, F32 floor) const
{
    F32 top = floor;
    for (const Batch& batch : mBatches)
    {
        for (const Group& group : batch.mGroups)
        {
            if (level.keeps(group))
            {
                top = llmax(top, group.mMaxZ);
            }
        }
    }
    return top;
}

namespace
{
    // Whether a part reaching this far from its centre crosses into the void: no region under one of its sides.
    bool reachesVoid(const LLVector3d& centre, F32 reach)
    {
        static const F64 SIDES[4][2] = { { 1.0, 0.0 }, { -1.0, 0.0 }, { 0.0, 1.0 }, { 0.0, -1.0 } };
        for (const auto& side : SIDES)
        {
            const LLVector3d at = centre + LLVector3d(side[0] * reach, side[1] * reach, 0.0);
            if (!LLWorld::getInstance()->getRegionFromPosGlobal(at))
            {
                return true;
            }
        }
        return false;
    }
}

void SKOverviewCensus::gatherSurrounds(const SKCensusSnapshot& snap)
{
    LL_PROFILE_ZONE_SCOPED;
    // Mega parts, at least a region on some axis or reaching into the void, that the census holds no shape for
    // (megasculpts, phantom or physics-less meshes and megaprims) are drawn from what is rendered, wherever they are
    // in the loaded regions, so the map shows the land they make.
    std::set<LLUUID> recorded;
    for (const sknav::RecordPtr& rec : snap.mRecords)
    {
        if (rec)
        {
            recorded.insert(rec->mPart);
        }
    }
    mSurrounds.clear();
    U64 sig = 1469598103934665603ull;
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
        if (!volume || volume->isDead() || volume->isAttachment() || volume->isAvatar() || !volume->getRegion() ||
            !volume->getVolume() || volume->getAvatar())
        {
            continue;
        }
        const LLVector3 scale = volume->getScale();
        const F32 size = llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ]));
        if (size < volume->getRegion()->getWidth() && !reachesVoid(volume->getPositionGlobal(), size * 0.5f))
        {
            continue;
        }
        const bool sculpt = volume->isSculpted() && !volume->isMesh();
        if (!sculpt && recorded.count(volume->getID()))
        {
            continue;
        }
        if (volume->isSculpted() && volume->getVolume()->getSculptLevel() != 0)
        {
            continue; // its map or mesh has not loaded: a placeholder, not its shape
        }
        if (partSeen(volume->getID(), nullptr) != Seen::VISIBLE)
        {
            continue;
        }
        SKShapeGeometry geom;
        if (!skRenderShapeGeometry(volume, geom))
        {
            continue;
        }
        RenderedPart part;
        part.mPart = volume->getID();
        part.mOrigin = volume->getRegion()->getOriginGlobal();
        part.mAnchor = volume->getRootEdit()->getPositionGlobal();
        part.mRoot = volume->getRootEdit()->getID();
        const LLVector3 region_agent = volume->getRegion()->getOriginAgent();
        part.mTris.reserve(geom.mTriangles.size());
        F32 lo = 1e30f, hi = -1e30f;
        for (const LLVector3& v : geom.mTriangles)
        {
            const LLVector3 local = volume->volumePositionToAgent(v) - region_agent;
            lo = llmin(lo, local.mV[VZ]);
            hi = llmax(hi, local.mV[VZ]);
            part.mTris.push_back(local);
        }
        part.mHeight = hi - lo;
        const LLVector3d centre = volume->getPositionGlobal();
        sig = (sig ^ part.mPart.getCRC32()) * 1099511628211ull;
        sig = (sig ^ (U64)part.mTris.size()) * 1099511628211ull;
        sig = (sig ^ ((U64)(S64)std::llround(centre.mdV[VX] * 64.0) ^ ((U64)(S64)std::llround(centre.mdV[VY] * 64.0) << 21) ^
                      ((U64)(S64)std::llround(centre.mdV[VZ] * 64.0) << 42))) * 1099511628211ull;
        mSurrounds.push_back(std::move(part));
    }
    mSurroundsSig = sig;
}

namespace
{
    // Whether the segment from p to q crosses the triangle (a, b, c), short of q (Moller-Trumbore).
    bool segmentHits(const LLVector3& p, const LLVector3& q, const LLVector3& a, const LLVector3& b, const LLVector3& c)
    {
        const LLVector3 d = q - p;
        const LLVector3 e1 = b - a;
        const LLVector3 e2 = c - a;
        const LLVector3 h = d % e2;
        const F32 det = e1 * h;
        if (std::fabs(det) < 1.0e-9f)
        {
            return false;
        }
        const F32 inv = 1.f / det;
        const LLVector3 s = p - a;
        const F32 u = (s * h) * inv;
        if (u < 0.f || u > 1.f)
        {
            return false;
        }
        const LLVector3 qv = s % e1;
        const F32 v = (d * qv) * inv;
        if (v < 0.f || u + v > 1.f)
        {
            return false;
        }
        const F32 t = (e2 * qv) * inv;
        return t > 1.0e-4f && t < 0.999f;
    }

    // The distance in plan from p to the triangle (a, b, c): 0 inside it; a wall's edge-on triangle is its segment.
    F32 planDistance(const LLVector2& p, const LLVector3& a3, const LLVector3& b3, const LLVector3& c3)
    {
        const LLVector2 a(a3.mV[VX], a3.mV[VY]), b(b3.mV[VX], b3.mV[VY]), c(c3.mV[VX], c3.mV[VY]);
        auto cross = [](const LLVector2& o, const LLVector2& u, const LLVector2& v)
        {
            return (u.mV[VX] - o.mV[VX]) * (v.mV[VY] - o.mV[VY]) - (u.mV[VY] - o.mV[VY]) * (v.mV[VX] - o.mV[VX]);
        };
        const F32 area = cross(a, b, c);
        if (std::fabs(area) > 1.0e-6f)
        {
            const F32 s0 = cross(a, b, p) / area, s1 = cross(b, c, p) / area, s2 = cross(c, a, p) / area;
            if (s0 >= 0.f && s1 >= 0.f && s2 >= 0.f)
            {
                return 0.f;
            }
        }
        auto segment = [&p](const LLVector2& u, const LLVector2& v)
        {
            const LLVector2 d = v - u;
            const F32 len2 = d * d;
            const F32 t = len2 > 0.f ? llclamp(((p - u) * d) / len2, 0.f, 1.f) : 0.f;
            return (u + d * t - p).length();
        };
        return llmin(segment(a, b), llmin(segment(b, c), segment(c, a)));
    }

    // Whether the segment from p to q meets the box (slab test).
    bool segmentMeetsBox(const LLVector3d& p, const LLVector3d& q, const LLVector3d& lo, const LLVector3d& hi)
    {
        F64 t0 = 0.0, t1 = 1.0;
        for (S32 k = 0; k < 3; ++k)
        {
            const F64 d = q.mdV[k] - p.mdV[k];
            if (std::fabs(d) < 1.0e-12)
            {
                if (p.mdV[k] < lo.mdV[k] || p.mdV[k] > hi.mdV[k])
                {
                    return false;
                }
                continue;
            }
            F64 a = (lo.mdV[k] - p.mdV[k]) / d;
            F64 b = (hi.mdV[k] - p.mdV[k]) / d;
            if (a > b)
            {
                std::swap(a, b);
            }
            t0 = llmax(t0, a);
            t1 = llmin(t1, b);
            if (t0 > t1)
            {
                return false;
            }
        }
        return true;
    }
}

void SKOverviewCensus::applyCut(const LLViewerRegion* region, const std::vector<Occupant>& occupants, const LLVector3d& eye, U64 key)
{
    LL_PROFILE_ZONE_SCOPED;
    if (key == mCutKey && mCutSignature == mSignature)
    {
        return;
    }
    mCutKey = key;
    // A linkset spans batches when it crosses a border: its bounds over all of them, and where its triangles are.
    struct Extent
    {
        LLVector3d mMin{ 1e30, 1e30, 1e30 };
        LLVector3d mMax{ -1e30, -1e30, -1e30 };
        bool mCuttable = false;
        LLUUID mRoot;
        bool mGround = false;   // in a cluster standing on the land
        F64 mBottom = 1e30;     // its cluster's base
        std::vector<std::pair<const Batch*, const Group*>> mParts;
    };
    std::map<U32, Extent> extents;
    for (const Batch& batch : mBatches)
    {
        for (const Group& group : batch.mGroups)
        {
            Extent& e = extents[group.mId];
            for (S32 i = 0; i < 3; ++i)
            {
                e.mMin.mdV[i] = llmin(e.mMin.mdV[i], group.mMin.mdV[i]);
                e.mMax.mdV[i] = llmax(e.mMax.mdV[i], group.mMax.mdV[i]);
            }
            e.mCuttable = !group.mNeverCut && region && region->pointInRegionGlobal(group.mAnchor);
            e.mRoot = group.mRoot;
            e.mGround |= group.mGround;
            e.mBottom = llmin(e.mBottom, (F64)group.mMinZ);
            e.mParts.emplace_back(&batch, &group);
        }
    }
    // Whether a linkset's own triangles (not its hatched invisible ones) block the segment.
    auto blocks = [](const Extent& e, const LLVector3d& p, const LLVector3d& q)
    {
        for (const auto& part : e.mParts)
        {
            const Batch& batch = *part.first;
            const LLVector3 a(p - batch.mOrigin);
            const LLVector3 b(q - batch.mOrigin);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(part.second->mStart[c] + part.second->mCount[c], (U32)batch.mPositions.size());
                for (U32 v = part.second->mStart[c]; v + 2 < end; v += 3)
                {
                    if (segmentHits(a, b, batch.mPositions[v], batch.mPositions[v + 1], batch.mPositions[v + 2]))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    const F64 SLACK_M = 0.1;    // column: how far outside a linkset's bounds still counts as under it
    const F64 ABOVE_M = 0.5;    // what rises less than this over the floor bounds nothing
    std::map<U32, F32> cut_of;
    std::map<U32, std::string> why;
    std::set<U32> for_agent;
    auto mark = [&](U32 id, const Occupant& occupant, const char* rule)
    {
        const std::string who = occupant.mSelf ? std::string("agent") : "occupant " + occupant.mId.asString();
        if (occupant.mSelf)
        {
            if (for_agent.insert(id).second)
            {
                cut_of[id] = occupant.mCut;
                why[id] = who + " " + rule;
            }
            return;
        }
        if (for_agent.count(id))
        {
            return;
        }
        auto it = cut_of.find(id);
        if (it == cut_of.end() || occupant.mCut > it->second)
        {
            cut_of[id] = occupant.mCut;
            why[id] = who + " " + rule;
        }
    };
    std::vector<const Occupant*> ordered;
    for (const Occupant& occupant : occupants)
    {
        if (occupant.mSelf)
        {
            ordered.insert(ordered.begin(), &occupant);
        }
        else
        {
            ordered.push_back(&occupant);
        }
    }
    // Whether a linkset has a triangle above p's floor whose plan footprint holds p (within the slack): it stands
    // over them, not merely its box.
    auto overColumn = [&](const Extent& e, const LLVector3d& p) -> bool
    {
        for (const auto& part : e.mParts)
        {
            const Batch& batch = *part.first;
            const LLVector3 local(p - batch.mOrigin);
            const LLVector2 at(local.mV[VX], local.mV[VY]);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(part.second->mStart[c] + part.second->mCount[c], (U32)batch.mPositions.size());
                for (U32 v = part.second->mStart[c]; v + 2 < end; v += 3)
                {
                    const LLVector3& a = batch.mPositions[v];
                    const LLVector3& b = batch.mPositions[v + 1];
                    const LLVector3& d = batch.mPositions[v + 2];
                    if (llmax(a.mV[VZ], llmax(b.mV[VZ], d.mV[VZ])) >= local.mV[VZ] + ABOVE_M &&
                        planDistance(at, a, b, d) <= SLACK_M)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    // Whether a linkset has a triangle above the floor whose plan footprint comes within a cell of one of the room's
    // dilated cells; how says which (wall or cover) and the cell, for the log.
    std::unordered_set<U64> dilated;
    LLVector3d room_min, room_max;
    auto cellKey = [](S32 x, S32 y) { return ((U64)(U32)x << 32) | (U64)(U32)y; };
    auto touchesRoom = [&](const Extent& e, const Occupant& occupant, const std::unordered_set<U64>& cells, F64 cell_m,
                           std::string& how) -> bool
    {
        const F32 reach = (F32)(cell_m * 0.7072);
        for (const auto& part : e.mParts)
        {
            const Batch& batch = *part.first;
            const F32 floor_local = (F32)(occupant.mFloor.mdV[VZ] - batch.mOrigin.mdV[VZ]);
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(part.second->mStart[c] + part.second->mCount[c], (U32)batch.mPositions.size());
                for (U32 v = part.second->mStart[c]; v + 2 < end; v += 3)
                {
                    const LLVector3& a = batch.mPositions[v];
                    const LLVector3& b = batch.mPositions[v + 1];
                    const LLVector3& d = batch.mPositions[v + 2];
                    if (llmax(a.mV[VZ], llmax(b.mV[VZ], d.mV[VZ])) < floor_local + ABOVE_M)
                    {
                        continue;
                    }
                    // The cells under the triangle's plan box, as far as the room has any.
                    const F64 lo_x = batch.mOrigin.mdV[VX] + llmin(a.mV[VX], llmin(b.mV[VX], d.mV[VX]));
                    const F64 hi_x = batch.mOrigin.mdV[VX] + llmax(a.mV[VX], llmax(b.mV[VX], d.mV[VX]));
                    const F64 lo_y = batch.mOrigin.mdV[VY] + llmin(a.mV[VY], llmin(b.mV[VY], d.mV[VY]));
                    const F64 hi_y = batch.mOrigin.mdV[VY] + llmax(a.mV[VY], llmax(b.mV[VY], d.mV[VY]));
                    const S32 x0 = (S32)std::floor(llmax(lo_x, room_min.mdV[VX]) / cell_m);
                    const S32 x1 = (S32)std::floor(llmin(hi_x, room_max.mdV[VX]) / cell_m);
                    const S32 y0 = (S32)std::floor(llmax(lo_y, room_min.mdV[VY]) / cell_m);
                    const S32 y1 = (S32)std::floor(llmin(hi_y, room_max.mdV[VY]) / cell_m);
                    for (S32 x = x0; x <= x1; ++x)
                    {
                        for (S32 y = y0; y <= y1; ++y)
                        {
                            if (!cells.count(cellKey(x, y)))
                            {
                                continue;
                            }
                            const LLVector2 centre((F32)((x + 0.5) * cell_m - batch.mOrigin.mdV[VX]),
                                                   (F32)((y + 0.5) * cell_m - batch.mOrigin.mdV[VY]));
                            if (planDistance(centre, a, b, d) <= reach)
                            {
                                const LLVector3 n = (b - a) % (d - a);
                                const bool wall = n.length() > 0.f && std::fabs(n.mV[VZ]) < 0.5f * n.length();
                                how = llformat("%s at cell %d,%d", wall ? "wall" : "cover", x, y);
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    };
    for (const Occupant* occupant : ordered)
    {
        const LLVector3d& p = occupant->mFloor;
        dilated.clear();
        if (occupant->mRoomCells && !occupant->mRoomCells->empty())
        {
            // The room's cells grown by one, so a wall standing just past its last cell of air still touches it.
            S32 min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
            for (const auto& cell : *occupant->mRoomCells)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    for (S32 dy = -1; dy <= 1; ++dy)
                    {
                        dilated.insert(cellKey(cell.first + dx, cell.second + dy));
                    }
                }
                min_x = llmin(min_x, cell.first - 1);
                min_y = llmin(min_y, cell.second - 1);
                max_x = llmax(max_x, cell.first + 1);
                max_y = llmax(max_y, cell.second + 1);
            }
            const F64 cell_m = occupant->mRoomCellM;
            room_min.setVec(min_x * cell_m, min_y * cell_m, 0.0);
            room_max.setVec((max_x + 1) * cell_m, (max_y + 1) * cell_m, 0.0);
        }
        for (const auto& entry : extents)
        {
            const Extent& e = entry.second;
            if (!e.mCuttable || e.mMax.mdV[VZ] < p.mdV[VZ] + ABOVE_M)
            {
                continue;
            }
            if (occupant->mRoomCells && !dilated.empty())
            {
                // The room's building, by its geometry: a linkset whose triangles above the floor come within a cell of
                // the room's own cells, standing round them (walls) or over them (floors above, the roof).
                const F64 cell_m = occupant->mRoomCellM;
                if (e.mMax.mdV[VX] < room_min.mdV[VX] || e.mMin.mdV[VX] > room_max.mdV[VX] ||
                    e.mMax.mdV[VY] < room_min.mdV[VY] || e.mMin.mdV[VY] > room_max.mdV[VY])
                {
                    continue; // its box is nowhere near the room's
                }
                std::string how;
                if (touchesRoom(e, *occupant, dilated, cell_m, how))
                {
                    mark(entry.first, *occupant, ("room " + how).c_str());
                }
            }
            else if (p.mdV[VX] >= e.mMin.mdV[VX] - SLACK_M && p.mdV[VX] <= e.mMax.mdV[VX] + SLACK_M &&
                     p.mdV[VY] >= e.mMin.mdV[VY] - SLACK_M && p.mdV[VY] <= e.mMax.mdV[VY] + SLACK_M &&
                     overColumn(e, p))
            {
                mark(entry.first, *occupant, "column");
            }
        }
        // Whatever hides them from the eye.
        for (const LLVector3d& target : occupant->mSight)
        {
            for (const auto& entry : extents)
            {
                const Extent& e = entry.second;
                if (e.mCuttable && segmentMeetsBox(eye, target, e.mMin, e.mMax) && blocks(e, eye, target))
                {
                    mark(entry.first, *occupant, "line-of-sight");
                }
            }
        }
    }
    // A floating cluster (a skybox) wholly above the agent's level would roof the view of the ground: hidden, unless
    // someone in it already had it cut. The agent's own skybox is the agent's building, cut as any other.
    F32 agent_cut = skoverview::NO_CUT;
    for (const Occupant& occupant : occupants)
    {
        if (occupant.mSelf)
        {
            agent_cut = occupant.mCut;
        }
    }
    for (const auto& entry : extents)
    {
        const Extent& e = entry.second;
        if (!e.mGround && e.mBottom > agent_cut && !cut_of.count(entry.first))
        {
            cut_of[entry.first] = skoverview::HIDDEN;
            why[entry.first] = "hidden floating";
        }
    }
    std::vector<std::pair<U32, F32>> cuts(cut_of.begin(), cut_of.end());
    if (cuts == mCuts && mCutSignature == mSignature)
    {
        return;
    }
    mCuts = std::move(cuts);
    mCutSignature = mSignature;
    ++mCutStamp;
    mCutRoots.clear();
    for (const auto& cut : cut_of)
    {
        mCutRoots[extents[cut.first].mRoot] = std::make_pair(cut.second, why[cut.first]);
    }
    {
        // Logged when which linksets are cut, or why, changes; not when someone's floor shifts a little.
        std::string what;
        std::string list;
        for (const auto& cut : cut_of)
        {
            const Extent& e = extents[cut.first];
            what += e.mRoot.asString() + why[cut.first] + ";";
            list += llformat("%s linkset %s at z %.2f (%s, top %.2f)", list.empty() ? "" : ";",
                             e.mRoot.asString().c_str(), cut.second, why[cut.first].c_str(), e.mMax.mdV[VZ]);
        }
        if (what != mCutLogged)
        {
            mCutLogged = what;
            LL_INFOS("SKOverview") << "tactical cut: " << (list.empty() ? std::string("nothing cut") : list) << LL_ENDL;
        }
    }
    for (const Batch& batch : mBatches)
    {
        const U32 total = batch.mStart[skoverview::NUM_CENSUS_CLASSES];
        LLStrider<F32> strider;
        if (!total || !batch.mBuffer->getWeightStrider(strider) || !strider.get())
        {
            continue;
        }
        std::vector<F32> values(total, skoverview::NO_CUT);
        for (const Group& group : batch.mGroups)
        {
            const auto it = cut_of.find(group.mId);
            if (it == cut_of.end())
            {
                continue;
            }
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                const U32 end = llmin(group.mStart[c] + group.mCount[c], total);
                std::fill(values.begin() + llmin(group.mStart[c], end), values.begin() + end, it->second);
            }
        }
        for (F32 v : values)
        {
            *strider++ = v;
        }
        batch.mBuffer->unmapBuffer();
    }
}

bool SKOverviewCensus::raycast(const LLVector3d& origin, const LLVector3& dir, F32 max_m, LLVector3d& hit) const
{
    F32 best = max_m;
    bool found = false;
    for (const Batch& batch : mBatches)
    {
        const LLVector3 from(origin - batch.mOrigin);
        const U32 total = (U32)batch.mPositions.size();
        for (const Group& group : batch.mGroups)
        {
            // The group's bounds first, by the slab test.
            const LLVector3 lo(group.mMin - batch.mOrigin);
            const LLVector3 hi(group.mMax - batch.mOrigin);
            F32 t_lo = 0.f;
            F32 t_hi = best;
            bool miss = false;
            for (S32 i = 0; i < 3 && !miss; ++i)
            {
                if (std::fabs(dir.mV[i]) < 1e-9f)
                {
                    miss = from.mV[i] < lo.mV[i] || from.mV[i] > hi.mV[i];
                    continue;
                }
                const F32 a = (lo.mV[i] - from.mV[i]) / dir.mV[i];
                const F32 b = (hi.mV[i] - from.mV[i]) / dir.mV[i];
                t_lo = llmax(t_lo, llmin(a, b));
                t_hi = llmin(t_hi, llmax(a, b));
                miss = t_lo > t_hi;
            }
            if (miss)
            {
                continue;
            }
            F32 cut = skoverview::NO_CUT;
            std::string why;
            if (cutOf(group.mRoot, cut, why) && cut <= skoverview::HIDDEN * 0.5f)
            {
                continue;
            }
            // Cuts are agent z, which is global z; positions are region-local.
            const F32 cut_local = cut >= skoverview::NO_CUT * 0.5f ? cut : cut - (F32)batch.mOrigin.mdV[VZ];
            for (U32 c = 0; c < skoverview::NUM_CENSUS_CLASSES; ++c)
            {
                if (c == skoverview::INVISIBLE)
                {
                    continue;
                }
                const U32 end = llmin(group.mStart[c] + group.mCount[c], total);
                for (U32 v = group.mStart[c]; v + 2 < end; v += 3)
                {
                    // Moller-Trumbore, either face, as the view draws both.
                    const LLVector3& p0 = batch.mPositions[v];
                    const LLVector3 e1 = batch.mPositions[v + 1] - p0;
                    const LLVector3 e2 = batch.mPositions[v + 2] - p0;
                    const LLVector3 pv = dir % e2;
                    const F32 det = e1 * pv;
                    if (std::fabs(det) < 1e-12f)
                    {
                        continue;
                    }
                    const F32 inv = 1.f / det;
                    const LLVector3 tv = from - p0;
                    const F32 u = (tv * pv) * inv;
                    if (u < 0.f || u > 1.f)
                    {
                        continue;
                    }
                    const LLVector3 qv = tv % e1;
                    const F32 w = (dir * qv) * inv;
                    const F32 t = (e2 * qv) * inv;
                    if (w < 0.f || u + w > 1.f || t <= 0.f || t >= best)
                    {
                        continue;
                    }
                    // Above its part's cut nothing is drawn, so the ray goes on through it.
                    if ((from + dir * t).mV[VZ] > cut_local)
                    {
                        continue;
                    }
                    best = t;
                    found = true;
                    hit = origin + LLVector3d(dir * t);
                }
            }
        }
    }
    return found;
}

bool SKOverviewCensus::cutOf(const LLUUID& root, F32& height, std::string& why) const
{
    const auto it = mCutRoots.find(root);
    if (it == mCutRoots.end())
    {
        return false;
    }
    height = it->second.first;
    why = it->second.second;
    return true;
}
