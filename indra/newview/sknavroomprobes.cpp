/**
 * @file sknavroomprobes.cpp
 * @brief Automatic reflection probe spheres for the navmesh's rooms, in place of the octree's.
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

#include "sknavroomprobes.h"

#include "llagent.h"
#include "llreflectionmap.h"
#include "llreflectionmapmanager.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "pipeline.h"
#include "sknavmesh.h"
#include "sknavtilecache.h"
#include "skworldcensus.h"

#include <algorithm>
#include <chrono>

namespace
{
    // Spaces this far from the camera are considered; the budget trims the rest.
    constexpr F64 REACH_M = 192.0;
    constexpr F32 UPDATE_PERIOD_S = 1.f;
    // Fresh placements run on the main thread, at tens of milliseconds a
    // space: a pass places until this is spent, then returns next frame for
    // the rest, so a region's worth of spaces costs frames, not one stall.
    constexpr F32 PLACE_BUDGET_MS = 6.f;

    U64 signatureOf(const sknav::Space& space)
    {
        U64 h = 1469598103934665603ull;
        auto mix = [&h](U64 v) { h = (h ^ v) * 1099511628211ull; };
        mix(static_cast<U64>(space.mBucket));
        mix(static_cast<U64>(std::lround(space.mCeilingMean * 8.f)));
        mix(static_cast<U64>(std::lround(space.mFloorMin * 8.f)));
        mix(static_cast<U64>(std::lround(space.mOpenShare * 16.f)));
        for (const auto& r : space.mRaster)
        {
            mix(r.first);
        }
        return h;
    }
}

SKNavRoomProbes::SKNavRoomProbes() : mReads(std::make_shared<DiskReads>())
{
}

SKNavRoomProbes::~SKNavRoomProbes() = default;

void SKNavRoomProbes::clear()
{
    if (!mHeld.empty() || mPlaced)
    {
        gPipeline.mReflectionMapManager.invalidateAutomaticPlacement();
    }
    mHeld.clear();
    mParts.clear();
    mSolids.clear();
    mSolidsCensus = nullptr;
    mCandidates = mClearedOut = mOverBudget = mPlaced = mCandidatesAdded = 0;
}

void SKNavRoomProbes::takeDiskReads()
{
    std::vector<std::pair<U64, std::vector<sknav::CachedSpaceParts>>> done;
    std::vector<U64> missed;
    {
        std::lock_guard<std::mutex> lock(mReads->mMutex);
        done.swap(mReads->mDone);
        missed.swap(mReads->mMissed);
    }
    for (auto& d : done)
    {
        mCache.adopt(d.first, d.second);
        mReading.erase(d.first);
        ++mDiskLoads;
    }
    if (mMissed.size() > 4096)
    {
        mMissed.clear();
    }
    for (U64 key : missed)
    {
        mReading.erase(key);
        mMissed.insert(key);
    }
}

void SKNavRoomProbes::update(const SKNavWorldViewPtr& view, SKNavMesh& navmesh)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavRoomProbes", true);
    static LLCachedControl<U32> max_probes(gSavedSettings, "SKNavRoomProbesMax", 240);
    static LLCachedControl<S32> probe_level(gSavedSettings, "RenderReflectionProbeLevel", 3);
    if (!enabled || !view || !LLPipeline::sReflectionProbesEnabled || probe_level < 3)
    {
        clear();
        return;
    }

    // A probe only this holds was dropped by the manager, on a teleport or a reset.
    const size_t held_before = mHeld.size();
    mHeld.erase(std::remove_if(mHeld.begin(), mHeld.end(), [](const Held& h) { return h.mProbe->getNumRefs() == 1; }), mHeld.end());
    const bool dropped = mHeld.size() != held_before;

    if (gFrameTimeSeconds < mNextUpdate)
    {
        return;
    }
    mNextUpdate = gFrameTimeSeconds + UPDATE_PERIOD_S;

    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - room probes");
    const auto started = std::chrono::steady_clock::now();
    takeDiskReads();
    const std::shared_ptr<sknav::TileDiskCache> disk = navmesh.diskCache();
    const LL::WorkQueue::ptr_t workers = disk ? navmesh.workerQueue() : nullptr;
    std::set<U64> placed_keys;
    std::vector<U64> tile_keys;
    mCacheHits = mCachePlaced = 0;
    bool deferred = false;
    const auto budget_spent = [&started]()
    {
        return std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count() > PLACE_BUDGET_MS;
    };

    LLReflectionMapManager& manager = gPipeline.mReflectionMapManager;
    const sknav::ProbeParams params;
    const auto census = SKWorldCensus::instance().snapshot();
    // The candidates' solid test reads a tile lattice over the census's solid
    // records, rebuilt only when a new scan publishes: the per-candidate scan
    // over every record swept each body's whole triangle soup.
    if (census != mSolidsCensus)
    {
        mSolidsCensus = census;
        if (census)
        {
            mSolids.rebuild(census->mRecords);
        }
        else
        {
            mSolids.clear();
        }
    }

    std::vector<LLReflectionMap*> placed_probes;
    manager.getNavRoomsContext(placed_probes);
    std::vector<sknav::ProbeInfluence> placed;
    for (LLReflectionMap* probe : placed_probes)
    {
        LLViewerObject* vobj = probe->mViewerObject;
        sknav::ProbeInfluence inf;
        inf.mCentre.set(probe->mOrigin.getF32ptr());
        inf.mRadius = probe->mRadius;
        if (vobj->getReflectionProbeIsBox())
        {
            const LLQuaternion rot = vobj->getRenderRotation();
            inf.mBox = true;
            inf.mAxes[0] = LLVector3::x_axis * rot;
            inf.mAxes[1] = LLVector3::y_axis * rot;
            inf.mAxes[2] = LLVector3::z_axis * rot;
            inf.mHalf = vobj->getScale() * 0.5f;
        }
        placed.push_back(inf);
    }

    const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3d camera_global = gAgent.getPosGlobalFromAgent(camera);
    const LLVector3d reach(REACH_M, REACH_M, REACH_M);

    // Portals name each side by its slot in the mesh's spaces, so the
    // candidates gather under the same names: space id to slot, per mesh.
    std::map<LLUUID, std::map<U32, U32>> productId_to_space;
    for (const auto& entry : view->meshes())
    {
        if (!entry.mRooms) continue;
        std::map<U32, U32>& ids = productId_to_space[entry.mId];
        for (size_t i = 0; i < entry.mRooms->mSpaces.size(); ++i)
        {
            ids[entry.mRooms->mSpaces[i].mId] = static_cast<U32>(i);
        }
    }

    struct Candidate
    {
        LLVector3 mOrigin;
        F32 mRadius = 0.f;
        F32 mArea = 0.f;
        U32 mPart = 0;
        U32 mSpace = 0;         ///< the space's slot in its product, what portals name
        LLUUID mMesh;
        bool mBridge = false;   ///< a threshold's own sphere, not a part's
    };
    std::vector<Candidate> candidates;
    mClearedOut = 0;
    for (auto& p : mParts)
    {
        p.second.mSeen = false;
    }
    view->spacesIn(camera_global - reach, camera_global + reach, [&](const SKNavWorldView::Mesh& mesh, const sknav::Space& space)
    {
        if (!sknav::wantsProbes(space, params))
        {
            return;
        }
        // Over tiles that all have content keys the cache answers, and a key
        // the disk holds is read on a worker first, its space waiting a pass.
        navmesh.tileContentKeysIn(mesh.mId, mesh.mOrigin + LLVector3d(space.mMin), mesh.mOrigin + LLVector3d(space.mMax), tile_keys);
        const U64 key = sknav::probePartsKey(tile_keys, params);
        std::vector<sknav::ProbePart> cached;
        const std::vector<sknav::ProbePart>* space_parts = &cached;
        if (key)
        {
            if (mReading.count(key))
            {
                return;
            }
            if (workers && !mCache.hasKey(key) && !mMissed.count(key) && disk->contains(key))
            {
                mReading.insert(key);
                const std::shared_ptr<DiskReads> reads = mReads;
                workers->post([disk, reads, key]()
                {
                    std::vector<U8> bytes;
                    std::vector<sknav::CachedSpaceParts> spaces;
                    const bool ok = disk->load(key, bytes) && sknav::unpackProbeParts(bytes.data(), bytes.size(), key, spaces);
                    if (!ok && !bytes.empty())
                    {
                        disk->erase(key);   // stale or damaged: placed anew and rewritten
                    }
                    std::lock_guard<std::mutex> lock(reads->mMutex);
                    if (ok)
                    {
                        reads->mDone.emplace_back(key, std::move(spaces));
                    }
                    else
                    {
                        reads->mMissed.push_back(key);
                    }
                });
                return;
            }
            if (!mCache.hasKey(key) && budget_spent())
            {
                deferred = true;
                return;
            }
            bool placed = false;
            cached = mCache.partsFor(key, space, params, &placed);
            if (placed)
            {
                placed_keys.insert(key);
                ++mCachePlaced;
            }
            else
            {
                ++mCacheHits;
            }
        }
        else
        {
            Parts& parts = mParts[std::make_pair(mesh.mId, space.mId)];
            if (parts.mIndex < 0)
            {
                parts.mIndex = static_cast<S32>(mParts.size()) - 1;
            }
            parts.mSeen = true;
            // A space that places no parts is placed once too: an empty
            // answer under the same signature is as final as a full one.
            const U64 signature = signatureOf(space);
            if (parts.mSignature != signature || !parts.mPlaced)
            {
                if (budget_spent())
                {
                    deferred = true;    // last pass's parts stand until the next
                }
                else
                {
                    parts.mSignature = signature;
                    parts.mParts = sknav::spaceProbes(space, params);
                    parts.mPlaced = true;
                    ++mCachePlaced;
                }
            }
            space_parts = &parts.mParts;
        }
        const LLVector3 origin = gAgent.getPosAgentFromGlobal(mesh.mOrigin);
        for (const sknav::ProbePart& part : *space_parts)
        {
            Candidate c;
            c.mOrigin = origin + part.mOrigin;
            const LLVector3d global = gAgent.getPosGlobalFromAgent(c.mOrigin);
            if (mSolids.contains(global))
            {
                continue;
            }
            c.mRadius = sknav::clearRadius(c.mOrigin, std::max(part.mRadius, part.mSpaceRadius), placed, params);
            if (c.mRadius <= 0.f)
            {
                ++mClearedOut;
                continue;
            }
            c.mArea = part.mArea;
            c.mPart = part.mPart;
            c.mSpace = productId_to_space[mesh.mId][space.mId];
            c.mMesh = mesh.mId;
            candidates.push_back(c);
        }
    });
    for (auto it = mParts.begin(); it != mParts.end();)
    {
        it = it->second.mSeen ? std::next(it) : mParts.erase(it);
    }
    // Every key a space was placed under this pass is written whole, on a worker.
    for (U64 key : placed_keys)
    {
        const std::vector<sknav::CachedSpaceParts>* spaces = workers ? mCache.spaces(key) : nullptr;
        if (spaces)
        {
            workers->post([disk, key, copy = *spaces]() { disk->store(key, sknav::packProbeParts(copy, key)); });
            ++mDiskStores;
        }
    }

    // Every doorway where two spaces meet is a blend the shader must carry:
    // both sides' bubbles reach the threshold and past it, so they overlap
    // and the graph links them; a doorway into a space with no bubbles of
    // its own is served by its neighbour reaching through; and a threshold
    // neither side can reach, however far the octree cap lets them grow,
    // gets a small bubble of its own. The camera's reach bounds the work.
    {
        std::map<std::pair<LLUUID, U32>, std::vector<U32>> by_space;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            by_space[{candidates[i].mMesh, candidates[i].mSpace}].push_back(static_cast<U32>(i));
        }
        const auto link_pair = [&](U32 a, U32 b)
        {
            const F32 dx = candidates[a].mOrigin.mV[VX] - candidates[b].mOrigin.mV[VX];
            const F32 dy = candidates[a].mOrigin.mV[VY] - candidates[b].mOrigin.mV[VY];
            const F32 dz = candidates[a].mOrigin.mV[VZ] - candidates[b].mOrigin.mV[VZ];
            const F32 reach = candidates[a].mRadius + candidates[b].mRadius;
            return dx * dx + dy * dy + dz * dz < reach * reach;
        };
        const auto any_link = [&](const std::vector<U32>& as, const std::vector<U32>& bs)
        {
            for (U32 a : as)
            {
                for (U32 b : bs)
                {
                    if (link_pair(a, b)) return true;
                }
            }
            return false;
        };
        const auto nearest_of = [&](const std::vector<U32>& list, const LLVector3& p)
        {
            U32 best = 0;
            F32 best_d2 = 1e30f;
            for (U32 q : list)
            {
                const F32 d2 = dist_vec_squared(p, candidates[q].mOrigin);
                if (d2 < best_d2) { best_d2 = d2; best = q; }
            }
            return std::make_pair(best, best_d2);
        };
        std::vector<std::pair<LLUUID, U32>> bridged;
        view->portalsIn(camera_global - reach, camera_global + reach, [&](const SKNavWorldView::Mesh& mesh, const sknav::Portal& portal)
        {
            if (portal.mSpaces[0] >= productId_to_space[mesh.mId].size() ||
                portal.mSpaces[1] >= productId_to_space[mesh.mId].size())
            {
                return;
            }
            const U32 id_a = portal.mSpaces[0], id_b = portal.mSpaces[1];
            const std::vector<U32>& list_a = by_space[{mesh.mId, id_a}];
            const std::vector<U32>& list_b = by_space[{mesh.mId, id_b}];
            if (!list_a.empty() && !list_b.empty() && any_link(list_a, list_b))
            {
                return;     // already blends across
            }
            const LLVector3 centre = LLVector3(mesh.mOrigin + LLVector3d(portal.mCentre)) + LLVector3(0.f, 0.f, 1.f);
            bool grew = false;
            for (const std::vector<U32>* list : { &list_a, &list_b })
            {
                if (list->empty())
                {
                    continue;
                }
                const auto pair = nearest_of(*list, centre);
                Candidate& c = candidates[pair.first];
                const F32 need = std::sqrt(pair.second) + 2.f;  // past the mouth, generous
                const F32 cap = std::min(params.mMaxRadiusM, std::max(c.mRadius * params.mObligationShare, c.mRadius + 2.f));
                if (need > c.mRadius && need <= cap)
                {
                    c.mRadius = need;
                    grew = true;
                }
            }
            if (!grew || list_a.empty() || list_b.empty() || !any_link(list_a, list_b))
            {
                // A threshold nothing reaches: its own small bubble, matched
                // to later updates by origin like any part's.
                Candidate bridge;
                bridge.mOrigin = centre;
                bridge.mRadius = 3.f;
                for (const std::vector<U32>* list : { &list_a, &list_b })
                {
                    if (list->empty()) continue;
                    const auto pair = nearest_of(*list, bridge.mOrigin);
                    bridge.mRadius = std::max(bridge.mRadius, std::sqrt(pair.second) + 1.f);
                }
                bridge.mRadius = std::min(bridge.mRadius, 8.f);
                bridge.mMesh = mesh.mId;
                bridge.mSpace = id_a;
                bridge.mBridge = true;
                bridged.emplace_back(mesh.mId, id_a);
                std::vector<U32>& list = by_space[{mesh.mId, id_a}];
                list.push_back(static_cast<U32>(candidates.size()));
                candidates.push_back(bridge);
                ++mCandidatesAdded;
            }
        });
    }

    // Slots stand for candidates, built after the portal pass so a grown or
    // bridged sphere is budgeted at its final size.
    // The budget counts spheres per space, and slots repeat across meshes.
    std::map<std::pair<LLUUID, U32>, U32> space_numbers;
    std::vector<sknav::ProbeSlot> slots;
    for (const Candidate& c : candidates)
    {
        sknav::ProbeSlot slot;
        slot.mKey = slots.size();
        slot.mSpace = space_numbers.emplace(std::make_pair(c.mMesh, c.mSpace), static_cast<U32>(space_numbers.size())).first->second;
        slot.mPart = c.mPart;
        slot.mOrigin = c.mOrigin;
        slot.mRadius = c.mRadius;
        slot.mArea = c.mArea;
        slot.mDistance = std::max(dist_vec(camera, c.mOrigin) - c.mRadius, 0.f);
        slots.push_back(slot);
    }

    // Parts renumbered by a rebuild keep their probes: match by nearest origin.
    std::vector<LLVector3> held_origins, origins;
    for (const Held& h : mHeld)
    {
        held_origins.push_back(gAgent.getPosAgentFromGlobal(h.mOrigin));
    }
    for (const Candidate& c : candidates)
    {
        origins.push_back(c.mOrigin);
    }
    const std::vector<S32> match = sknav::matchNearest(held_origins, origins, params.mMatchM);
    for (sknav::ProbeSlot& slot : slots)
    {
        slot.mHeld = match[slot.mKey] >= 0;
    }

    // The probe array's cap, less the default probe and every placed one.
    const U32 room = std::max(static_cast<S32>(manager.probeCount()) - 1 - static_cast<S32>(placed.size()), 0);
    const U32 cap = std::min<U32>(max_probes, std::min<U32>(room, LL_MAX_REFLECTION_PROBE_COUNT - 1));
    mCandidates = static_cast<U32>(slots.size());
    sknav::budgetProbes(slots, cap, params);
    mOverBudget = mCandidates - static_cast<U32>(slots.size());

    // One real move per update, so re-captures are staggered and placed probes are never starved.
    bool moved = false;
    bool created = false;
    std::vector<Held> held;
    for (const sknav::ProbeSlot& slot : slots)
    {
        const Candidate& c = candidates[slot.mKey];
        const S32 m = match[slot.mKey];
        Held h;
        if (m >= 0)
        {
            h = mHeld[m];
            if (!moved && sknav::shouldReplace(held_origins[m], h.mRadius, c.mOrigin, c.mRadius, params))
            {
                moved = true;
                h.mProbe->mOrigin.load3(c.mOrigin.mV);
                h.mProbe->mRadius = c.mRadius;
                manager.invalidateNavRoomsProbe(h.mProbe);
                h.mOrigin = gAgent.getPosGlobalFromAgent(c.mOrigin);
                h.mRadius = c.mRadius;
                ++mMoves;
            }
        }
        else
        {
            h.mProbe = manager.registerNavRoomsProbe(c.mOrigin, c.mRadius);
            if (h.mProbe.isNull())
            {
                continue;
            }
            h.mOrigin = gAgent.getPosGlobalFromAgent(c.mOrigin);
            h.mRadius = c.mRadius;
            ++mCreated;
            created = true;
        }
        held.push_back(h);
    }
    if (deferred)
    {
        // A space the budget put off has no candidates this pass: its
        // probes stay up rather than drop and come back a frame later.
        for (size_t i = 0; i < mHeld.size(); ++i)
        {
            if (std::find(match.begin(), match.end(), static_cast<S32>(i)) == match.end())
            {
                held.push_back(mHeld[i]);
            }
        }
        mNextUpdate = gFrameTimeSeconds;
    }
    mHeld.swap(held);
    mPlaced = static_cast<U32>(mHeld.size());
    // Only a change in what the held spheres cover has to re-open the grid
    // placement's questions: an idle pass must not send the manager re-testing
    // every probe every second.
    if (dropped || created || moved || mHeld.size() != held_before)
    {
        manager.invalidateAutomaticPlacement();
    }
    mLastMs = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
}

std::string SKNavRoomProbes::statsLine() const
{
    const LLReflectionMapManager& manager = gPipeline.mReflectionMapManager;
    return llformat("room probes %u of %u parts (%u clear of placed, %u dropped by the budget), %u created, %u moves, "
                    "%u nav-octree links, %u relinked, cache %u hits %u placed, %u keys %u parts, %u loads %u stores, %.2f ms",
                    mPlaced, mCandidates + mClearedOut, mClearedOut, mOverBudget, mCreated, mMoves,
                    manager.countNavOctreeLinks(), manager.getRelevanceRelinks(), mCacheHits, mCachePlaced,
                    static_cast<U32>(mCache.keys()), static_cast<U32>(mCache.parts()), mDiskLoads, mDiskStores, mLastMs);
}
