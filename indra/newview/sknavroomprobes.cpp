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
#include "llapp.h"
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

SKNavRoomProbes::SKNavRoomProbes() : mResults(std::make_shared<WorkerResults>())
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
    mPlacing.clear();
    mSolids.clear();
    mSolidsCensus = nullptr;
    mCandidates = mClearedOut = mOverBudget = mPlaced = mCandidatesAdded = 0;
}

void SKNavRoomProbes::takeWorkerResults()
{
    std::vector<std::pair<U64, std::vector<sknav::CachedSpaceParts>>> done;
    std::vector<U64> missed;
    std::vector<Placed> placed;
    {
        std::lock_guard<std::mutex> lock(mResults->mMutex);
        done.swap(mResults->mDone);
        missed.swap(mResults->mMissed);
        placed.swap(mResults->mPlaced);
    }
    for (Placed& p : placed)
    {
        mPlacing.erase(std::make_pair(p.mMesh, p.mSpace));
        if (p.mKey)
        {
            mCache.insert(p.mKey, p.mSpace, p.mAnchor, p.mParts);
            mStoreKeys.insert(p.mKey);
        }
        else
        {
            Parts& parts = mParts[std::make_pair(p.mMesh, p.mSpace)];
            parts.mSignature = p.mSignature;
            parts.mParts = std::move(p.mParts);
            parts.mPlaced = true;
        }
        ++mCachePlaced;
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
    mCacheHits = mCachePlaced = 0;
    takeWorkerResults();
    const std::shared_ptr<sknav::TileDiskCache> disk = navmesh.diskCache();
    const LL::WorkQueue::ptr_t workers = navmesh.workerQueue();
    std::set<U64> placed_keys;
    placed_keys.swap(mStoreKeys);
    std::vector<U64> tile_keys;
    // Placement is pure work over a space's raster, tens of milliseconds a
    // space: it runs on the workers, and a space whose answer is pending
    // keeps the parts it had, or waits, until the result is taken.
    bool deferred = false;
    const sknav::ProbeParams params;
    const auto place = [&](const LLUUID& mesh, const sknav::Space& space, U64 key)
    {
        if (mPlacing.insert(std::make_pair(mesh, space.mId)).second)
        {
            const std::shared_ptr<WorkerResults> results = mResults;
            workers->post([results, mesh, key, copy = space, params]()
            {
                if (LLApp::isExiting()) return;   // the pool drains before it joins
                Placed p;
                p.mKey = key;
                p.mMesh = mesh;
                p.mSpace = copy.mId;
                p.mAnchor = copy.mAnchor;
                p.mSignature = signatureOf(copy);
                p.mParts = sknav::spaceProbes(copy, params);
                std::lock_guard<std::mutex> lock(results->mMutex);
                results->mPlaced.push_back(std::move(p));
            });
        }
        deferred = true;
    };

    LLReflectionMapManager& manager = gPipeline.mReflectionMapManager;
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
        U64 mSource = 0;        ///< what its space was placed from, 0 for a bridge
    };
    std::vector<Candidate> candidates;
    std::vector<std::pair<LLVector3d, LLVector3d>> pending;    ///< spaces still being read or placed, global
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
        const LLVector3d space_min = mesh.mOrigin + LLVector3d(space.mMin);
        const LLVector3d space_max = mesh.mOrigin + LLVector3d(space.mMax);
        navmesh.tileContentKeysIn(mesh.mId, space_min, space_max, tile_keys);
        const U64 key = sknav::probePartsKey(tile_keys, params);
        std::vector<sknav::ProbePart> cached;
        const std::vector<sknav::ProbePart>* space_parts = &cached;
        U64 source = key;
        if (key)
        {
            if (mReading.count(key))
            {
                deferred = true;
                pending.emplace_back(space_min, space_max);
                return;
            }
            if (disk && !mCache.hasKey(key) && !mMissed.count(key) && disk->contains(key))
            {
                mReading.insert(key);
                const std::shared_ptr<WorkerResults> reads = mResults;
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
                deferred = true;
                pending.emplace_back(space_min, space_max);
                return;
            }
            const std::vector<sknav::ProbePart>* held = mCache.find(key, space.mId, space.mAnchor);
            if (!held)
            {
                place(mesh.mId, space, key);
                pending.emplace_back(space_min, space_max);
                return;
            }
            cached = *held;
            ++mCacheHits;
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
                place(mesh.mId, space, 0);  // last pass's parts stand meanwhile
            }
            space_parts = &parts.mParts;
            source = parts.mSignature;
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
            c.mSource = source;
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
        const std::vector<sknav::CachedSpaceParts>* spaces = disk ? mCache.spaces(key) : nullptr;
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
    std::vector<Held> retiring;
    for (const sknav::ProbeSlot& slot : slots)
    {
        const Candidate& c = candidates[slot.mKey];
        const S32 m = match[slot.mKey];
        Held h;
        if (m >= 0)
        {
            h = mHeld[m];
            h.mRetiring = false;
            h.mWaitFor = nullptr;
            // Kept over a space placed anew from other content -- a wall gone, a doorway opened:
            // its capture stands, and is retaken in place when the refresh rotation comes round.
            if (h.mSource && c.mSource && h.mSource != c.mSource)
            {
                manager.staleNavRoomsProbe(h.mProbe);
                ++mStaled;
            }
            if (c.mSource)
            {
                h.mSource = c.mSource;
            }
            // A capture taken from inside what is now a solid shows its inside: always retake it.
            const bool buried = mSolids.contains(h.mOrigin);
            if (!moved && (buried || sknav::shouldRecapture(held_origins[m], h.mRadius, c.mOrigin, params)))
            {
                // Make before break: the old sphere keeps its capture until the new one has its own.
                LLPointer<LLReflectionMap> fresh = manager.registerNavRoomsProbe(c.mOrigin, c.mRadius);
                if (fresh.notNull())
                {
                    moved = true;
                    Held old = h;
                    old.mRetiring = true;
                    old.mWaitFor = fresh;
                    retiring.push_back(old);
                    h.mProbe = fresh;
                    h.mOrigin = gAgent.getPosGlobalFromAgent(c.mOrigin);
                    h.mRadius = c.mRadius;
                    ++mMoves;
                    created = true;
                }
            }
            else if (sknav::shouldReplace(held_origins[m], h.mRadius, held_origins[m], c.mRadius, params))
            {
                // Close enough to keep its capture: only the sphere it reaches over follows the part.
                manager.resizeNavRoomsProbe(h.mProbe, c.mRadius);
                h.mRadius = c.mRadius;
                ++mResizes;
                created = true;
            }
        }
        else
        {
            h.mProbe = manager.registerNavRoomsProbe(c.mOrigin, c.mRadius);
            if (h.mProbe.isNull())
            {
                continue;
            }
            h.mSource = c.mSource;
            h.mOrigin = gAgent.getPosGlobalFromAgent(c.mOrigin);
            h.mRadius = c.mRadius;
            ++mCreated;
            created = true;
        }
        held.push_back(h);
    }
    // A probe that lost its candidate -- its space being read or placed, or out
    // of the view while its tiles and rooms rebuild -- retires rather than
    // goes: it stays matchable, so the same parts coming back take it up again
    // capture intact, and it lets go only once its area has settled and every
    // probe now covering it, or the one it moved to, has finished capturing.
    for (size_t i = 0; i < mHeld.size(); ++i)
    {
        if (std::find(match.begin(), match.end(), static_cast<S32>(i)) == match.end())
        {
            Held h = mHeld[i];
            if (!h.mRetiring)
            {
                h.mRetiring = true;
                h.mWaitFor = nullptr;
            }
            retiring.push_back(h);
        }
    }
    const auto captured = [](const LLReflectionMap* probe) { return probe->mComplete && probe->mFadeIn >= 1.f; };
    for (Held& r : retiring)
    {
        if ((r.mOrigin - camera_global).magVec() > REACH_M + r.mRadius)
        {
            continue;
        }
        if (r.mWaitFor.notNull())
        {
            const bool still_held = std::any_of(held.begin(), held.end(), [&](const Held& h) { return h.mProbe == r.mWaitFor; });
            if (still_held && captured(r.mWaitFor))
            {
                continue;
            }
            if (still_held)
            {
                held.push_back(r);
                continue;
            }
            r.mWaitFor = nullptr;   // its successor went too: judge it by its area
        }
        const LLVector3d span(r.mRadius, r.mRadius, r.mRadius);
        const LLVector3d lo = r.mOrigin - span, hi = r.mOrigin + span;
        const bool unsettled = navmesh.settlingIn(lo, hi) ||
            std::any_of(pending.begin(), pending.end(), [&](const std::pair<LLVector3d, LLVector3d>& box)
            {
                return box.first.mdV[VX] <= hi.mdV[VX] && lo.mdV[VX] <= box.second.mdV[VX] &&
                       box.first.mdV[VY] <= hi.mdV[VY] && lo.mdV[VY] <= box.second.mdV[VY] &&
                       box.first.mdV[VZ] <= hi.mdV[VZ] && lo.mdV[VZ] <= box.second.mdV[VZ];
            });
        const bool covered_uncaptured = std::any_of(held.begin(), held.end(), [&](const Held& h)
        {
            return !h.mRetiring && !captured(h.mProbe) && (h.mOrigin - r.mOrigin).magVec() < h.mRadius;
        });
        if (unsettled || covered_uncaptured)
        {
            held.push_back(r);
        }
    }
    if (deferred)
    {
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

bool SKNavRoomProbes::pendingFor(const LLUUID& mesh) const
{
    const auto it = mPlacing.lower_bound(std::make_pair(mesh, 0u));
    return it != mPlacing.end() && it->first == mesh;
}

std::string SKNavRoomProbes::statsLine() const
{
    const LLReflectionMapManager& manager = gPipeline.mReflectionMapManager;
    return llformat("room probes %u of %u parts (%u clear of placed, %u dropped by the budget), %u created, %u moves, %u resizes, %u staled, "
                    "%u nav-octree links, %u relinked, cache %u hits %u placed, %u keys %u parts, %u loads %u stores, %.2f ms",
                    mPlaced, mCandidates + mClearedOut, mClearedOut, mOverBudget, mCreated, mMoves, mResizes, mStaled,
                    manager.countNavOctreeLinks(), manager.getRelevanceRelinks(), mCacheHits, mCachePlaced,
                    static_cast<U32>(mCache.keys()), static_cast<U32>(mCache.parts()), mDiskLoads, mDiskStores, mLastMs);
}
