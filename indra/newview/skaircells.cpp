/**
 * @file skaircells.cpp
 * @brief The air cells around the agent and the camera: one mesh per airborne cluster and per region, built tile by tile on a pool.
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

#include "skaircells.h"

#include "alobjectproperties.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"
#include "skairdebug.h"
#include "skairevict.h"
#include "skairlayout.h"
#include "skcensusrecord.h"
#include "skworldpalette.h"
#include "skworldtrace.h"
#include "workqueue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_set>

using namespace skworld;

namespace
{
    const char* causeName(U8 cause)
    {
        static const char* names[] = { "none", "created", "records", "terrain", "params", "failed" };
        return cause < LL_ARRAY_SIZE(names) ? names[cause] : "?";
    }

    /// The trace's line for one tile, with where it is and why it last built.
    template <typename T> void traceTile(const char* type, const LLUUID& mesh, const TileKey& key, const T& tile)
    {
        SKWorldTrace::Line(type).mesh(mesh).tile(key.mX, key.mY).box(tile.mMin, tile.mMax).str("cause", causeName(tile.mCause));
    }

    /// Destroy a large value on the pool rather than on the main thread.
    template <typename T> void retireOn(const LL::WorkQueue::ptr_t& workers, T value)
    {
        auto held = std::make_shared<T>(std::move(value));
        if (!workers || !workers->tryPost([held]() { *held = {}; }))
        {
            *held = {};
        }
    }

    bool overlaps3D(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax)
    {
        return overlapsXY(amin, amax, bmin, bmax) && !(amin.mdV[VZ] > bmax.mdV[VZ] || bmin.mdV[VZ] > amax.mdV[VZ]);
    }

    // Squared XY distance from a point to a box, 0 inside it.
    F64 distanceXYSquared(const LLVector3d& lo, const LLVector3d& hi, const LLVector3d& point)
    {
        const F64 dx = llmax(lo.mdV[VX] - point.mdV[VX], 0.0, point.mdV[VX] - hi.mdV[VX]);
        const F64 dy = llmax(lo.mdV[VY] - point.mdV[VY], 0.0, point.mdV[VY] - hi.mdV[VY]);
        return dx * dx + dy * dy;
    }

    LLVector3d cameraGlobal()
    {
        return gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    }
}

SKAirCells::SKAirCells()
{
}

SKAirCells::~SKAirCells()
{
}

bool SKAirCells::enabled()
{
    static LLCachedControl<bool> air(gSavedSettings, "SKAirCellsEnabled", true);
    return air;
}

bool SKAirCells::overlayActive()
{
    if (!enabled() || !gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WORLD))
    {
        return false;
    }
    static LLCachedControl<U32> air(gSavedSettings, "SKAirOverlay", 0);
    static LLCachedControl<bool> faces(gSavedSettings, "SKAirOverlayFaces", false);
    static LLCachedControl<bool> runoff(gSavedSettings, "SKAirOverlayRunoff", false);
    static LLCachedControl<bool> sky(gSavedSettings, "SKAirOverlaySky", false);
    static LLCachedControl<bool> updates(gSavedSettings, "SKAirOverlayUpdates", true);
    return air != 0 || faces || runoff || sky || updates;
}

bool SKAirCells::wanted() const
{
    static LLCachedControl<bool> always(gSavedSettings, "SKAirAlways", true);
    return enabled() && (always || overlayActive() || !mConsumers.empty());
}

void SKAirCells::addDemand(const std::string& who)
{
    mConsumers.insert(who);
}

void SKAirCells::removeDemand(const std::string& who)
{
    mConsumers.erase(who);
}

void SKAirCells::requestAir(const std::string& who, const LLVector3d& min, const LLVector3d& max) const
{
    mAirRequests[who] = { min, max, LLTimer::getTotalSeconds() };
}

SKAirCells::AirReach SKAirCells::airReach() const
{
    static LLCachedControl<F32> radius(gSavedSettings, "SKAirRadius", 300.f);
    static LLCachedControl<F32> agent_radius(gSavedSettings, "SKAirAgentRadius", 30.f);
    static LLCachedControl<F32> hold(gSavedSettings, "SKAirRequestHoldSeconds", 20.f);
    AirReach reach;
    reach.mCamera = cameraGlobal();
    reach.mAgent = gAgent.getPositionGlobal();
    const F64 r = radius, agent_r = agent_radius;
    reach.mRadius2 = r * r;
    reach.mAgentRadius2 = agent_r * agent_r;
    reach.mBoxes = mHomeBoxes;
    const F64 now = LLTimer::getTotalSeconds();
    for (const auto& request : mAirRequests)
    {
        if (now - request.second.mAt < hold)
        {
            reach.mBoxes.emplace_back(request.second.mMin, request.second.mMax);
        }
    }
    return reach;
}

bool SKAirCells::inAirRadius(const Tile& tile, const AirReach& reach) const
{
    if (distanceXYSquared(tile.mMin, tile.mMax, reach.mCamera) <= reach.mRadius2
        || distanceXYSquared(tile.mMin, tile.mMax, reach.mAgent) <= reach.mAgentRadius2)
    {
        return true;
    }
    for (const auto& box : reach.mBoxes)
    {
        if (overlapsXY(box.first, box.second, tile.mMin, tile.mMax))
        {
            return true;
        }
    }
    return false;
}

U8 SKAirCells::meshPriority(const LLUUID& mesh) const
{
    const auto it = mMeshes.find(mesh);
    return it == mMeshes.end() ? 3 : meshPriority(*it->second);
}

U8 SKAirCells::meshPriority(const Mesh& mesh) const
{
    LLViewerRegion* current = gAgent.getRegion();
    if (!current)
    {
        return 3;
    }
    if (mesh.mTerrain && mesh.mId == current->getRegionID())
    {
        return 0;
    }
    if (!mesh.mTerrain && mesh.mRegion == current->getHandle())
    {
        return 2;
    }
    if (!mesh.mTerrain && mesh.mRegion == 0)
    {
        return 3;
    }
    // A mesh of a touching region builds with the regions; anything past them waits for them.
    if (LLWorld* world = LLWorld::getInstance())
    {
        const LLVector3d here = current->getOriginGlobal();
        for (LLViewerRegion* region : world->getRegionList())
        {
            if (!region || (mesh.mTerrain ? region->getRegionID() != mesh.mId : region->getHandle() != mesh.mRegion))
            {
                continue;
            }
            const LLVector3d d = region->getOriginGlobal() - here;
            const F64 width = llmax(region->getWidth(), current->getWidth());
            const bool touching = std::fabs(d.mdV[VX]) <= width && std::fabs(d.mdV[VY]) <= width;
            return touching ? (mesh.mTerrain ? 1 : 2) : 3;
        }
    }
    return 3;
}

void SKAirCells::findHomeBoxes(const SKCensusSnapshotPtr& snap)
{
    static LLCachedControl<bool> whole(gSavedSettings, "SKAirWholeCluster", true);
    mHomeBoxes.clear();
    if (!whole || !snap)
    {
        return;
    }
    std::vector<ClusterPtr> around;
    snap->mClusters.at(gAgent.getPositionGlobal(), around);
    for (const ClusterPtr& c : around)
    {
        mHomeBoxes.emplace_back(c->mMin, c->mMax);
    }
}

LL::WorkQueue::ptr_t SKAirCells::workerQueue()
{
    if (!mWorkers)
    {
        // A pool of its own: the shared "General" pool is one thread in a
        // normal session, and a tile build is pure and independent, so one per
        // core is the shape that fits, leaving a couple for the viewer itself.
        static LLCachedControl<S32> setting(gSavedSettings, "SKAirWorkerThreads", 0);
        const S32 cores = static_cast<S32>(std::thread::hardware_concurrency());
        const S32 width = llclamp(setting > 0 ? static_cast<S32>(setting) : cores - 2, 1, 16);
        mWorkers = std::make_unique<LL::ThreadPool>("AirCells", width);
        mWorkers->start();
        LL_INFOS("SKAir") << "air cells pool: " << mWorkers->getWidth() << " threads on " << cores << " cores" << LL_ENDL;
    }
    return LL::WorkQueue::getInstance("AirCells");
}

void SKAirCells::cleanupSingleton()
{
    // Drop what is queued and join the pool before the singleton dies: a job
    // in flight carries this pointer in its publish callback.
    if (mWorkers)
    {
        mWorkers->close();
        mWorkers.reset();
    }
    if (SKWorldCensus::instanceExists())
    {
        SKWorldCensus::instance().removeCensusSection("air.params");
        if (mDemanding)
        {
            SKWorldCensus::instance().removeDemand("air");
        }
    }
    mMeshes.clear();
}

void SKAirCells::evictAir()
{
    static LLCachedControl<U32> budget_mb(gSavedSettings, "SKAirBudgetMB", 512);
    static LLCachedControl<F32> radius(gSavedSettings, "SKAirRadius", 300.f);
    auto workers = workerQueue();
    if (mEvictionJob.valid())
    {
        if (mEvictionJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        mPendingEvictions = mEvictionJob.get();
        mNextEviction = 0;
    }
    if (mPendingEvictions)
    {
        const AirReach reach = airReach();
        LLTimer timer;
        while (mNextEviction < mPendingEvictions->size())
        {
            if (timer.getElapsedTimeF32() > 0.001f) return;
            const auto& candidate = (*mPendingEvictions)[mNextEviction++];
            const auto mesh = mMeshes.find(candidate.mMesh);
            if (mesh == mMeshes.end() || mesh->second->mGeneration != candidate.mGeneration) continue;
            const auto found = mesh->second->mTiles.find(candidate.mKey);
            if (found == mesh->second->mTiles.end()) continue;
            Tile& tile = found->second;
            if (tile.mInFlight || tile.mAir != candidate.mAir || inAirRadius(tile, reach)) continue;
            tile.mAirCoarse = candidate.mCoarse;
            tile.mAir.reset();
            tile.mAirBuilt = 0;
            ++mStats.mAirEvicted;
            if (SKWorldTrace::tracing())
            {
                traceTile("air.evict", candidate.mMesh, candidate.mKey, tile);
            }
            noteAirChange(AirChange::EVICTED, *mesh->second, candidate.mKey, tile);
            bumpRevision("air.evict");
        }
        auto retired = std::move(mPendingEvictions);
        retireOn(workers, std::move(retired));
    }
    if (!mEvictionScan) mEvictionScan = std::make_shared<std::vector<AirCandidate>>();
    const LLVector3d camera = cameraGlobal();
    const LLVector3d agent = gAgent.getPositionGlobal();
    const F64 dt = llclamp(static_cast<F64>(gFrameIntervalSeconds), 0.0, 1.0);
    if (!mEvictionScanReady && !scanTiles(mEvictionCursor, 0.001f, [&](Mesh& mesh, const TileKey& key, Tile& tile)
        {
            if (!tile.mAir || tile.mInFlight) return;
            // The nearer of the camera and the agent, so the agent's disc keeps what inAirRadius holds.
            const F64 distance = std::sqrt(llmin(distanceXYSquared(tile.mMin, tile.mMax, camera), distanceXYSquared(tile.mMin, tile.mMax, agent)));
            tile.mNearness = accumulateNearness(tile.mNearness, distance, radius, dt);
            mEvictionScan->push_back({mesh.mId, mesh.mGeneration, key, tile.mAir, nullptr, tile.mNearness, distance, meshPriority(mesh)});
        })) return;
    mEvictionScanReady = true;
    auto held = mEvictionScan;
    const size_t budget = static_cast<size_t>(budget_mb) * 1048576;
    const F64 protect = 1.5 * radius;
    const size_t live = gAirLiveBytes.load();
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<std::vector<AirCandidate>>()>>([held, budget, protect, live]()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - evict air worker");
        std::vector<EvictionCandidate> candidates;
        candidates.reserve(held->size());
        size_t scanned = 0;
        // A priority's 0 is what the budget gives up last.
        for (const auto& tile : *held)
        {
            candidates.push_back({tile.mAir->bytes(), tile.mNearness, tile.mDistance, static_cast<U8>(3 - tile.mPriority)});
            scanned += candidates.back().mBytes;
        }
        // The budget holds the live air: what the tiles do not point at, coarse air and what jobs hold, comes off it first.
        const size_t elsewhere = live > scanned ? live - scanned : 0;
        auto result = std::make_shared<std::vector<AirCandidate>>();
        for (size_t index : chooseEvictions(candidates, budget > elsewhere ? budget - elsewhere : 0, protect))
        {
            auto candidate = (*held)[index];
            candidate.mCoarse = coarseAir(*candidate.mAir);
            result->push_back(std::move(candidate));
        }
        return result;
    });
    auto future = task->get_future();
    if (workers->tryPost([task]() { (*task)(); }))
    {
        mEvictionJob = std::move(future);
        mEvictionScan.reset();
        mEvictionScanReady = false;
    }
}

void SKAirCells::fillParams(TileInput& input) const
{
    static LLCachedControl<F32> cell(gSavedSettings, "SKAirCell", 0.125f);
    static LLCachedControl<F32> climb(gSavedSettings, "SKAirClimb", 0.75f);
    static LLCachedControl<F32> slope(gSavedSettings, "SKAirSlope", 74.f);
    static LLCachedControl<bool> walls(gSavedSettings, "SKAirWalls", true);
    static LLCachedControl<F32> runoff_max(gSavedSettings, "SKAirRunoffMax", 24.f);
    static LLCachedControl<F32> wall_min(gSavedSettings, "SKAirWallMinHeight", 0.5f);
    static LLCachedControl<F32> min_gap(gSavedSettings, "SKAirMinGap", 0.5f);

    input.mCell = llclamp(static_cast<F32>(cell), 0.05f, 1.f);
    input.mWallMinHeight = llclamp(static_cast<F32>(wall_min), 0.f, 8.f);
    // As tall as the user's own avatar, rounded up to the next cell, so a tall
    // avatar never gets floor it cannot stand on.
    F32 height = 2.f;
    if (isAgentAvatarValid())
    {
        height = llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f);
    }
    height = std::ceil(height / input.mCell) * input.mCell;
    input.mAvatar = { height, ClassParams().mRadius, climb, llclamp(static_cast<F32>(slope), 1.f, 89.9f) };
    input.mTopParams.mClimb = climb;
    // The air is read off the raster; its pad is metres, whatever the cell.
    input.mAir = true;
    input.mAirPad = airPadCells(input.mCell);
    input.mAirMinGap = llclamp(static_cast<F32>(min_gap), 0.f, 4.f);
    input.mWallFaces = walls;
    input.mRunoff = walls;
    input.mRunoffMax = runoff_max;
}

TileInput SKAirCells::params() const
{
    TileInput input;
    input.mTileSize = static_cast<F32>(TILE_M);
    fillParams(input);
    return input;
}

U64 SKAirCells::paramsSignature() const
{
    const TileInput probe = params();
    U64 h = SEED;
    h = mixF32(h, probe.mCell);
    h = mixF32(h, probe.mWallMinHeight);
    h = mix(h, probe.mWallFaces);
    h = mix(h, probe.mRunoff);
    h = mixF32(h, probe.mRunoffMax);
    h = mixF32(h, probe.mTopParams.mHeight);
    h = mixF32(h, probe.mTopParams.mSlope);
    h = mixF32(h, probe.mTopParams.mClimb);
    h = mix(h, static_cast<U64>(probe.mAirPad));
    h = mix(h, probe.mAir);
    h = mixF32(h, probe.mAirMinGap);
    h = mixF32(h, probe.mAvatar.mHeight);
    h = mixF32(h, probe.mAvatar.mRadius);
    h = mixF32(h, probe.mAvatar.mClimb);
    h = mixF32(h, probe.mAvatar.mSlope);
    return h;
}

U64 SKAirCells::terrainSignature(const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad) const
{
    // Every patch under the padded tile, by its last update and height range:
    // exact and cheap, and it never churns.
    U64 h = SEED;
    const LLVector3d pmin = tile_min - LLVector3d(pad, pad, 0), pmax = tile_max + LLVector3d(pad, pad, 0);
    LLWorld* world = LLWorld::getInstance();
    std::vector<LLViewerRegion*> regions;
    const LLVector3d corners[5] = { pmin, LLVector3d(pmax.mdV[VX], pmin.mdV[VY], 0), LLVector3d(pmin.mdV[VX], pmax.mdV[VY], 0), pmax, (pmin + pmax) * 0.5 };
    for (const LLVector3d& c : corners)
    {
        LLViewerRegion* r = world->getRegionFromPosGlobal(c);
        if (r && std::find(regions.begin(), regions.end(), r) == regions.end())
        {
            regions.push_back(r);
        }
    }
    for (LLViewerRegion* region : regions)
    {
        const LLVector3d& o = region->getOriginGlobal();
        const F64 w = region->getWidth();
        LLSurface& land = region->getLand();
        const S32 per_edge = land.getPatchesPerEdge();
        const F64 patch_m = w / per_edge;
        const S32 x0 = llclamp(static_cast<S32>(std::floor((pmin.mdV[VX] - o.mdV[VX]) / patch_m)), 0, per_edge - 1);
        const S32 x1 = llclamp(static_cast<S32>(std::floor((pmax.mdV[VX] - o.mdV[VX]) / patch_m)), 0, per_edge - 1);
        const S32 y0 = llclamp(static_cast<S32>(std::floor((pmin.mdV[VY] - o.mdV[VY]) / patch_m)), 0, per_edge - 1);
        const S32 y1 = llclamp(static_cast<S32>(std::floor((pmax.mdV[VY] - o.mdV[VY]) / patch_m)), 0, per_edge - 1);
        h = mix(h, region->getHandle());
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                if (const LLSurfacePatch* patch = land.getPatch(x, y))
                {
                    h = mix(h, static_cast<U64>(patch->getUpdateCount()));
                    h = mixF32(h, patch->getMinZ());
                    h = mixF32(h, patch->getMaxZ());
                }
            }
        }
        h = mixF32(h, region->getWaterHeight());
    }
    return h;
}

U64 SKAirCells::landStamp(LLViewerRegion& region) const
{
    // The whole region's land in one value, so a tile can tell in one compare that nothing under it moved.
    LLSurface& land = region.getLand();
    const S32 per_edge = land.getPatchesPerEdge();
    U64 h = mix(SEED, region.getHandle());
    for (S32 y = 0; y < per_edge; ++y)
    {
        for (S32 x = 0; x < per_edge; ++x)
        {
            if (const LLSurfacePatch* patch = land.getPatch(x, y))
            {
                h = mix(h, static_cast<U64>(patch->getUpdateCount()));
                h = mixF32(h, patch->getMinZ());
                h = mixF32(h, patch->getMaxZ());
            }
        }
    }
    return mixF32(h, region.getWaterHeight());
}

U64 SKAirCells::worldStamp() const
{
    // Which regions there are: a tile's window reaches past its own region's edge.
    U64 h = SEED;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        h = mix(h, region->getHandle());
    }
    return h;
}

void SKAirCells::fillTerrain(const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad, TerrainWindow& out) const
{
    const F32 spacing = 1.f;
    const LLVector3d start(tile_min.mdV[VX] - pad, tile_min.mdV[VY] - pad, 0.0);
    const S32 n = static_cast<S32>(std::ceil((tile_max.mdV[VX] - tile_min.mdV[VX] + 2 * pad) / spacing)) + 1;
    out.mOrigin.set(static_cast<F32>(start.mdV[VX] - mesh_origin.mdV[VX]), static_cast<F32>(start.mdV[VY] - mesh_origin.mdV[VY]), 0.f);
    out.mSpacing = spacing;
    out.mWidth = n;
    out.mHeight = n;
    out.mZ.assign(n * n, 0.f);
    bool any = false;
    LLWorld* world = LLWorld::getInstance();
    LLViewerRegion* centre = world->getRegionFromPosGlobal((tile_min + tile_max) * 0.5);
    out.mWater = centre ? centre->getWaterHeight() - static_cast<F32>(mesh_origin.mdV[VZ]) : -1e9f;
    LLViewerRegion* region = nullptr;
    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const LLVector3d p(start.mdV[VX] + x * spacing, start.mdV[VY] + y * spacing, 0.0);
            // Regions are squares; the last one answers most samples.
            if (!region || p.mdV[VX] < region->getOriginGlobal().mdV[VX] || p.mdV[VX] >= region->getOriginGlobal().mdV[VX] + region->getWidth() ||
                p.mdV[VY] < region->getOriginGlobal().mdV[VY] || p.mdV[VY] >= region->getOriginGlobal().mdV[VY] + region->getWidth())
            {
                region = world->getRegionFromPosGlobal(p);
            }
            if (region)
            {
                out.mZ[y * n + x] = region->getLand().resolveHeightGlobal(p) - static_cast<F32>(mesh_origin.mdV[VZ]);
                any = true;
            }
            else
            {
                // The void: a deep floor keeps the window rectangular without offering anything to stand on.
                out.mZ[y * n + x] = -1000.f;
            }
        }
    }
    if (!any)
    {
        out.mWidth = out.mHeight = 0;
        out.mZ.clear();
    }
}

bool SKAirCells::scanTiles(TileCursor& cursor, F32 seconds, const std::function<void(Mesh&, const TileKey&, Tile&)>& visit)
{
    LLTimer timer;
    auto mesh = cursor.mStarted ? mMeshes.lower_bound(cursor.mMesh) : mMeshes.begin();
    for (; mesh != mMeshes.end(); ++mesh)
    {
        auto& tiles = mesh->second->mTiles;
        auto tile = cursor.mStarted && mesh->first == cursor.mMesh ? tiles.upper_bound(cursor.mKey) : tiles.begin();
        for (; tile != tiles.end(); ++tile)
        {
            cursor.mMesh = mesh->first;
            cursor.mKey = tile->first;
            cursor.mStarted = true;
            visit(*mesh->second, tile->first, tile->second);
            if (timer.getElapsedTimeF32() >= seconds) return false;
        }
    }
    cursor.mStarted = false;
    return true;
}

void SKAirCells::sampleTerrain()
{
    static LLCachedControl<F32> budget(gSavedSettings, "SKAirTerrainBudgetMS", 1.f);
    const F32 pad = tilePad(params());
    scanTiles(mTerrainCursor, llmax(0.1f, static_cast<F32>(budget)) * 0.001f,
        [&](Mesh& mesh, const TileKey&, Tile& tile)
        {
            if (!mesh.mTerrain || !tile.mOverLand || !tile.mTerrainStale) return;
            auto window = std::make_shared<TerrainWindow>();
            fillTerrain(mesh.mOrigin, tile.mMin, tile.mMax, pad, *window);
            tile.mTerrain = window;
            tile.mTerrainStale = false;
        });
}

bool SKAirCells::grounded(const Cluster& cluster) const
{
    static LLCachedControl<F32> margin(gSavedSettings, "SKAirGroundedMargin", 5.f);
    if (!LLWorld::instanceExists())
    {
        return false;
    }
    // The highest ground under the footprint: a structure on a slope stands on the high side of it.
    const F32 land = SKWorldCensus::landUnder(cluster.mMin, cluster.mMax);
    return static_cast<F32>(cluster.mMin.mdV[VZ]) - land <= margin;
}

void SKAirCells::reconcile(const SKCensusSnapshotPtr& snap)
{
    auto workers = workerQueue();
    if (!workers) return;
    const U64 params_key = paramsSignature();
    const TileInput probe = params();
    const F32 pad = tilePad(probe);
    // The census samples the land on the lattice the air's windows are on: whole metres from the padded corner.
    SKWorldCensus::instance().setTerrainPhase(std::ceil(pad) - pad);
    {
        static LLCachedControl<F32> radius(gSavedSettings, "SKAirRadius", 300.f);
        static LLCachedControl<F32> margin(gSavedSettings, "SKAirGroundedMargin", 5.f);
        mCensusParams.mParams = probe;
        mCensusParams.mAirRadius = radius;
        mCensusParams.mGroundedMargin = margin;
    }
    const U64 world_stamp = worldStamp();
    static LLCachedControl<F32> void_margin(gSavedSettings, "SKCensusVoidMargin", 256.f);
    auto ground = std::make_shared<std::vector<ClusterPtr>>();
    std::vector<ClusterPtr> airborne;
    for (const auto& cluster : snap->mClusters.mClusters)
    {
        (grounded(*cluster) ? *ground : airborne).push_back(cluster);
    }
    auto regions = std::make_shared<std::vector<RegionSquare>>();
    for (auto* region : LLWorld::getInstance()->getRegionList())
    {
        regions->push_back({ region->getOriginGlobal(), region->getWidth() });
    }

    const auto submit = [&](Mesh& mesh, ClusterPtr cluster, LLViewerRegion* region)
    {
        mesh.mLastSeen = gFrameTimeSeconds;
        const U64 land = region ? landStamp(*region) : 0;
        if (mesh.mLayoutJob.valid() || mesh.mPendingLayout ||
            (mesh.mLayoutCensus == snap->mStamp && mesh.mLayoutWorld == world_stamp &&
             mesh.mLayoutLand == land && mesh.mLayoutParams == params_key && mesh.mLayoutVoidMargin == void_margin)) return;
        auto layout = std::make_shared<Layout>();
        layout->mCensus = snap->mStamp;
        layout->mWorld = world_stamp;
        layout->mLand = land;
        layout->mParams = params_key;
        layout->mPad = pad;
        layout->mVoidMargin = void_margin;
        layout->mMinZ = region ? region->getLand().getMinZ() : cluster->mMin.mdV[VZ];
        layout->mMaxZ = region ? region->getLand().getMaxZ() : cluster->mMax.mdV[VZ];
        const LLVector3d origin = mesh.mOrigin;
        const LLVector3d region_origin = region ? region->getOriginGlobal() : LLVector3d();
        const F64 width = region ? region->getWidth() : 0.;
        auto task = std::make_shared<std::packaged_task<std::shared_ptr<Layout>()>>(
            [layout, cluster, ground, regions, origin, region_origin, width]()
            {
                if (cluster)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - lay airborne worker");
                    layout->mRefused = !layAirborne(*cluster, origin, layout->mPad, layout->mTiles);
                }
                else
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - lay region worker");
                    std::vector<RecordPtr> records;
                    for (const auto& c : *ground) records.insert(records.end(), c->mRecords.begin(), c->mRecords.end());
                    layout->mRefused = !layRegion(region_origin, width, origin, records, layout->mPad, layout->mVoidMargin, *regions, layout->mTiles);
                }
                for (const auto& tile : layout->mTiles)
                {
                    layout->mKeys.insert(tile.mKey);
                    layout->mSignatures.push_back(recordSignature(tile.mRecords));
                }
                return layout;
            });
        auto future = task->get_future();
        if (workers->tryPost([task]() { (*task)(); }))
        {
            mesh.mLayoutJob = std::move(future);
        }
    };
    std::set<LLUUID> live;
    for (const auto& cluster : airborne)
    {
        live.insert(cluster->mId);
        auto& mesh = mMeshes[cluster->mId];
        if (!mesh)
        {
            mesh = std::make_unique<Mesh>();
            mesh->mId = cluster->mId;
            mesh->mGeneration = mNextGeneration++;
            mesh->mOrigin = latticeOrigin(cluster->mMin);
            if (!cluster->mRecords.empty())
            {
                auto* region = LLWorld::getInstance()->getRegionFromPosGlobal(cluster->mRecords.front()->mOrigin + LLVector3d(1, 1, 0));
                mesh->mRegion = region ? region->getHandle() : 0;
            }
        }
        submit(*mesh, cluster, nullptr);
    }
    for (auto* region : LLWorld::getInstance()->getRegionList())
    {
        live.insert(region->getRegionID());
        auto& mesh = mMeshes[region->getRegionID()];
        if (!mesh)
        {
            mesh = std::make_unique<Mesh>();
            mesh->mId = region->getRegionID();
            mesh->mTerrain = true;
            mesh->mRegion = region->getHandle();
            mesh->mGeneration = mNextGeneration++;
            mesh->mOrigin = latticeOrigin(region->getOriginGlobal());
        }
        submit(*mesh, nullptr, region);
    }
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
    {
        if (live.count(it->first)) { ++it; continue; }
        // Detached meshes have no readers; destroy their large tables on the pool.
        std::shared_ptr<Mesh> retired(std::move(it->second));
        retireOn(workers, std::move(retired));
        if (SKWorldTrace::tracing())
        {
            SKWorldTrace::Line("air.mesh.drop").mesh(it->first).str("why", "census");
        }
        noteAirChange(AirChange::MESH_GONE, it->first);
        it = mMeshes.erase(it);
        bumpRevision("mesh.drop");
    }
}

void SKAirCells::applyLayouts()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - publish layout");
    LLTimer timer;
    auto workers = workerQueue();
    const U64 world_stamp = worldStamp();
    const U64 params_key = paramsSignature();
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        if (mesh.mLayoutJob.valid() && mesh.mLayoutJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            mesh.mPendingLayout = mesh.mLayoutJob.get();
            if (mesh.mPendingLayout && mesh.mPendingLayout->mRefused)
            {
                mStats.mRefused++;
                LL_WARNS("SKAir") << "mesh " << mesh.mId << " wants more than " << MAX_TILES_PER_MESH << " tiles; left unbuilt ("
                                  << mStats.mRefused << " refused)" << LL_ENDL;
            }
        }
        if (!mesh.mPendingLayout) continue;
        auto& layout = *mesh.mPendingLayout;
        // A region or parameter change invalidates the whole result before publication.
        if (layout.mWorld != world_stamp || layout.mParams != params_key)
        {
            auto retired = std::move(mesh.mPendingLayout);
            retireOn(workers, std::move(retired));
            mesh.mDropping = false;
            continue;
        }
        while (layout.mNext < layout.mTiles.size())
        {
            if (timer.getElapsedTimeF32() > 0.002f) return;
            const size_t index = layout.mNext++;
            auto& laid = layout.mTiles[index];
            Tile& tile = mesh.mTiles[laid.mKey];
            std::vector<RecordPtr> before;
            before.swap(tile.mRecords);
            tile.mRecords.swap(laid.mRecords);
            tile.mMin = meshTileMin(mesh.mOrigin, laid.mKey);
            tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.);
            tile.mMin.mdV[VZ] = layout.mMinZ;
            tile.mMax.mdV[VZ] = layout.mMaxZ;
            tile.mOverLand = mesh.mTerrain && laid.mOverLand;
            U64 terrain = 0;
            if (tile.mOverLand)
            {
                terrain = tile.mTerrainSignature;
                if (!tile.mTerrain || tile.mLandStamp != layout.mLand || tile.mWorldStamp != layout.mWorld)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - layout terrain signature");
                    terrain = terrainSignature(tile.mMin, tile.mMax, layout.mPad);
                    tile.mTerrainStale = true;
                }
                tile.mLandStamp = layout.mLand;
                tile.mWorldStamp = layout.mWorld;
            }
            else
            {
                tile.mTerrain.reset();
                tile.mTerrainStale = false;
            }
            reSign(tile, before, layout.mSignatures[index], terrain, layout.mParams);
            tile.mTerrainSignature = terrain;
            laid.mRecords.swap(before); // the old records go with the layout, on a worker
        }
        if (!mesh.mDropping) { mesh.mDropCursor = mesh.mTiles.begin(); mesh.mDropping = true; }
        while (mesh.mDropCursor != mesh.mTiles.end())
        {
            if (timer.getElapsedTimeF32() > 0.002f) return;
            auto current = mesh.mDropCursor++;
            if (layout.mKeys.count(current->first)) continue;
            if (SKWorldTrace::tracing())
            {
                traceTile("air.tile.drop", mesh.mId, current->first, current->second);
            }
            noteAirChange(AirChange::DROPPED, mesh, current->first, current->second);
            auto retired = std::make_shared<Tile>(std::move(current->second));
            retireOn(workers, std::move(retired));
            mesh.mTiles.erase(current);
            bumpRevision("tile.drop");
        }
        mesh.mLayoutCensus = layout.mCensus;
        mesh.mLayoutWorld = layout.mWorld;
        mesh.mLayoutLand = layout.mLand;
        mesh.mLayoutParams = layout.mParams;
        mesh.mLayoutVoidMargin = layout.mVoidMargin;
        mesh.mDropping = false;
        auto retired = std::move(mesh.mPendingLayout);
        retireOn(workers, std::move(retired));
    }
}

void SKAirCells::reSign(Tile& tile, const std::vector<RecordPtr>& before, U64 records, U64 terrain, U64 params_key)
{
    // The old signatures say which input moved: the mixed one alone cannot tell a record from the land from the parameters.
    const bool records_moved = records != tile.mRecordSignature;
    const bool terrain_moved = terrain != tile.mTerrainSignature;
    tile.mRecordSignature = records;
    tile.mSignature = mix(mix(records, terrain), params_key);
    if (tile.mSignature == tile.mBuilt)
    {
        return;
    }
    if (!tile.mBuilt)
    {
        tile.mCause = CAUSE_CREATED;
        tile.mHasCauseAt = false;
    }
    else if (records_moved)
    {
        tile.mCause = CAUSE_RECORDS;
        tile.mHasCauseAt = changedRecord(before, tile.mRecords, tile.mCauseAt);
    }
    else if (terrain_moved)
    {
        tile.mCause = CAUSE_TERRAIN;
        LLVector3d centre = (tile.mMin + tile.mMax) * 0.5;
        if (LLWorld::instanceExists())
        {
            centre.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(centre);
        }
        tile.mCauseAt = centre;
        tile.mHasCauseAt = true;
    }
    else
    {
        tile.mCause = CAUSE_PARAMS;
        tile.mHasCauseAt = false;
    }
}

bool SKAirCells::changedRecord(const std::vector<RecordPtr>& before, const std::vector<RecordPtr>& after, LLVector3d& out)
{
    // The census reuses a part's record while its signature holds, so a new
    // pointer is a part that appeared or changed, a missing one a part that left.
    const std::unordered_set<RecordPtr> old_records(before.begin(), before.end());
    for (const RecordPtr& r : after)
    {
        if (r && !old_records.count(r))
        {
            out = (r->mMin + r->mMax) * 0.5;
            return true;
        }
    }
    const std::unordered_set<RecordPtr> new_records(after.begin(), after.end());
    for (const RecordPtr& r : before)
    {
        if (r && !new_records.count(r))
        {
            out = (r->mMin + r->mMax) * 0.5;
            return true;
        }
    }
    return false;
}

void SKAirCells::bumpRevision(const char* why)
{
    ++mRevision;
    if (SKWorldTrace::tracing())
    {
        SKWorldTrace::Line("air.rev").str("why", why).num("rev", static_cast<S64>(mRevision));
    }
}

void SKAirCells::tileBlip(const Mesh& mesh, const Tile& tile) const
{
    static LLCachedControl<bool> show(gSavedSettings, "SKAirOverlayUpdates", true);
    if (!show || tile.mCause == CAUSE_NONE || !gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WORLD))
    {
        return;
    }
    LLColor4 color;
    switch (tile.mCause)
    {
        case CAUSE_CREATED: color.set(0.25f, 1.f, 0.25f, 1.f); break;
        case CAUSE_RECORDS: color.set(1.f, 0.6f, 0.1f, 1.f); break;
        case CAUSE_TERRAIN: color.set(0.2f, 0.8f, 1.f, 1.f); break;
        case CAUSE_PARAMS: color.set(1.f, 0.3f, 1.f, 1.f); break;
        default: color.set(1.f, 0.2f, 0.2f, 1.f); break;    // a failed build
    }
    // The blip rises from the tile's own floor: the land under a region tile, the middle of its height range otherwise.
    LLVector3d at = (tile.mMin + tile.mMax) * 0.5;
    if (mesh.mTerrain && tile.mOverLand && LLWorld::instanceExists())
    {
        at.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(at);
    }
    else
    {
        at.mdV[VZ] = (tile.mMin.mdV[VZ] + tile.mMax.mdV[VZ]) * 0.5;
    }
    gPipeline.addDebugBlip(gAgent.getPosAgentFromGlobal(at), color);
}

bool SKAirCells::prepareInput(const Mesh& mesh, const TileKey& key, const Tile& tile, std::shared_ptr<TileInput>& input,
                              std::shared_ptr<const std::vector<RecordPtr>>& records) const
{
    input = std::make_shared<TileInput>(params());
    input->mMeshOrigin = mesh.mOrigin;
    input->mTileX = key.mX;
    input->mTileY = key.mY;
    // Detail air near the camera; a tile further out keeps the detail it has, else reads coarse air.
    const bool detail = (tile.mAir && tile.mAirBuilt) || (tile.mAirBuilt && tile.mAirBuilt == tile.mSignature);
    if (!detail && !inAirRadius(tile, airReach()))
    {
        input->mAir = false;
        input->mCoarseAir = true;
    }
    // Only a region mesh has ground in it, and only its own. Until the budget
    // has sampled the land, the tile waits rather than build on ground that is gone.
    if (mesh.mTerrain && tile.mOverLand)
    {
        if (!tile.mTerrain || tile.mTerrainStale)
        {
            return false;
        }
        input->mTerrain = *tile.mTerrain;
    }

    records = std::make_shared<const std::vector<RecordPtr>>(tile.mRecords);
    for (const auto& record : tile.mRecords)
    {
        if (record && record->mNavRole != NavRole::EXCLUSION_VOLUME) input->mRecords.push_back(record);
    }
    // A linkset named for stairs or a ramp says which it is better than its shape can.
    const SKWorldCensus* census = SKWorldCensus::instanceExists() ? SKWorldCensus::getInstance() : nullptr;
    tileRootCues(
        [census](const LLUUID& id) {
            if (const SKWorldCensus::LinksetFacts* facts = census ? census->linksetFactsOf(id) : nullptr; facts && !facts->mName.empty())
            {
                return nameCue(facts->mName, facts->mDescription);
            }
            const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(id);
            return props ? nameCue(props->mName, props->mDescription) : static_cast<U8>(CUE_NONE);
        },
        *input);
    // finishTileInput splits the records again on the worker.
    input->mRecords.clear();
    return true;
}

bool SKAirCells::launch(Mesh& mesh, const TileKey& key, Tile& tile)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t workers = workerQueue();
    if (!main_queue || !workers || workers->isClosed())
    {
        return false;
    }
    std::shared_ptr<TileInput> input;
    std::shared_ptr<const std::vector<RecordPtr>> records;
    if (!prepareInput(mesh, key, tile, input, records))
    {
        return false;
    }

    const LLUUID id = mesh.mId;
    const U32 generation = mesh.mGeneration;
    const U64 signature = tile.mSignature;
    struct Outcome
    {
        std::shared_ptr<TileAirResult> mResult;
        U64 mKey = 0;
    };
    const bool posted = main_queue->postTo(
        workers,
        [input, records]()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - build tile worker");
            auto out = std::make_shared<Outcome>();
            out->mResult = std::make_shared<TileAirResult>();
            // A pool drains its queue before it joins, so work left at logout skips itself.
            if (LLApp::isExiting()) return out;
            finishTileInput(*records, input->mMeshOrigin, *input);
            out->mKey = tileContentKey(*input);
            rasteriseTile(*input, *out->mResult);
            return out;
        },
        [this, id, generation, key, signature](std::shared_ptr<Outcome> out)
        {
            publish(id, generation, key, signature, out->mResult, out->mKey);
        });
    if (posted)
    {
        tile.mInFlight = true;
        mInFlight++;
        if (SKWorldTrace::tracing())
        {
            traceTile("air.launch", id, key, tile);
        }
    }
    return posted;
}

void SKAirCells::publish(const LLUUID& mesh_id, U32 generation, const TileKey& key, U64 signature, const std::shared_ptr<TileAirResult>& result,
                         U64 content_key)
{
    mInFlight = std::max(0, mInFlight - 1);
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end() || it->second->mGeneration != generation)
    {
        return;     // the mesh went away while the job ran
    }
    Mesh& mesh = *it->second;
    const auto tit = mesh.mTiles.find(key);
    if (tit == mesh.mTiles.end())
    {
        return;
    }
    Tile& tile = tit->second;
    tile.mInFlight = false;
    if (tile.mSignature != signature)
    {
        return;     // superseded: the scheduler launches it again
    }
    mStats.mBuilds++;
    if (result->mStatus == TileAirResult::FAILED)
    {
        mStats.mFailures++;
        LL_WARNS("SKAir") << "tile build failed: " << result->mError << LL_ENDL;
        // The products of the build before describe geometry that has changed; drop them rather than answer with them.
        tile.mFaces.reset();
        tile.mLedges.reset();
        tile.mRunoff.reset();
        tile.mAir.reset();
        tile.mAirId = 0;
        tile.mAirCoarse.reset();
        tile.mContentKey = 0;
        tile.mAirBuilt = signature;
        tile.mBuilt = signature;    // do not retry the same input
        tile.mCause = CAUSE_FAILED;
        tile.mHasCauseAt = false;
        tileBlip(mesh, tile);
        if (SKWorldTrace::tracing())
        {
            traceTile("air.fail", mesh_id, key, tile);
        }
        noteAirChange(AirChange::DROPPED, mesh, key, tile);
        bumpRevision("tile.fail");
        return;
    }
    if (result->mStatus == TileAirResult::SUCCESS)
    {
        mStats.mLastBuildMs = result->mMillis;
        mStats.mWorstBuildMs = std::max(mStats.mWorstBuildMs, result->mMillis);
        mLastRasterMs = result->mMillis;
        if (result->mExcludeMillis > 0.f)
        {
            mStats.mExcludeMs = result->mExcludeMillis;
        }
    }
    // Built again to the air it has: the published air stands, so nothing that reads it runs again.
    const bool same_air = tile.mAir && result->mAir && tile.mAir != result->mAir && sameAir(*tile.mAir, *result->mAir);
    const bool air_moved = !result->mAirBuilt || !same_air;
    tile.mFaces = std::make_shared<const std::vector<WallFace>>(std::move(result->mFaces));
    tile.mLedges = std::make_shared<const std::vector<WallSegment>>(std::move(result->mLedges));
    tile.mRunoff = std::make_shared<const std::vector<RunoffSegment>>(std::move(result->mRunoff));
    if (air_moved)
    {
        tile.mAir = result->mAirBuilt ? result->mAir : nullptr;
        tile.mAirId = tile.mAir ? ++mNextAirId : 0;
    }
    tile.mAirBuilt = result->mAirBuilt && !result->mCoarseAir ? signature : 0;
    // A tile rebuilt without its air keeps the tops it had only while its input holds.
    if (tile.mAir || tile.mBuilt != signature)
    {
        tile.mAirCoarse.reset();
    }
    tile.mContentKey = content_key;
    tile.mBuilt = signature;
    tile.mMillis = result->mMillis;
    tileBlip(mesh, tile);
    if (SKWorldTrace::tracing())
    {
        SKWorldTrace::Line("air.built").mesh(mesh_id).tile(key.mX, key.mY).box(tile.mMin, tile.mMax).str("cause", causeName(tile.mCause))
            .flag("coarse", result->mCoarseAir).flag("air", tile.mAir != nullptr);
    }
    if (air_moved)
    {
        noteAirChange(AirChange::PUBLISHED, mesh, key, tile);
    }
    bumpRevision("air.built");
}

void SKAirCells::schedule()
{
    static LLCachedControl<S32> per_frame(gSavedSettings, "SKAirBuildsPerFrame", 4);
    static LLCachedControl<S32> in_flight_max(gSavedSettings, "SKAirBuildsInFlight", 0);

    auto workers = workerQueue();
    if (!workers)
    {
        return;
    }
    // In flight is the pool's width unless the setting overrides it: more cannot run at once.
    const S32 in_flight_cap = in_flight_max > 0 ? static_cast<S32>(in_flight_max) : static_cast<S32>(mWorkers->getWidth());
    if (mScheduleJob.valid())
    {
        if (mScheduleJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        mBuildCandidates = mScheduleJob.get();
        mNextCandidate = 0;
        mCandidateTime = gFrameTimeSeconds;
    }
    if (mBuildCandidates && gFrameTimeSeconds - mCandidateTime > 1.f)
    {
        auto retired = std::move(mBuildCandidates);
        retireOn(workers, std::move(retired));
    }
    const AirReach reach = airReach();
    const auto due = [&](const Tile& tile)
    {
        const bool air_due = tile.mSignature == tile.mBuilt && tile.mAirBuilt != tile.mSignature && inAirRadius(tile, reach);
        return !tile.mInFlight && (tile.mSignature != tile.mBuilt || air_due);
    };
    if (mBuildCandidates)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - launch builds");
        LLTimer timer;
        S32 launched = 0;
        while (mNextCandidate < mBuildCandidates->size())
        {
            if (launched >= per_frame || mInFlight >= in_flight_cap || timer.getElapsedTimeF32() >= 0.001f) return;
            const auto& candidate = (*mBuildCandidates)[mNextCandidate++];
            const auto mesh = mMeshes.find(candidate.mMesh);
            if (mesh == mMeshes.end() || mesh->second->mGeneration != candidate.mGeneration) continue;
            const auto tile = mesh->second->mTiles.find(candidate.mKey);
            if (tile != mesh->second->mTiles.end() && due(tile->second) && launch(*mesh->second, candidate.mKey, tile->second)) ++launched;
        }
        auto retired = std::move(mBuildCandidates);
        retireOn(workers, std::move(retired));
    }
    if (mInFlight >= in_flight_cap) return;
    if (!mScheduleScan) mScheduleScan = std::make_shared<std::vector<BuildCandidate>>();
    const LLVector3d agent = gAgent.getPositionGlobal();
    const LLVector3d camera = cameraGlobal();
    if (!mScheduleScanReady && !scanTiles(mScheduleCursor, 0.001f, [&](Mesh& mesh, const TileKey& key, Tile& tile)
        {
            if (!due(tile)) return;
            const LLVector3d centre = (tile.mMin + tile.mMax) * 0.5;
            const F64 distance = llmin(dist_vec_squared2D(centre, agent), dist_vec_squared2D(centre, camera));
            mScheduleScan->push_back({mesh.mId, mesh.mGeneration, key, distance, meshPriority(mesh)});
        })) return;
    mScheduleScanReady = true;
    auto candidates = mScheduleScan;
    mStats.mQueued = static_cast<U32>(candidates->size());
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<std::vector<BuildCandidate>>()>>([candidates]()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - schedule worker");
        std::sort(candidates->begin(), candidates->end(), [](const auto& a, const auto& b)
        {
            return a.mPriority != b.mPriority ? a.mPriority < b.mPriority : a.mDistance < b.mDistance;
        });
        return candidates;
    });
    auto future = task->get_future();
    if (workers->tryPost([task]() { (*task)(); }))
    {
        mScheduleJob = std::move(future);
        mScheduleScan.reset();
        mScheduleScanReady = false;
    }
}

void SKAirCells::update()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - update");
    SKWorldCensus& census = SKWorldCensus::instance();
    if (!mSectionRegistered)
    {
        census.addCensusSection(airParamsSection(mCensusParams));
        mSectionRegistered = true;
    }
    const bool want = wanted();
    if (want != mDemanding)
    {
        static LLCachedControl<F32> range(gSavedSettings, "SKCensusRange", 512.f);
        if (want)
        {
            census.addDemand("air", range);
        }
        else
        {
            census.removeDemand("air");
        }
        mDemanding = want;
    }
    if (!want || !LLWorld::instanceExists() || !gAgent.getRegion())
    {
        return;
    }

    SKCensusSnapshotPtr snap = census.snapshot();
    if (!snap)
    {
        return;
    }
    // On a new census, and once a second besides: the land changes without the census noticing.
    if (snap->mStamp != mCensusStamp || gFrameTimeSeconds - mLastReconcile > 1.f)
    {
        mCensusStamp = snap->mStamp;
        mLastReconcile = gFrameTimeSeconds;
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - reconcile");
        reconcile(snap);
    }
    findHomeBoxes(snap);
    applyLayouts();

    // Meshes nothing has claimed for a while are gone.
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - retire meshes");
        for (auto it = mMeshes.begin(); it != mMeshes.end();)
        {
            if (gFrameTimeSeconds - it->second->mLastSeen > 30.f)
            {
                std::shared_ptr<Mesh> retired(std::move(it->second));
                retireOn(workerQueue(), std::move(retired));
                if (SKWorldTrace::tracing())
                {
                    SKWorldTrace::Line("air.mesh.drop").mesh(it->first).str("why", "unseen");
                }
                noteAirChange(AirChange::MESH_GONE, it->first);
                it = mMeshes.erase(it);
                bumpRevision("mesh.drop");
            }
            else
            {
                ++it;
            }
        }
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - sample terrain");
        sampleTerrain();
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - evict air");
        evictAir();
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - schedule");
        schedule();
    }
    if (mSkyJob.valid() || mSkyRevision != mRevision)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - sync sky map");
        syncSkyMap();
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - stats scan");
        mStats.mMeshes = static_cast<U32>(mMeshes.size());
        mStats.mInFlight = mInFlight;
        // Keep the last complete counts while a bounded scan runs; partial totals would look like lost tiles.
        if (scanTiles(mStatsCursor, 0.0005f, [&](Mesh&, const TileKey&, Tile& tile)
            {
                ++mScanningStats.mTiles;
                if (tile.mAir) { ++mScanningStats.mAirTiles; mScanningStats.mAirBytes += tile.mAir->bytes(); }
                if (!tile.mBuilt) return;
                ++mScanningStats.mTilesBuilt;
                mScanningStats.mFaces += tile.mFaces ? static_cast<U32>(tile.mFaces->size()) : 0;
            }))
        {
            mStats.mTiles = mScanningStats.mTiles;
            mStats.mTilesBuilt = mScanningStats.mTilesBuilt;
            mStats.mAirTiles = mScanningStats.mAirTiles;
            mStats.mAirBytes = mScanningStats.mAirBytes;
            mStats.mFaces = mScanningStats.mFaces;
            mScanningStats = {};
        }
    }

    static LLCachedControl<bool> log(gSavedSettings, "SKAirLog", false);
    if (log && gFrameTimeSeconds - mLastLog > 5.f)
    {
        mLastLog = gFrameTimeSeconds;
        LL_INFOS("SKAir") << statsLine() << LL_ENDL;
    }
}

void SKAirCells::snapshotMeshes(std::vector<MeshInfo>& out) const
{
    out.clear();
    out.reserve(mMeshes.size());
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        out.push_back({ mesh.mId, mesh.mTerrain, mesh.mRegion, mesh.mOrigin, mesh.mGeneration, meshPriority(mesh) });
    }
}

void SKAirCells::tileContentKeysIn(const LLUUID& mesh_id, const LLVector3d& min, const LLVector3d& max, std::vector<U64>& out) const
{
    out.clear();
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end())
    {
        return;
    }
    // Inset, so a box flush with a tile edge does not pick up its neighbour.
    const LLVector3d inset(0.01, 0.01, 0.0);
    for (const auto& t : it->second->mTiles)
    {
        if (overlapsXY(t.second.mMin, t.second.mMax, min + inset, max - inset))
        {
            out.push_back(t.second.mBuilt ? t.second.mContentKey : 0);
        }
    }
}

bool SKAirCells::settlingIn(const LLVector3d& min, const LLVector3d& max) const
{
    for (const auto& m : mMeshes)
    {
        for (const auto& t : m.second->mTiles)
        {
            if ((t.second.mInFlight || t.second.mSignature != t.second.mBuilt) && overlaps3D(t.second.mMin, t.second.mMax, min, max))
            {
                return true;
            }
        }
    }
    return false;
}

std::string SKAirCells::statsLine() const
{
    return llformat("air: %u meshes, %u/%u tiles built (%u queued, %u in flight), %u builds, %u failures, %u refused, raster %.1f ms (worst %.1f, "
                    "exclusion %.2f), %u faces, air %u tiles %.1f MB, live %.1f MB, tops %.1f MB, %u evicted",
                    mStats.mMeshes, mStats.mTilesBuilt, mStats.mTiles, mStats.mQueued, mStats.mInFlight, mStats.mBuilds, mStats.mFailures,
                    mStats.mRefused, mLastRasterMs, mStats.mWorstBuildMs, mStats.mExcludeMs, mStats.mFaces, mStats.mAirTiles,
                    mStats.mAirBytes / 1048576.0, gAirLiveBytes.load() / 1048576.0, gAirTopsBytes.load() / 1048576.0, mStats.mAirEvicted)
        + (mStats.mAirOverlayTiles ? llformat(", overlay %u verts over %u tiles, %u cut", mStats.mAirOverlayVerts, mStats.mAirOverlayTiles,
                                              mStats.mAirOverlayCut)
                                   : std::string());
}

void SKAirCells::noteAirChange(AirChange::Kind kind, const LLUUID& mesh)
{
    // Unread for this long, the list says no more than that anything may have moved.
    constexpr size_t MOST_AIR_CHANGES = 65536;
    if (mAirChanges.size() >= MOST_AIR_CHANGES)
    {
        mAirChanges.clear();
        mAirChangesLost = true;
    }
    AirChange& change = mAirChanges.emplace_back();
    change.mMesh = mesh;
    change.mKind = kind;
}

void SKAirCells::noteAirChange(AirChange::Kind kind, const Mesh& mesh, const TileKey& key, const Tile& tile)
{
    noteAirChange(kind, mesh.mId);
    AirChange& change = mAirChanges.back();
    change.mKey = key;
    change.mAirId = tile.mAirId;
    change.mMin = tile.mMin;
    change.mMax = tile.mMax;
    change.mWholeMesh = false;
}

bool SKAirCells::drainChanges(std::vector<AirChange>& out) const
{
    out.clear();
    out.swap(mAirChanges);
    const bool whole = !mAirChangesLost;
    mAirChangesLost = false;
    return whole;
}

SKAirCells::AirSnapshot SKAirCells::airSnapshot(const LLUUID& mesh_id) const
{
    AirSnapshot out;
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end())
    {
        return out;
    }
    const Mesh& mesh = *it->second;
    out.mFound = true;
    out.mOrigin = mesh.mOrigin;
    out.mPriority = meshPriority(mesh);
    out.mTiles.reserve(mesh.mTiles.size());
    for (const auto& t : mesh.mTiles)
    {
        out.mTiles.push_back({ t.first, t.second.mAir, t.second.mAirCoarse, t.second.mAirId, skyContributor(mesh.mId, t.first), t.second.mMin,
                               t.second.mMax });
    }
    return out;
}

void SKAirCells::syncSkyMap()
{
    auto workers = workerQueue();
    if (mSkyJob.valid())
    {
        if (mSkyJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = mSkyJob.get();
        std::swap(mSkyMap, result->mMap);
        mSkyTiles.swap(result->mTiles);
        mSkyRevision = result->mRevision;
        retireOn(workers, std::move(result));
    }
    if (mSkyRevision == mRevision) return;
    if (!mSkyScan)
    {
        mSkyScan = std::make_shared<std::map<U64, SkyContributor>>();
        mSkyScanRevision = mRevision;
    }
    if (!mSkyScanReady && !scanTiles(mSkyCursor, 0.001f, [&](Mesh& mesh, const TileKey& key, Tile& tile)
        {
            const auto& air = tile.mAir ? tile.mAir : tile.mAirCoarse;
            if (air) (*mSkyScan)[skyContributor(mesh.mId, key)] = {mesh.mOrigin, air->mTops};
        })) return;
    mSkyScanReady = true;
    auto seen = mSkyScan;
    auto result = std::make_shared<SkyResult>();
    // The worker's copy is the sky map's only writer; the map shares its immutable blocks until the worker writes them.
    result->mMap = mSkyMap;
    result->mRevision = mSkyScanRevision;
    auto previous = std::make_shared<std::map<U64, TileTopsPtr>>(mSkyTiles);
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<SkyResult>()>>([result, seen, previous]()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("skair - sync sky map worker");
        for (const auto& entry : *seen)
        {
            const auto old = previous->find(entry.first);
            const auto& tops = entry.second.mTops;
            // Air built again to the same tops keeps the cells.
            if (old == previous->end() || (old->second != tops && !sameTops(*old->second, *tops)))
                result->mMap.set(entry.first, entry.second.mOrigin + LLVector3d(tops->mOrigin), *tops);
            result->mTiles.emplace(entry.first, tops);
        }
        for (const auto& old : *previous)
            if (!seen->count(old.first)) result->mMap.remove(old.first);
        return result;
    });
    auto future = task->get_future();
    if (workers->tryPost([task]() { (*task)(); }))
    {
        mSkyJob = std::move(future);
        mSkyScan.reset();
        mSkyScanReady = false;
    }
}

void SKAirCells::forEachTileIn(const LLVector3d& min, const LLVector3d& max, const std::function<void(const Mesh&, const Tile&)>& fn) const
{
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        visitTilesIn(mesh.mTiles, mesh.mOrigin, min, max, [&](const TileKey&, const Tile& tile) {
            if (tile.mBuilt && overlapsXY(tile.mMin, tile.mMax, min, max))
            {
                fn(mesh, tile);
            }
        });
    }
}

void SKAirCells::wallFacesIn(const LLVector3d& min, const LLVector3d& max, const WallFaceFn& fn) const
{
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        if (!tile.mFaces)
        {
            return;
        }
        for (const WallFace& f : *tile.mFaces)
        {
            const LLVector3d fmin = mesh.mOrigin + LLVector3d(f.mMin), fmax = mesh.mOrigin + LLVector3d(f.mMax);
            if (overlapsXY(fmin, fmax, min, max))
            {
                fn(fmin, fmax, f.mSide, f.mArea);
            }
        }
    });
}

void SKAirCells::runoffIn(const LLVector3d& min, const LLVector3d& max, const LedgeFn& ledge_fn, const RunoffFn& runoff_fn) const
{
    const auto planBox = [](const LLVector3d& a, const LLVector3d& b, LLVector3d& lo, LLVector3d& hi) {
        lo.set(llmin(a.mdV[VX], b.mdV[VX]), llmin(a.mdV[VY], b.mdV[VY]), 0.0);
        hi.set(llmax(a.mdV[VX], b.mdV[VX]), llmax(a.mdV[VY], b.mdV[VY]), 0.0);
    };
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        LLVector3d lo, hi;
        if (tile.mLedges && ledge_fn)
        {
            for (const WallSegment& seg : *tile.mLedges)
            {
                const LLVector3d a = mesh.mOrigin + LLVector3d(seg.mA), b = mesh.mOrigin + LLVector3d(seg.mB);
                planBox(a, b, lo, hi);
                if (overlapsXY(lo, hi, min, max))
                {
                    ledge_fn(a, b, seg.mKind, seg.mValue);
                }
            }
        }
        if (tile.mRunoff && runoff_fn)
        {
            for (const RunoffSegment& seg : *tile.mRunoff)
            {
                const LLVector3d a = mesh.mOrigin + LLVector3d(seg.mA), b = mesh.mOrigin + LLVector3d(seg.mB);
                planBox(a, b, lo, hi);
                if (overlapsXY(lo, hi, min, max))
                {
                    runoff_fn(a, b, seg.mDropA, seg.mDropB, seg.mSide);
                }
            }
        }
    });
}
