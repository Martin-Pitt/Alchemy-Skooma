/**
 * @file sknavmesh.cpp
 * @brief The navmesh: one Detour mesh per cluster and per region's terrain, built tile by tile on a worker.
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

#include "sknavmesh.h"

#include "skcaptureexport.h"
#include "sknavannotate.h"
#include "sknavannotator.h"
#include "sknavcensusio.h"
#include "sknavexport.h"
#include "sknavlayout.h"
#include "sknavoverlay.h"
#include "sknavpalette.h"
#include "sknavroomprobes.h"
#include "sknavrooms.h"
#include "sknavspacefilter.h"

#include "alobjectproperties.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbase64.h"
#include "llcallingcard.h"
#include "llappviewer.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llgltfmaterial.h"
#include "llglslshader.h"
#include "llmaterial.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewerjointattachment.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llwind.h"
#include "llviewershadermgr.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "skphysicsshape.h"
#include "workqueue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <set>
#include <thread>
#include <unordered_set>

namespace
{
    using namespace sknav;
    using skcapture::faceAlphaMode;
    using skcapture::facePbrAlpha;
    using skcapture::linkNumber;

    template<typename T>
    void retireOn(const LL::WorkQueue::ptr_t& workers, T value)
    {
        auto held = std::make_shared<T>(std::move(value));
        workers->post([held]() { *held = {}; });
    }


    bool overlaps3D(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax)
    {
        return overlapsXY(amin, amax, bmin, bmax) && !(amin.mdV[VZ] > bmax.mdV[VZ] || bmin.mdV[VZ] > amax.mdV[VZ]);
    }

    void writeVec3d(std::ostream& file, const LLVector3d& v)
    {
        file << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']';
    }

    void writeVec3(std::ostream& file, const LLVector3& v)
    {
        file << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']';
    }

    // Two span corners the same within a few centimetres.
    bool sameSpan(const LLVector3d& a, const LLVector3d& b)
    {
        return std::fabs(a.mdV[VX] - b.mdV[VX]) <= 0.05 && std::fabs(a.mdV[VY] - b.mdV[VY]) <= 0.05
            && std::fabs(a.mdV[VZ] - b.mdV[VZ]) <= 0.05;
    }

    constexpr F32 EXPORT_REACH_M = 16.f; ///< the object export's surroundings: half the side of the cube about the point

    /// Whether a triangle's box meets the cube of half side reach about the origin.
    bool withinReach(const LLVector3* tri, F32 reach)
    {
        for (S32 axis = 0; axis < 3; ++axis)
        {
            const F32 lo = llmin(tri[0].mV[axis], llmin(tri[1].mV[axis], tri[2].mV[axis]));
            const F32 hi = llmax(tri[0].mV[axis], llmax(tri[1].mV[axis], tri[2].mV[axis]));
            if (hi < -reach || lo > reach)
            {
                return false;
            }
        }
        return true;
    }
}

SKNavMesh::Mesh::~Mesh()
{
    for (S32 c = 0; c < NUM_CLASSES; ++c)
    {
        dtFreeNavMeshQuery(mQuery[c]);
        dtFreeNavMesh(mNav[c]);
    }
}

SKNavMesh::SKNavMesh()
    : mRooms(std::make_unique<SKNavRooms>()),
      mRoomProbes(std::make_unique<SKNavRoomProbes>())
{
}

SKNavMesh::~SKNavMesh()
{
}

bool SKNavMesh::overlayActive()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavMeshEnabled", true);
    if (!enabled)
    {
        return false;
    }
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH))
    {
        return true;
    }
    // A layer of its own is enough. The walls, the wall faces and the census
    // boxes all read the built mesh, and none of them needs the surface drawn
    // in the way of what they are for.
    return gSavedSettings.getBOOL("SKNavOverlayWalls") || gSavedSettings.getBOOL("SKNavOverlayFaces") || gSavedSettings.getBOOL("SKNavOverlayRunoff")
        || gSavedSettings.getBOOL("SKNavOverlayCensus") || gSavedSettings.getBOOL("SKNavOverlayFeatures")
        || gSavedSettings.getU32("SKNavOverlayEnclosure") != 0 || gSavedSettings.getBOOL("SKNavOverlaySpaces")
        || gSavedSettings.getBOOL("SKNavOverlayWind");
}

bool SKNavMesh::wanted() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavMeshEnabled", true);
    static LLCachedControl<bool> always(gSavedSettings, "SKNavMeshAlways", false);
    return enabled && (always || overlayActive() || probesDemanded() || !mConsumers.empty());
}

void SKNavMesh::addDemand(const std::string& who)
{
    mConsumers.insert(who);
}

void SKNavMesh::removeDemand(const std::string& who)
{
    mConsumers.erase(who);
}

bool SKNavMesh::probesDemanded()
{
    // Mirrors the guards in SKNavRoomProbes::update, which cannot demand
    // anything itself: it only runs when the navmesh already does.
    static LLCachedControl<bool> room_probes(gSavedSettings, "SKNavRoomProbes", true);
    static LLCachedControl<S32> probe_level(gSavedSettings, "RenderReflectionProbeLevel", 3);
    return room_probes && probe_level >= 3 && LLPipeline::sReflectionProbesEnabled;
}

bool SKNavMesh::inAirRadius(const Tile& tile)
{
    static LLCachedControl<F32> radius(gSavedSettings, "SKNavAirRadius", 128.f);
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 dx = llmax(tile.mMin.mdV[VX] - camera.mdV[VX], 0.0, camera.mdV[VX] - tile.mMax.mdV[VX]);
    const F64 dy = llmax(tile.mMin.mdV[VY] - camera.mdV[VY], 0.0, camera.mdV[VY] - tile.mMax.mdV[VY]);
    return dx * dx + dy * dy <= static_cast<F64>(radius) * radius;
}

LL::WorkQueue::ptr_t SKNavMesh::workerQueue()
{
    if (!mWorkers)
    {
        // A pool of its own. The viewer's shared "General" pool is one thread
        // in a normal session, so tile builds queued behind it both starve
        // everything else on that queue and leave every other core idle. A
        // tile build is pure and independent, so one per core is the shape
        // that fits, leaving a couple for the viewer itself.
        static LLCachedControl<S32> setting(gSavedSettings, "SKNavWorkerThreads", 0);
        const S32 cores = static_cast<S32>(std::thread::hardware_concurrency());
        const S32 width = llclamp(setting > 0 ? static_cast<S32>(setting) : cores - 2, 1, 16);
        mWorkers = std::make_unique<LL::ThreadPool>("NavMesh", width);
        mWorkers->start();
        LL_INFOS("SKNav") << "navmesh build pool: " << mWorkers->getWidth() << " threads on " << cores << " cores" << LL_ENDL;
    }
    return LL::WorkQueue::getInstance("NavMesh");
}

std::shared_ptr<TileDiskCache> SKNavMesh::diskCache()
{
    static LLCachedControl<U32> cap_mb(gSavedSettings, "SKNavDiskCacheMB", 2048);
    if (!cap_mb)
    {
        return nullptr;
    }
    const U64 cap = static_cast<U64>(cap_mb) * 1048576;
    if (!mDiskCache)
    {
        mDiskCache = std::make_shared<TileDiskCache>(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "sknav"), cap);
        // Indexing a full folder is slow; until it is done a lookup misses.
        if (LL::WorkQueue::ptr_t workers = workerQueue())
        {
            std::shared_ptr<TileDiskCache> cache = mDiskCache;
            workers->post([cache]() { cache->index(); });
        }
    }
    else
    {
        mDiskCache->setCap(cap);
    }
    return mDiskCache;
}

void SKNavMesh::evictAir()
{
    static LLCachedControl<U32> budget_mb(gSavedSettings, "SKNavAirBudgetMB", 512);
    static LLCachedControl<F32> radius(gSavedSettings, "SKNavAirRadius", 128.f);
    auto workers = workerQueue();
    if (mEvictionJob.valid())
    {
        if (mEvictionJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        mPendingEvictions = mEvictionJob.get();
        mNextEviction = 0;
    }
    if (mPendingEvictions)
    {
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
            if (tile.mInFlight || tile.mAir != candidate.mAir || inAirRadius(tile)) continue;
            const auto cache = diskCache();
            if (cache && tile.mContentKey && !cache->contains(tile.mContentKey))
            {
                // Pack the shared products on the worker.
                struct Spill
                {
                    TileBuildResult mProduct;
                    decltype(Tile::mFaces) mFaces;
                    decltype(Tile::mLedges) mLedges;
                    decltype(Tile::mRunoff) mRunoff;
                };
                auto spill = std::make_shared<Spill>();
                spill->mProduct.mStatus = TileBuildResult::SUCCESS;
                spill->mProduct.mMillis = tile.mMillis;
                spill->mProduct.mAir = tile.mAir;
                spill->mProduct.mAirBuilt = true;
                spill->mFaces = tile.mFaces;
                spill->mLedges = tile.mLedges;
                spill->mRunoff = tile.mRunoff;
                // The raster stage's products only: the stamped polys are on disk under the stamp's own key.
                spill->mProduct.mCoarseAir = tile.mAirBuilt == 0;
                const U64 content_key = tile.mContentKey;
                workers->post([spill, cache, content_key]()
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - spill air worker");
                    if (spill->mFaces) spill->mProduct.mFaces = *spill->mFaces;
                    if (spill->mLedges) spill->mProduct.mLedges = *spill->mLedges;
                    if (spill->mRunoff) spill->mProduct.mRunoff = *spill->mRunoff;
                    cache->store(content_key, packTileProduct(spill->mProduct, content_key));
                });
            }
            tile.mAirCoarse = candidate.mCoarse;
            tile.mAir.reset();
            tile.mAirBuilt = 0;
            ++mStats.mAirEvicted;
            ++mRevision;
        }
        auto retired = std::move(mPendingEvictions);
        retireOn(workers, std::move(retired));
    }
    if (!mEvictionScan) mEvictionScan = std::make_shared<std::vector<AirCandidate>>();
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 dt = llclamp(static_cast<F64>(gFrameIntervalSeconds), 0.0, 1.0);
    if (!mEvictionScanReady && !scanTiles(mEvictionCursor, 0.001f, [&](Mesh& mesh, const TileKey& key, Tile& tile)
        {
            if ((!tile.mAir && !tile.mRaster) || tile.mInFlight) return;
            const F64 dx = llmax(tile.mMin.mdV[VX] - camera.mdV[VX], 0.0, camera.mdV[VX] - tile.mMax.mdV[VX]);
            const F64 dy = llmax(tile.mMin.mdV[VY] - camera.mdV[VY], 0.0, camera.mdV[VY] - tile.mMax.mdV[VY]);
            const F64 distance = std::sqrt(dx * dx + dy * dy);
            if (tile.mRaster)
            {
                mRasterScan.push_back({mesh.mId, mesh.mGeneration, key, tile.mRaster, tile.mRaster->bytes(), distance});
            }
            if (!tile.mAir) return;
            tile.mNearness = accumulateNearness(tile.mNearness, distance, radius, dt);
            mEvictionScan->push_back({mesh.mId, mesh.mGeneration, key, tile.mAir, nullptr, tile.mNearness, distance});
        })) return;
    // One ledger: the held rasters first, then the air in what they leave of the budget.
    const size_t whole = static_cast<size_t>(budget_mb) * 1048576;
    if (!mEvictionScanReady)
    {
        evictRasters(whole, radius);
    }
    mEvictionScanReady = true;
    auto held = mEvictionScan;
    const size_t budget = whole - std::min(mHeldRasterBytes, whole);
    const F64 protect = 1.5 * radius;
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<std::vector<AirCandidate>>()>>([held, budget, protect]()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - evict air worker");
        std::vector<EvictionCandidate> candidates;
        for (const auto& tile : *held) candidates.push_back({tile.mAir->bytes(), tile.mNearness, tile.mDistance});
        auto result = std::make_shared<std::vector<AirCandidate>>();
        for (size_t index : chooseEvictions(candidates, budget, protect))
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

void SKNavMesh::evictRasters(size_t budget, F64 radius)
{
    std::vector<RasterCandidate> scan;
    scan.swap(mRasterScan);
    size_t total = 0;
    for (const RasterCandidate& c : scan) total += c.mBytes;
    const size_t cap = budget / 4;
    const bool over = total > cap;
    std::sort(scan.begin(), scan.end(), [](const RasterCandidate& a, const RasterCandidate& b) { return a.mDistance > b.mDistance; });
    std::vector<TileRasterPtr> dropped;
    for (const RasterCandidate& c : scan)
    {
        if (c.mDistance <= 1.5 * radius && (!over || total <= cap / 8 * 7)) break;
        const auto mesh = mMeshes.find(c.mMesh);
        if (mesh == mMeshes.end() || mesh->second->mGeneration != c.mGeneration) continue;
        const auto found = mesh->second->mTiles.find(c.mKey);
        if (found == mesh->second->mTiles.end()) continue;
        Tile& tile = found->second;
        // Never the raster of a tile with a build in flight, nor one its stamp is still to read.
        if (tile.mInFlight || tile.mStampDue || tile.mStampInFlight || tile.mRaster != c.mRaster) continue;
        dropped.push_back(std::move(tile.mRaster));
        tile.mRaster.reset();
        tile.mRasterDropped = true;
        total -= c.mBytes;
        ++mStats.mRasterEvicted;
    }
    mHeldRasterBytes = total;
    if (!dropped.empty())
    {
        auto retired = std::make_shared<std::vector<TileRasterPtr>>(std::move(dropped));
        retireOn(workerQueue(), std::move(retired));
    }
}

void SKNavMesh::cleanupSingleton()
{
    // Join the pool before the singleton dies: a job in flight carries this
    // pointer in its publish callback, and the main loop stops pumping first.
    if (mWorkers)
    {
        mWorkers->close();
        mWorkers.reset();
    }
    mRooms->close();
    mRoomProbes->clear();
}

bool SKNavMesh::ensureNav(Mesh& mesh, S32 cls)
{
    if (mesh.mNav[cls])
    {
        return true;
    }
    dtNavMesh* nav = allocMeshNav();
    if (!nav)
    {
        mStats.mFailures++;
        LL_WARNS_ONCE("SKNav") << "dtNavMesh::init refused " << MAX_TILES_PER_MESH << " tiles x " << MAX_POLYS_PER_TILE << " polys" << LL_ENDL;
        return false;
    }
    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (!dtStatusSucceed(query->init(nav, 4096)))
    {
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(nav);
        return false;
    }
    mesh.mNav[cls] = nav;
    mesh.mQuery[cls] = query;
    return true;
}

U64 SKNavMesh::paramsSignature() const
{
    TileBuildInput probe;
    fillParams(probe);
    U64 h = SEED;
    h = mixF32(h, probe.mCell);
    h = mix(h, static_cast<U64>(probe.mPartition));
    h = mixF32(h, probe.mDetailSampleCells);
    h = mixF32(h, probe.mWallMinHeight);
    h = mix(h, probe.mWallFaces);
    h = mix(h, probe.mRunoff);
    h = mixF32(h, probe.mRunoffMax);
    h = mixF32(h, probe.mTopParams.mHeight);
    h = mixF32(h, probe.mTopParams.mSlope);
    h = mix(h, static_cast<U64>(probe.mAirPad));
    h = mix(h, probe.mAir);
    h = mixF32(h, probe.mAirMinGap);
    h = mixF32(h, probe.mDropSpacing);
    for (S32 c = 0; c < NUM_CLASSES; ++c)
    {
        h = mix(h, probe.mEdges[c]);
        h = mixF32(h, probe.mDropMax[c]);
        h = mixF32(h, probe.mParams[c].mHeight);
        h = mixF32(h, probe.mParams[c].mRadius);
        h = mixF32(h, probe.mParams[c].mClimb);
        h = mixF32(h, probe.mParams[c].mSlope);
    }
    return h;
}

void SKNavMesh::fillParams(TileBuildInput& input) const
{
    static LLCachedControl<F32> cell(gSavedSettings, "SKNavCell", 0.125f);
    static LLCachedControl<F32> radius(gSavedSettings, "SKNavAvatarRadius", 0.3f);
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    static LLCachedControl<F32> slope(gSavedSettings, "SKNavSlope", 74.f);
    static LLCachedControl<bool> raster_layers(gSavedSettings, "SKNavBuildRaster", false);
    static LLCachedControl<bool> walls(gSavedSettings, "SKNavBuildWalls", true);
    static LLCachedControl<F32> dropAvatar(gSavedSettings, "SKNavDropMax", 10.f);
    static LLCachedControl<F32> dropWeather(gSavedSettings, "SKNavWeatherDropMax", 24.f);
    static LLCachedControl<F32> dropSpacing(gSavedSettings, "SKNavDropSpacing", 2.f);

    static LLCachedControl<U32> partition(gSavedSettings, "SKNavPartition", 3);
    static LLCachedControl<F32> detail(gSavedSettings, "SKNavDetailSample", 6.f);
    static LLCachedControl<F32> wallMin(gSavedSettings, "SKNavWallMinHeight", 0.5f);

    input.mCell = llclamp(static_cast<F32>(cell), 0.05f, 1.f);
    input.mPartition = static_cast<Partition>(llclamp(static_cast<S32>(partition), 0, 3));
    input.mDetailSampleCells = llclamp(static_cast<F32>(detail), 0.f, 32.f);
    input.mWallMinHeight = llclamp(static_cast<F32>(wallMin), 0.f, 8.f);
    // The avatar class is as tall as the user's own avatar, rounded up to the
    // next cell, so a tall avatar never gets floor it cannot stand on.
    F32 height = 2.f;
    if (isAgentAvatarValid())
    {
        height = llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f);
    }
    height = std::ceil(height / input.mCell) * input.mCell;

    input.mParams[static_cast<S32>(AgentClass::AVATAR)] = { height, radius, climb,
                                                            llclamp(static_cast<F32>(slope), 1.f, 89.9f) };
    // The air is read off the solid raster near the camera; its pad is metres, whatever the cell.
    input.mAir = true;
    input.mAirPad = airPadCells(input.mCell);
    static LLCachedControl<F32> airMinGap(gSavedSettings, "SKNavAirMinGap", 0.5f);
    input.mAirMinGap = llclamp(static_cast<F32>(airMinGap), 0.f, 4.f);
    input.mTopParams.mClimb = climb;

    // The raster's faces, ledges and run-off are for Atmo's readers; AVATAR's edges and drops for the map.
    input.mWallFaces = walls && raster_layers;
    input.mRunoff = walls && raster_layers;
    input.mRunoffMax = dropWeather;
    input.mEdges[static_cast<S32>(AgentClass::AVATAR)] = walls;
    input.mDropMax[static_cast<S32>(AgentClass::AVATAR)] = dropAvatar;
    input.mDropSpacing = llmax(0.5f, static_cast<F32>(dropSpacing));
}

U64 SKNavMesh::terrainSignature(const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad) const
{
    // Every patch under the padded tile, by its last update and height range:
    // exact and cheap, and it never churns. The regions are the ones under
    // the padded corners, the same lookup the terrain window uses.
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
                const LLSurfacePatch* patch = land.getPatch(x, y);
                if (patch)
                {
                    h = mix(h, patch->getLastUpdateTime());
                    h = mixF32(h, patch->getMinZ());
                    h = mixF32(h, patch->getMaxZ());
                }
            }
        }
        h = mixF32(h, region->getWaterHeight());
    }
    return h;
}

U64 SKNavMesh::landStamp(LLViewerRegion& region) const
{
    // The whole region's land in one value, so a tile can tell in one compare
    // that nothing under it moved. Cheaper than re-signing every tile of the
    // region, which is what this replaces.
    LLSurface& land = region.getLand();
    const S32 per_edge = land.getPatchesPerEdge();
    U64 h = mix(SEED, region.getHandle());
    for (S32 y = 0; y < per_edge; ++y)
    {
        for (S32 x = 0; x < per_edge; ++x)
        {
            const LLSurfacePatch* patch = land.getPatch(x, y);
            if (patch)
            {
                h = mix(h, patch->getLastUpdateTime());
                h = mixF32(h, patch->getMinZ());
                h = mixF32(h, patch->getMaxZ());
            }
        }
    }
    return mixF32(h, region.getWaterHeight());
}

U64 SKNavMesh::worldStamp() const
{
    // Which regions there are, so a tile's terrain signature is redone when
    // one arrives or leaves: its window reaches past its own region's edge.
    U64 h = SEED;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        h = mix(h, region->getHandle());
    }
    return h;
}

void SKNavMesh::fillTerrain(const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad, TerrainWindow& out) const
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
            // Regions are 256 m squares; the last one answers most samples.
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
                // No region here: the void. A deep floor keeps the window
                // rectangular without offering anything to stand on.
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

bool SKNavMesh::scanTiles(TileCursor& cursor, F32 seconds,
    const std::function<void(Mesh&, const TileKey&, Tile&)>& visit)
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

void SKNavMesh::sampleTerrain()
{
    static LLCachedControl<F32> budget(gSavedSettings, "SKNavTerrainBudgetMS", 1.f);
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = tilePad(probe);
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

bool SKNavMesh::grounded(const Cluster& cluster) const
{
    static LLCachedControl<F32> margin(gSavedSettings, "SKNavGroundedMargin", 5.f);
    if (!LLWorld::instanceExists())
    {
        return false;
    }
    // The highest ground under the cluster's footprint: a structure on a slope
    // stands on the high side of it.
    LLWorld* world = LLWorld::getInstance();
    const LLVector3d corners[5] = {
        cluster.mMin,
        LLVector3d(cluster.mMax.mdV[VX], cluster.mMin.mdV[VY], 0.0),
        LLVector3d(cluster.mMin.mdV[VX], cluster.mMax.mdV[VY], 0.0),
        cluster.mMax,
        (cluster.mMin + cluster.mMax) * 0.5,
    };
    F32 land = -1e9f;
    for (const LLVector3d& corner : corners)
    {
        land = llmax(land, static_cast<F32>(world->resolveLandHeightGlobal(corner)));
    }
    return static_cast<F32>(cluster.mMin.mdV[VZ]) - land <= margin;
}

void SKNavMesh::reconcile(const SKCensusSnapshotPtr& snap)
{
    auto workers = workerQueue();
    if (!workers) return;
    const U64 params = paramsSignature();
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = tilePad(probe);
    const U64 world_stamp = worldStamp();
    static LLCachedControl<F32> void_margin(gSavedSettings, "SKNavVoidMargin", 256.f);
    auto ground = std::make_shared<std::vector<ClusterPtr>>();
    std::vector<ClusterPtr> airborne;
    for (const auto& cluster : snap->mClusters.mClusters)
    {
        (grounded(*cluster) ? *ground : airborne).push_back(cluster);
    }
    auto regions = std::make_shared<std::vector<RegionSquare>>();
    for (auto* region : LLWorld::getInstance()->getRegionList())
        regions->push_back({ region->getOriginGlobal(), region->getWidth() });

    const auto submit = [&](Mesh& mesh, ClusterPtr cluster, LLViewerRegion* region)
    {
        mesh.mLastSeen = gFrameTimeSeconds;
        const U64 land = region ? landStamp(*region) : 0;
        if (mesh.mLayoutJob.valid() || mesh.mPendingLayout ||
            (mesh.mLayoutCensus == snap->mStamp && mesh.mLayoutWorld == world_stamp &&
             mesh.mLayoutLand == land && mesh.mLayoutParams == params && mesh.mLayoutVoidMargin == void_margin)) return;
        auto layout = std::make_shared<Layout>();
        layout->mCensus = snap->mStamp;
        layout->mWorld = world_stamp;
        layout->mLand = land;
        layout->mParams = params;
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
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - lay airborne worker");
                    layout->mRefused = !layAirborne(*cluster, origin, layout->mPad, layout->mTiles);
                }
                else
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - lay region worker");
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
        it = mMeshes.erase(it);
        ++mRevision;
    }
}

void SKNavMesh::applyLayouts()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - publish layout");
    LLTimer timer;
    auto workers = workerQueue();
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        if (mesh.mLayoutJob.valid() && mesh.mLayoutJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            mesh.mPendingLayout = mesh.mLayoutJob.get();
            if (mesh.mPendingLayout && mesh.mPendingLayout->mRefused)
            {
                mStats.mRefused++;
                LL_WARNS("SKNav") << "mesh " << mesh.mId << " wants more than " << MAX_TILES_PER_MESH << " tiles; left unbuilt ("
                                  << mStats.mRefused << " refused)" << LL_ENDL;
            }
        }
        if (!mesh.mPendingLayout) continue;
        auto& layout = *mesh.mPendingLayout;
        // A region or parameter change invalidates the whole result before publication.
        if (layout.mWorld != worldStamp() || layout.mParams != paramsSignature())
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
                    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - layout terrain signature");
                    terrain = terrainSignature(tile.mMin, tile.mMax, layout.mPad);
                    tile.mTerrainStale = true;
                }
                tile.mLandStamp = layout.mLand;
                tile.mWorldStamp = layout.mWorld;
            }
            else { tile.mTerrain.reset(); tile.mTerrainStale = false; }
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - layout resign");
            LL_PROFILE_ZONE_NUM(tile.mRecords.size());
            reSign(tile, before, layout.mSignatures[index], terrain, layout.mParams);
            tile.mTerrainSignature = terrain;
            laid.mRecords.swap(before); // Release the old records with the result on a worker.
        }
        if (!mesh.mDropping) { mesh.mDropCursor = mesh.mTiles.begin(); mesh.mDropping = true; }
        while (mesh.mDropCursor != mesh.mTiles.end())
        {
            if (timer.getElapsedTimeF32() > 0.002f) return;
            auto current = mesh.mDropCursor++;
            if (layout.mKeys.count(current->first)) continue;
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - layout drop tile");
            for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                if (mesh.mNav[cls]) mesh.mNav[cls]->removeTile(mesh.mNav[cls]->getTileRefAt(current->first.mY, current->first.mX, 0), nullptr, nullptr);
            mSlotSpaces.erase(current->second.mStampSlot);
            auto retired = std::make_shared<Tile>(std::move(current->second));
            retireOn(workers, std::move(retired));
            mesh.mTiles.erase(current);
            ++mRevision;
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

U64 SKNavMesh::recordSignature(const std::vector<RecordPtr>& records)
{
    // Order independent: the grid hands the same records back in whatever
    // order the census scan produced them, and a signature that changes with
    // that order rebuilds the tile on every scan.
    U64 h = 0;
    for (const RecordPtr& r : records)
    {
        h ^= mix(SEED, r->mSignature);
    }
    return h;
}

void SKNavMesh::reSign(Tile& tile, const std::vector<RecordPtr>& before, U64 records, U64 terrain, U64 params)
{
    // The old signatures are what says which input moved: the mixed signature
    // alone cannot tell a record from the land from the parameters.
    const bool records_moved = records != tile.mRecordSignature;
    const bool terrain_moved = terrain != tile.mTerrainSignature;
    tile.mRecordSignature = records;
    tile.mSignature = mix(mix(records, terrain), params);
    if (tile.mSignature == tile.mBuilt)
    {
        return;     // what it was built from is what it would be built from now
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

bool SKNavMesh::changedRecord(const std::vector<RecordPtr>& before, const std::vector<RecordPtr>& after, LLVector3d& out)
{
    // The census reuses a part's record while its signature holds, so a
    // pointer that is new here is a part that appeared or changed, and one
    // that is gone is a part that left.
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

void SKNavMesh::tileBlip(const Mesh& mesh, const Tile& tile) const
{
    static LLCachedControl<bool> show(gSavedSettings, "SKNavOverlayUpdates", true);
    if (!show || tile.mCause == CAUSE_NONE)
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
    // The blip rises from the tile's own floor, which is the land under it for
    // a region tile and the middle of its own height range otherwise.
    LLVector3d at = (tile.mMin + tile.mMax) * 0.5;
    if (mesh.mTerrain && tile.mOverLand && LLWorld::instanceExists())
    {
        at.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(at);
    }
    else
    {
        at.mdV[VZ] = (tile.mMin.mdV[VZ] + tile.mMax.mdV[VZ]) * 0.5;
    }
    const LLVector3 pos = gAgent.getPosAgentFromGlobal(at);
    if (tile.mHasCauseAt)
    {
        gPipeline.addDebugBlip(pos, color, gAgent.getPosAgentFromGlobal(tile.mCauseAt));
    }
    else
    {
        gPipeline.addDebugBlip(pos, color);
    }
}

bool SKNavMesh::prepareInput(const Mesh& mesh, const TileKey& key, Tile& tile, std::shared_ptr<TileBuildInput>& input,
                             std::shared_ptr<const std::vector<RecordPtr>>& records) const
{
    input = std::make_shared<TileBuildInput>();
    input->mMeshOrigin = mesh.mOrigin;
    input->mTileX = key.mX;
    input->mTileY = key.mY;
    input->mTileSize = static_cast<F32>(TILE_M);
    fillParams(*input);
    // Air is built eagerly near the camera; a tile further out keeps any it has, else reads coarse air for its stamp.
    if (input->mAir && !(tile.mAir && tile.mAirBuilt) && !inAirRadius(tile))
    {
        input->mAir = false;
        input->mCoarseAir = true;
    }
    // A tile with air of its own keeps its solid raster, so an edit rasterises only its own cells.
    input->mHoldSolid = input->mAir;
    // Only a region mesh has ground in it, and only its own ground: a cluster
    // clear of the ground has nothing to do with the land far below it, and a
    // tile past the region's edge is standing on land another region meshes.
    // The window was sampled when the land last moved; until the budget has
    // got to it, the tile waits rather than build on ground that is gone.
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
        if (record && record->mNavRole != NavRole::EXCLUSION_VOLUME) input->mRecords.push_back(record);
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
    input->mRecords.clear();
    return true;
}

bool SKNavMesh::launch(Mesh& mesh, const TileKey& key, Tile& tile)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t workers = workerQueue();
    if (!main_queue || !workers || workers->isClosed())
    {
        return false;
    }
    std::shared_ptr<TileBuildInput> input;
    std::shared_ptr<const std::vector<RecordPtr>> records;
    if (!prepareInput(mesh, key, tile, input, records))
    {
        return false;
    }

    const LLUUID id = mesh.mId;
    const U32 generation = mesh.mGeneration;
    const U64 signature = tile.mSignature;
    const std::shared_ptr<TileDiskCache> cache = diskCache();
    // A stored product Detour refused last time is built afresh, and overwritten.
    const bool skip_load = tile.mDiskRefused;
    tile.mDiskRefused = false;
    if (tile.mRasterDropped)
    {
        ++mStats.mReRasters;
        tile.mRasterDropped = false;
    }
    struct Outcome
    {
        std::shared_ptr<TileBuildResult> mResult;
        U64 mKey = 0;
        bool mLooked = false;
        bool mLoaded = false;
        bool mCorrupt = false;
        F32 mLoadMs = 0.f;
        TileRasterPtr mRaster;
        F32 mRasterMs = 0.f;
        bool mUpdated = false;      ///< built from the held raster rather than from nothing
        UpdatePath mPath = UpdatePath::NO_SOLID;
    };
    // Whatever raster the update starts from, it makes the tile a whole raster of this input makes.
    const TileRasterPtr held = tile.mRaster;
    const bool posted = main_queue->postTo(
        workers,
        [input, records, cache, skip_load, held]()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - build tile worker");
            finishTileInput(*records, input->mMeshOrigin, *input);
            auto out = std::make_shared<Outcome>();
            // What the tile is built from, keyed so the same world finds it on disk
            // next session; hashed here, since a big mesh's triangles are not cheap.
            const U64 content_key = tileContentKey(*input);
            out->mKey = content_key;
            if (cache && !skip_load)
            {
                out->mLooked = true;
                LLTimer timer;
                std::vector<U8> bytes;
                if (cache->load(content_key, bytes))
                {
                    auto loaded = std::make_shared<TileBuildResult>();
                    if (unpackTileProduct(bytes.data(), bytes.size(), content_key, *loaded))
                    {
                        out->mResult = loaded;
                        out->mLoaded = true;
                        out->mLoadMs = timer.getElapsedTimeF32() * 1000.f;
                        loaded->mMillis = out->mLoadMs;     // what this tile cost now, not when it was built
                        return out;
                    }
                    // Damaged or written by another version: gone, and built afresh.
                    cache->erase(content_key);
                    out->mCorrupt = true;
                }
            }
            out->mResult = std::make_shared<TileBuildResult>();
            TileBuildResult& result = *out->mResult;
            // Past the radius too: the raster makes the coarse air from its own soup, and is held for the stamp.
            TileUpdate update;
            out->mUpdated = held != nullptr;
            if (TileRasterPtr raster = held ? updateTile(*input, *held, result, nullptr, &update) : rasteriseTile(*input, result))
            {
                out->mPath = update.mPath;
                out->mRasterMs = result.mMillis;
                result.mStatus = TileBuildResult::SUCCESS;
                if (awaitsStamp(result))
                {
                    // Its polygons wait for the spaces over its air.
                    out->mRaster = std::move(raster);
                }
                else
                {
                    // No air, so nothing to stamp them with: they are final now.
                    polygoniseTile(*input, *raster, result);
                }
            }
            if (cache && result.mStatus != TileBuildResult::FAILED)
            {
                cache->store(content_key, packTileProduct(result, content_key));
            }
            return out;
        },
        [this, id, generation, key, signature](std::shared_ptr<Outcome> out)
        {
            if (out->mLoaded)
            {
                mStats.mDiskLoads++;
                mStats.mDiskLoadMs = out->mLoadMs;
            }
            else if (out->mLooked)
            {
                mStats.mDiskRebuilds++;
                mStats.mDiskCorrupt += out->mCorrupt ? 1 : 0;
            }
            if (!out->mLoaded && out->mResult->mStatus == TileBuildResult::SUCCESS)
            {
                mStats.mLastRasterMs = out->mRasterMs;
                mStats.mLastPolyMs = out->mResult->mMillis - out->mRasterMs;
                if (out->mUpdated && out->mPath == UpdatePath::LOCAL)
                {
                    ++mStats.mLocalUpdates;
                    mStats.mLastUpdateMs = out->mRasterMs;
                }
                else if (out->mUpdated)
                {
                    ++mStats.mUpdateFallbacks;
                }
            }
            publish(id, generation, key, signature, out->mResult, out->mKey, out->mLoaded, std::move(out->mRaster));
        });
    if (posted)
    {
        tile.mInFlight = true;
        mInFlight++;
    }
    return posted;
}

void SKNavMesh::publish(const LLUUID& mesh_id, U32 generation, const TileKey& key, U64 signature, std::shared_ptr<TileBuildResult> result,
                        U64 content_key, bool loaded, TileRasterPtr raster)
{
    bool refused = false;
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
        return;     // superseded: the scheduler will launch it again
    }
    mStats.mBuilds++;
    if (result->mStatus == TileBuildResult::SUCCESS)
    {
        mStats.mLastBuildMs = result->mMillis;
        mStats.mWorstBuildMs = std::max(mStats.mWorstBuildMs, result->mMillis);
        if (result->mExcludeMillis > 0.f)
        {
            mStats.mExcludeMs = result->mExcludeMillis;
        }
    }
    if (result->mStatus == TileBuildResult::FAILED)
    {
        mStats.mFailures++;
        LL_WARNS("SKNav") << "tile build failed: " << result->mError << LL_ENDL;
        // The walls of the build before this one describe geometry that has
        // changed; drop them rather than answer with them.
        tile.mFaces.reset();
        tile.mLedges.reset();
        tile.mRunoff.reset();
        tile.mAir.reset();
        tile.mAirCoarse.reset();
        tile.mRaster.reset();
        tile.mContentKey = 0;
        tile.mAirBuilt = signature;
        for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
        {
            tile.mSegments[cls].reset();
            tile.mLinks[cls].reset();
            tile.mFlights[cls].reset();
            tile.mPolyCache[cls].reset();
        }
        tile.mBuilt = signature;    // do not retry the same input
        tile.mCause = CAUSE_FAILED;
        tile.mHasCauseAt = false;
        tileBlip(mesh, tile);
        mRevision++;                // the tile's walls are gone: the overlay moved
        return;
    }
    if (awaitsStamp(*result))
    {
        // The raster stage alone: its air goes to the spaces, and the polygons
        // published before stay until the stamped ones replace them.
        tile.mFaces = std::make_shared<const std::vector<WallFace>>(std::move(result->mFaces));
        tile.mLedges = std::make_shared<const std::vector<WallSegment>>(std::move(result->mLedges));
        tile.mRunoff = std::make_shared<const std::vector<DropLink>>(std::move(result->mRunoff));
        tile.mAir = result->mAir;
        tile.mAirBuilt = result->mCoarseAir ? 0 : signature;
        tile.mAirCoarse.reset();
        tile.mContentKey = content_key;
        tile.mRaster = std::move(raster);
        tile.mBuilt = signature;
        tile.mMillis = result->mMillis;
        if (tile.mStaticAt <= 0.0)
        {
            tile.mStaticAt = LLTimer::getTotalSeconds();
        }
        tile.mStampDue = true;
        mesh.mStampScan = true;
        tileBlip(mesh, tile);
        mRevision++;                // new air: the spaces over it are cut again
        return;
    }
    bool kept = false;
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        if (!mesh.mNav[cls] && result->mData[cls].empty())
        {
            tile.mPolys[cls] = result->mPolys[cls];
            continue;
        }
        if (!ensureNav(mesh, cls))
        {
            tile.mPolys[cls] = result->mPolys[cls];
            continue;
        }
        // Detour's x runs along Recast x, which is SL y. The tile keeps its bytes: refused, the old tile stays.
        const bool swapped = swapDetourTile(*mesh.mNav[cls], key.mY, key.mX, tile.mDetour[cls], result->mData[cls]);
        if (!swapped)
        {
            mStats.mFailures++;
            refused = loaded;
            LL_WARNS("SKNav") << "addTile failed for mesh " << mesh_id << " tile " << key.mX << "," << key.mY << LL_ENDL;
            if (!tile.mDetour[cls].empty())
            {
                kept = true;
                continue;
            }
        }
        tile.mPolys[cls] = swapped ? result->mPolys[cls] : 0;
        tile.mPolyCache[cls].reset();
    }
    // A refused tile leaves the published one whole, products and all: built again only from a fresh build.
    if (kept)
    {
        tile.mBuilt = refused ? 0 : signature;
        tile.mDiskRefused = refused;
        tileBlip(mesh, tile);
        return;
    }
    tile.mFaces = std::make_shared<const std::vector<WallFace>>(std::move(result->mFaces));
    tile.mLedges = std::make_shared<const std::vector<WallSegment>>(std::move(result->mLedges));
    tile.mRunoff = std::make_shared<const std::vector<DropLink>>(std::move(result->mRunoff));
    tile.mAir = result->mAirBuilt ? result->mAir : nullptr;
    tile.mAirBuilt = result->mAirBuilt && !result->mCoarseAir ? signature : 0;
    // Nothing to stamp this tile with: its polygons are final as built.
    mStats.mUnstampedPublishes += result->mAir && !result->mAir->mLeaves.empty() ? 1 : 0;
    tile.mStampInput.reset();
    tile.mStampGeom.reset();
    tile.mBinding.reset();
    tile.mStampedBy.reset();
    tile.mStaticAt = 0.0;
    tile.mStampDue = false;
    // A tile rebuilt without its air keeps the tops it had only while its input holds.
    if (tile.mAir || tile.mBuilt != signature)
    {
        tile.mAirCoarse.reset();
    }
    tile.mContentKey = refused ? 0 : content_key;
    tile.mRaster = std::move(raster);
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        tile.mSegments[cls] = std::make_shared<const std::vector<WallSegment>>(std::move(result->mSegments[cls]));
        tile.mLinks[cls] = std::make_shared<const std::vector<DropLink>>(std::move(result->mLinks[cls]));
        tile.mFlights[cls] = result->mFlights[cls].empty() ? nullptr : std::make_shared<const std::vector<Flight>>(std::move(result->mFlights[cls]));
    }
    tile.mBuilt = signature;
    tile.mMillis = result->mMillis;
    if (refused)
    {
        tile.mBuilt = 0;            // scheduled again, and built rather than loaded
        tile.mDiskRefused = true;
    }
    // The floors beside a ladder or a shaft may have moved with the tile.
    for (auto& region : mFeatureLinks)
    {
        for (auto& entry : region.second)
        {
            if (overlapsXY(entry.second.mMin, entry.second.mMax, tile.mMin, tile.mMax))
            {
                entry.second.mDirty = true;
            }
        }
    }
    tileBlip(mesh, tile);
    mRevision++;                    // a published tile is one the overlay redraws
}

struct SKNavMesh::StampOutcome
{
    enum Kind : U8 { DROP, REBIND, STAMPED } mKind = DROP;
    std::shared_ptr<TileStampInput> mInput;
    std::shared_ptr<TileBuildResult> mResult;   ///< STAMPED: the polygon stage's products
    std::shared_ptr<TileBinding> mBinding;
    TileRasterPtr mRaster;                      ///< rasterised again for want of a held one
    std::shared_ptr<const std::vector<WallSegment>> mSegments;  ///< REBIND: the published segments bound again
    std::shared_ptr<const std::vector<WallFace>> mFaces;        ///< the published faces bound to these spaces
    bool mLoaded = false;
    F32 mMillis = 0.f;
};

void SKNavMesh::stampTiles()
{
    static LLCachedControl<S32> per_frame(gSavedSettings, "SKNavBuildsPerFrame", 4);
    static LLCachedControl<S32> in_flight_max(gSavedSettings, "SKNavBuildsInFlight", 0);
    if (!workerQueue())
    {
        return;
    }
    const S32 in_flight_cap = in_flight_max > 0 ? static_cast<S32>(in_flight_max) : static_cast<S32>(mWorkers->getWidth());
    S32 launched = 0;
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        EnclosureProductPtr enc;
        RoomsProductPtr rooms;
        if (!mRooms->landed(mesh.mId, enc, rooms))
        {
            continue;
        }
        if (rooms != mesh.mStampRooms)
        {
            // New spaces: every tile under them is read against them again.
            mesh.mStampRooms = rooms;
            mesh.mStampEnc = enc;
            for (auto& t : mesh.mTiles)
            {
                t.second.mStampDue = t.second.mStampDue || (t.second.mAir && !t.second.mAir->mLeaves.empty());
            }
            mesh.mStampScan = true;
        }
        if (!mesh.mStampScan)
        {
            continue;
        }
        bool pending = false;
        for (auto& t : mesh.mTiles)
        {
            Tile& tile = t.second;
            if (!tile.mStampDue || tile.mStampInFlight)
            {
                pending = pending || tile.mStampDue;
                continue;
            }
            // Only against the answers for exactly this air; a newer pass will come for newer air.
            const TileEnclosure* te = tile.mAir ? enc->tile(t.first) : nullptr;
            if (!te || te->mAir != tile.mAir || te->mGaps.size() != tile.mAir->mGaps.size())
            {
                pending = true;
                continue;
            }
            if (tile.mSignature != tile.mBuilt || !tile.mBuilt)
            {
                pending = true;     // its raster stage is about to publish new air
                continue;
            }
            if (tile.mInFlight || launched >= per_frame || mInFlight >= in_flight_cap)
            {
                pending = true;
                continue;
            }
            if (launchStamp(mesh, t.first, tile))
            {
                ++launched;
            }
            else
            {
                pending = true;
            }
        }
        mesh.mStampScan = pending;
    }
}

bool SKNavMesh::launchStamp(Mesh& mesh, const TileKey& key, Tile& tile)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t workers = workerQueue();
    if (!main_queue || !workers || workers->isClosed())
    {
        return false;
    }
    std::shared_ptr<TileBuildInput> input;
    std::shared_ptr<const std::vector<RecordPtr>> records;
    if (!prepareInput(mesh, key, tile, input, records))
    {
        if (!tile.mRaster)
        {
            return false;
        }
        // A held raster needs only what the polygon stage reads of the input.
        input = std::make_shared<TileBuildInput>();
        input->mMeshOrigin = mesh.mOrigin;
        input->mTileX = key.mX;
        input->mTileY = key.mY;
        input->mTileSize = static_cast<F32>(TILE_M);
        fillParams(*input);
        records = std::make_shared<const std::vector<RecordPtr>>();
    }
    const LLUUID id = mesh.mId;
    const U32 generation = mesh.mGeneration;
    const U64 signature = tile.mSignature;
    const TileAirPtr air = tile.mAir;
    const EnclosureProductPtr enc = mesh.mStampEnc;
    const RoomsProductPtr rooms = mesh.mStampRooms;
    const TileRasterPtr raster = tile.mRaster;
    // The published polys rebind only if they were built from this same raster.
    const std::shared_ptr<const TileStampGeom> published = tile.mStampBuilt == signature ? tile.mStampGeom : nullptr;
    const auto published_segments = tile.mSegments[static_cast<S32>(AgentClass::AVATAR)];
    const auto published_faces = tile.mFaces;
    const U64 content_key = tile.mContentKey;
    const std::shared_ptr<TileDiskCache> cache = diskCache();
    const LLVector3 lo(tile.mMin - mesh.mOrigin), hi(tile.mMax - mesh.mOrigin);
    const bool posted = main_queue->postTo(
        workers,
        [input, records, air, enc, rooms, raster, published, published_segments, published_faces, content_key, cache, lo, hi, key]()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - stamp tile worker");
            LLTimer timer;
            auto out = std::make_shared<StampOutcome>();
            const TileEnclosure* te = enc->tile(key);
            if (!te || te->mAir != air)
            {
                return out;
            }
            out->mInput = std::make_shared<TileStampInput>();
            stampInput(*air, *te, *rooms, lo, hi, *out->mInput);
            const TileStampInput& in = *out->mInput;
            out->mBinding = std::make_shared<TileBinding>();
            // The same partition stamps the same polys: only the binding to the spaces moved.
            if (published && published->mPattern == in.mPattern)
            {
                bindStamp(*published, in, te, *rooms, *out->mBinding);
                // The walls the tile has, bound to the spaces standing now.
                if (published_segments)
                {
                    auto segments = std::make_shared<std::vector<WallSegment>>(*published_segments);
                    bindWallSpaces(*enc, *segments);
                    out->mSegments = segments;
                }
                if (published_faces)
                {
                    auto faces = std::make_shared<std::vector<WallFace>>(*published_faces);
                    bindWallSpaces(*enc, *faces);
                    out->mFaces = faces;
                }
                out->mKind = StampOutcome::REBIND;
                out->mMillis = timer.getElapsedTimeF32() * 1000.f;
                return out;
            }
            const U64 stamp_key = content_key ? stampedKey(content_key, in.mPattern) : 0;
            if (cache && stamp_key)
            {
                std::vector<U8> bytes;
                auto loaded = std::make_shared<TileBuildResult>();
                if (cache->load(stamp_key, bytes) && unpackTileProduct(bytes.data(), bytes.size(), stamp_key, *loaded) && loaded->mStamp.mStamped &&
                    loaded->mStamp.mPattern == in.mPattern)
                {
                    out->mResult = loaded;
                    out->mLoaded = true;
                }
            }
            if (!out->mLoaded)
            {
                TileRasterPtr held = raster;
                if (!held)
                {
                    finishTileInput(*records, input->mMeshOrigin, *input);
                    auto again = std::make_shared<TileBuildResult>();
                    held = rasteriseTile(*input, *again);
                    out->mRaster = held;
                    out->mResult = again;
                }
                if (!held || !held->mChf)
                {
                    return out;
                }
                if (!out->mResult)
                {
                    out->mResult = std::make_shared<TileBuildResult>();
                }
                TileStamp stamp;
                stampTile(*held->mChf, borderCells(input->mParams[static_cast<S32>(AgentClass::AVATAR)].mRadius, input->mCell), held->mSpanGap, in,
                          held->mReal, held->mFlights, StampParams(), stamp);
                polygoniseTile(*input, *held, *out->mResult, nullptr, &in, &stamp);
                if (cache && stamp_key && out->mResult->mStatus == TileBuildResult::SUCCESS && !out->mResult->mPolyOverflow)
                {
                    cache->store(stamp_key, packTileProduct(stampedPart(*out->mResult), stamp_key));
                }
            }
            bindStamp(out->mResult->mStamp, in, te, *rooms, *out->mBinding);
            bindWallSpaces(*enc, out->mResult->mSegments[static_cast<S32>(AgentClass::AVATAR)]);
            if (out->mRaster)
            {
                bindWallSpaces(*enc, out->mResult->mFaces);
            }
            else if (published_faces)
            {
                auto faces = std::make_shared<std::vector<WallFace>>(*published_faces);
                bindWallSpaces(*enc, *faces);
                out->mFaces = faces;
            }
            out->mKind = StampOutcome::STAMPED;
            out->mMillis = timer.getElapsedTimeF32() * 1000.f;
            return out;
        },
        [this, id, generation, key, signature, air, rooms](std::shared_ptr<StampOutcome> out)
        {
            publishStamped(id, generation, key, signature, air, rooms, out);
        });
    if (posted)
    {
        tile.mStampInFlight = true;
        tile.mStampDue = false;
        mInFlight++;
    }
    return posted;
}

void SKNavMesh::publishStamped(const LLUUID& mesh_id, U32 generation, const TileKey& key, U64 signature, const TileAirPtr& air,
                               const RoomsProductPtr& rooms, const std::shared_ptr<StampOutcome>& out)
{
    mInFlight = std::max(0, mInFlight - 1);
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end() || it->second->mGeneration != generation)
    {
        return;
    }
    Mesh& mesh = *it->second;
    const auto tit = mesh.mTiles.find(key);
    if (tit == mesh.mTiles.end())
    {
        return;
    }
    Tile& tile = tit->second;
    tile.mStampInFlight = false;
    // One ticket: the input, the air and the spaces it was stamped with must all still stand.
    if (tile.mSignature != signature || tile.mAir != air || mesh.mStampRooms != rooms || out->mKind == StampOutcome::DROP)
    {
        tile.mStampDue = tile.mAir && !tile.mAir->mLeaves.empty();
        mesh.mStampScan = mesh.mStampScan || tile.mStampDue;
        return;
    }
    mStats.mLastStampMs = out->mMillis;
    if (out->mKind == StampOutcome::REBIND)
    {
        tile.mStampInput = out->mInput;
        tile.mBinding = out->mBinding;
        tile.mStampedBy = rooms;
        if (out->mSegments)
        {
            tile.mSegments[static_cast<S32>(AgentClass::AVATAR)] = out->mSegments;
        }
        if (out->mFaces)
        {
            tile.mFaces = out->mFaces;
        }
        applyBinding(mesh, key, tile);
        ++mStats.mRebinds;
        mRevision++;
        return;
    }
    const std::shared_ptr<TileBuildResult>& result = out->mResult;
    const S32 avatar = static_cast<S32>(AgentClass::AVATAR);
    if (result->mPolyOverflow)
    {
        ++mStats.mPolyOverflowTiles;
        LL_WARNS("SKNav") << "tile " << key.mX << "," << key.mY << " of mesh " << mesh_id << " has " << result->mPolys[avatar]
                          << " polys, past a Detour tile's " << MAX_POLYS_PER_TILE << ": not published" << LL_ENDL;
        return;
    }
    if (result->mStatus != TileBuildResult::SUCCESS)
    {
        mStats.mFailures++;
        LL_WARNS("SKNav") << "stamped build failed: " << result->mError << LL_ENDL;
        return;
    }
    if (result->mStamp.mOverflow != TileStamp::FIT)
    {
        ++mStats.mStampOverflow;
        LL_INFOS("SKNav") << "tile " << key.mX << "," << key.mY << " of mesh " << mesh_id << ": stamp palette over its codes, cascade stage "
                          << static_cast<S32>(result->mStamp.mOverflow) << ", " << result->mStamp.mPairs << " codes" << LL_ENDL;
        // One colour for everything is a stamp in name only: the stamped tile already there is the better one.
        if (result->mStamp.mOverflow == TileStamp::ONE_COLOUR && !tile.mDetour[avatar].empty() && tile.mStampGeom)
        {
            return;
        }
    }
    if (!ensureNav(mesh, avatar))
    {
        return;
    }
    // The header's user id is the filter's key to this tile's polys' spaces.
    if (!tile.mStampSlot)
    {
        tile.mStampSlot = ++mNextSlot;
    }
    std::vector<U8>& data = result->mData[avatar];
    if (data.size() >= sizeof(dtMeshHeader))
    {
        reinterpret_cast<dtMeshHeader*>(data.data())->userId = tile.mStampSlot;
    }
    const bool swapped = swapDetourTile(*mesh.mNav[avatar], key.mY, key.mX, tile.mDetour[avatar], data);
    if (!swapped)
    {
        mStats.mFailures++;
        LL_WARNS("SKNav") << "addTile refused the stamped tile for mesh " << mesh_id << " tile " << key.mX << "," << key.mY << LL_ENDL;
        return;
    }
    tile.mPolys[avatar] = result->mPolys[avatar];
    tile.mPolyCache[avatar].reset();
    tile.mSegments[avatar] = std::make_shared<const std::vector<WallSegment>>(std::move(result->mSegments[avatar]));
    tile.mLinks[avatar] = std::make_shared<const std::vector<DropLink>>(std::move(result->mLinks[avatar]));
    tile.mFlights[avatar] = result->mFlights[avatar].empty() ? nullptr : std::make_shared<const std::vector<Flight>>(std::move(result->mFlights[avatar]));
    if (out->mRaster)
    {
        // Rasterised again: its faces are the tile's own; its air is the same air under another pointer, and is not swapped.
        tile.mFaces = std::make_shared<const std::vector<WallFace>>(std::move(result->mFaces));
        tile.mLedges = std::make_shared<const std::vector<WallSegment>>(std::move(result->mLedges));
        tile.mRunoff = std::make_shared<const std::vector<DropLink>>(std::move(result->mRunoff));
        ++mStats.mStampReRasters;
    }
    else if (out->mFaces)
    {
        tile.mFaces = out->mFaces;
    }
    tile.mStampGeom = std::make_shared<const TileStampGeom>(std::move(result->mStamp));
    tile.mStampBuilt = signature;
    tile.mStampInput = out->mInput;
    tile.mBinding = out->mBinding;
    tile.mStampedBy = rooms;
    applyBinding(mesh, key, tile);
    ++mStats.mStamps;
    mStats.mStampLoads += out->mLoaded ? 1 : 0;
    if (tile.mStaticAt > 0.0)
    {
        mStats.mLastStampLatencyMs = static_cast<F32>((LLTimer::getTotalSeconds() - tile.mStaticAt) * 1000.0);
        mStats.mWorstStampLatencyMs = std::max(mStats.mWorstStampLatencyMs, mStats.mLastStampLatencyMs);
        tile.mStaticAt = 0.0;
    }
    // Past the radius the raster is not held once its polygons are built.
    if (!inAirRadius(tile))
    {
        tile.mRaster.reset();
    }
    for (auto& region : mFeatureLinks)
    {
        for (auto& entry : region.second)
        {
            if (overlapsXY(entry.second.mMin, entry.second.mMax, tile.mMin, tile.mMax))
            {
                entry.second.mDirty = true;
            }
        }
    }
    tileBlip(mesh, tile);
    mRevision++;
}

void SKNavMesh::applyBinding(Mesh& mesh, const TileKey& key, Tile& tile)
{
    const S32 avatar = static_cast<S32>(AgentClass::AVATAR);
    if (!tile.mBinding || !tile.mStampGeom || !mesh.mNav[avatar])
    {
        return;
    }
    const TileBinding& b = *tile.mBinding;
    dtNavMesh& nav = *mesh.mNav[avatar];
    if (const dtMeshTile* dt = nav.getTileAt(key.mY, key.mX, 0); dt && dt->header)
    {
        const dtPolyRef base = nav.getPolyRefBase(dt);
        for (S32 p = 0; p < dt->header->polyCount && p < static_cast<S32>(b.mPolyFlags.size()); ++p)
        {
            const unsigned short flags = dt->polys[p].flags;
            if (flags)
            {
                nav.setPolyFlags(base | static_cast<dtPolyRef>(p), static_cast<unsigned short>((flags & ~POLY_BINDING_FLAGS) | b.mPolyFlags[p]));
            }
        }
    }
    auto spaces = std::make_shared<std::vector<U32>>(tile.mStampGeom->mPolyComponent.size(), 0);
    for (size_t p = 0; p < spaces->size(); ++p)
    {
        const U16 c = tile.mStampGeom->mPolyComponent[p];
        (*spaces)[p] = c < b.mComponentSpaceId.size() ? b.mComponentSpaceId[c] : 0;
    }
    mSlotSpaces[tile.mStampSlot] = spaces;
    tile.mPolySpace = spaces;
    const TileStampGeom& geom = *tile.mStampGeom;
    auto edges = std::make_shared<std::vector<PortalEdge>>();
    const auto space_of = [&b](U16 c) { return c < b.mComponentSpaceId.size() ? b.mComponentSpaceId[c] : 0u; };
    for (size_t i = 0; i < geom.mPortalEdges.size(); ++i)
    {
        const StampPortalEdge& e = geom.mPortalEdges[i];
        if (e.mPortal >= geom.mPortals.size() || e.mSide > 1)
        {
            continue;
        }
        const StampPortal& portal = geom.mPortals[e.mPortal];
        PortalEdge out;
        out.mPoly = e.mPoly;
        out.mEdge = e.mEdge;
        out.mKind = static_cast<PortalKind>(portal.mKind);
        out.mPortal = i < b.mPortalKeys.size() ? b.mPortalKeys[i] : 0;
        out.mInside = space_of(portal.mSide[e.mSide]);
        out.mAcross = space_of(portal.mSide[1 - e.mSide]);
        edges->push_back(out);
    }
    tile.mPortalEdges = edges;
    tile.mPolyCache[avatar].reset();
}

U32 SKNavMesh::polySpace(const dtMeshTile* tile, U32 poly) const
{
    if (!tile || !tile->header)
    {
        return 0;
    }
    const auto it = mSlotSpaces.find(tile->header->userId);
    return it != mSlotSpaces.end() && poly < it->second->size() ? (*it->second)[poly] : 0;
}

void SKNavMesh::schedule()
{
    static LLCachedControl<S32> perFrame(gSavedSettings, "SKNavBuildsPerFrame", 4);
    static LLCachedControl<S32> inFlightMax(gSavedSettings, "SKNavBuildsInFlight", 0);

    if (!workerQueue())
    {
        return;
    }
    // In flight is the pool's width unless the setting overrides it: more than
    // that cannot run at once, and each build holds a heightfield's worth of
    // memory while it does.
    const S32 in_flight_cap = inFlightMax > 0 ? static_cast<S32>(inFlightMax) : static_cast<S32>(mWorkers->getWidth());

    auto workers = workerQueue();
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
    const bool annotating = SKNavAnnotator::instanceExists() && SKNavAnnotator::instance().armed();
    const auto due = [&](const Tile& tile)
    {
        const bool air_due = tile.mSignature == tile.mBuilt && tile.mAirBuilt != tile.mSignature && inAirRadius(tile);
        return !tile.mInFlight && (tile.mSignature != tile.mBuilt || air_due) && !(annotating && tile.mBuilt);
    };
    if (mBuildCandidates)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - launch builds");
        LLTimer timer;
        S32 launched = 0;
        while (mNextCandidate < mBuildCandidates->size())
        {
            if (launched >= perFrame || mInFlight >= in_flight_cap || timer.getElapsedTimeF32() >= 0.001f) return;
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
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    if (!mScheduleScanReady && !scanTiles(mScheduleCursor, 0.001f, [&](Mesh& mesh, const TileKey& key, Tile& tile)
        {
            if (!due(tile)) return;
            const LLVector3d centre = (tile.mMin + tile.mMax) * 0.5;
            const F64 distance = llmin(dist_vec_squared2D(centre, agent), dist_vec_squared2D(centre, camera));
            mScheduleScan->push_back({mesh.mId, mesh.mGeneration, key, distance});
        })) return;
    mScheduleScanReady = true;
    auto candidates = mScheduleScan;
    mStats.mQueued = static_cast<U32>(candidates->size());
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<std::vector<BuildCandidate>>()>>([candidates]()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - schedule worker");
        std::sort(candidates->begin(), candidates->end(), [](const auto& a, const auto& b) { return a.mDistance < b.mDistance; });
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

void SKNavMesh::update()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - update");
    SKWorldCensus& census = SKWorldCensus::instance();
    const bool want = wanted();
    if (want != mDemanding)
    {
        static LLCachedControl<F32> range(gSavedSettings, "SKNavRange", 512.f);
        if (want)
        {
            census.addDemand("mesh", range);
        }
        else
        {
            census.removeDemand("mesh");
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
    // On a new census, and once a second besides: the land patches change
    // without the census noticing, and a region mesh is built from both.
    static F32 last = -1.f;
    if (snap->mStamp != mCensusStamp || gFrameTimeSeconds - last > 1.f)
    {
        mCensusStamp = snap->mStamp;
        last = gFrameTimeSeconds;
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - reconcile");
        reconcile(snap);
    }

    applyLayouts();

    // Meshes nothing has claimed for a while are gone with their structure.
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - retire meshes");
        for (auto it = mMeshes.begin(); it != mMeshes.end();)
        {
            if (gFrameTimeSeconds - it->second->mLastSeen > 30.f)
            {
                std::shared_ptr<Mesh> retired(std::move(it->second));
                retireOn(workerQueue(), std::move(retired));
                it = mMeshes.erase(it);
                mRevision++;
            }
            else
            {
                ++it;
            }
        }
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - sample terrain");
        sampleTerrain();
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - evict air");
        evictAir();
    }
    schedule();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - sync feature links");
        syncFeatureLinks(snap);
    }
    if (mSkyJob.valid() || mSkyRevision != mRevision)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - sync sky map");
        syncSkyMap();
    }
    mRooms->update(*this);
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - stamp tiles");
        stampTiles();
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - stats scan");
        mStats.mMeshes = static_cast<U32>(mMeshes.size());
        mStats.mInFlight = mInFlight;
        mStats.mDiskBytes = mDiskCache ? mDiskCache->bytes() : 0;
        mStats.mDiskEntries = mDiskCache ? static_cast<U32>(mDiskCache->entries()) : 0;
        // Keep the last complete counts while a bounded scan runs; partial totals would look like lost tiles.
        if (scanTiles(mStatsCursor, 0.0005f, [&](Mesh&, const TileKey&, Tile& tile)
            {
                ++mScanningStats.mTiles;
                if (tile.mAir) { ++mScanningStats.mAirTiles; mScanningStats.mAirBytes += tile.mAir->bytes(); }
                if (tile.mRaster) { ++mScanningStats.mRasterTiles; mScanningStats.mRasterBytes += tile.mRaster->bytes(); }
                mScanningStats.mAwaitingStamp += tile.mStaticAt > 0.0 ? 1 : 0;
                if (!tile.mBuilt) return;
                ++mScanningStats.mTilesBuilt;
                mScanningStats.mFaces += tile.mFaces ? static_cast<U32>(tile.mFaces->size()) : 0;
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    mScanningStats.mSegments += tile.mSegments[cls] ? static_cast<U32>(tile.mSegments[cls]->size()) : 0;
                    mScanningStats.mLinks += tile.mLinks[cls] ? static_cast<U32>(tile.mLinks[cls]->size()) : 0;
                }
            }))
        {
            mStats.mTiles = mScanningStats.mTiles;
            mStats.mTilesBuilt = mScanningStats.mTilesBuilt;
            mStats.mAirTiles = mScanningStats.mAirTiles;
            mStats.mAirBytes = mScanningStats.mAirBytes;
        mStats.mRasterTiles = mScanningStats.mRasterTiles;
        mStats.mRasterBytes = mScanningStats.mRasterBytes;
            mStats.mAwaitingStamp = mScanningStats.mAwaitingStamp;
            mStats.mFaces = mScanningStats.mFaces;
            mStats.mSegments = mScanningStats.mSegments;
            mStats.mLinks = mScanningStats.mLinks;
            mScanningStats = {};
        }
    }

    static LLCachedControl<bool> log(gSavedSettings, "SKNavCensusLog", false);
    if (log && gFrameTimeSeconds - mLastLog > 5.f)
    {
        mLastLog = gFrameTimeSeconds;
        LL_INFOS("SKNav") << statsLine() << LL_ENDL;
    }
}

void SKNavMesh::placeReflectionProbes()
{
    if (!wanted())
    {
        mRoomProbes->clear();
        return;
    }
    mRoomProbes->update(mRooms->view(), *this);
}

void SKNavMesh::tileContentKeysIn(const LLUUID& mesh_id, const LLVector3d& min, const LLVector3d& max, std::vector<U64>& out) const
{
    out.clear();
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end())
    {
        return;
    }
    // Inset, so a space flush with a tile edge does not pick up its neighbour.
    const LLVector3d inset(0.01, 0.01, 0.0);
    for (const auto& t : it->second->mTiles)
    {
        if (overlapsXY(t.second.mMin, t.second.mMax, min + inset, max - inset))
        {
            out.push_back(t.second.mBuilt ? t.second.mContentKey : 0);
        }
    }
}

bool SKNavMesh::settlingIn(const LLVector3d& min, const LLVector3d& max) const
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
    return mRooms->settlingIn(min, max);
}

std::string SKNavMesh::statsLine() const
{
    const U32 looked = mStats.mDiskLoads + mStats.mDiskRebuilds;
    const std::string stamps = llformat(", stamps %u (%u from disk, %u re-rastered, %u rebinds), awaiting %u, last stamp %.1f ms, latency %.0f ms (worst %.0f), stamp overflow %u, poly overflow %u, unstamped %u",
                                        mStats.mStamps, mStats.mStampLoads, mStats.mStampReRasters, mStats.mRebinds, mStats.mAwaitingStamp, mStats.mLastStampMs,
                                        mStats.mLastStampLatencyMs, mStats.mWorstStampLatencyMs, mStats.mStampOverflow, mStats.mPolyOverflowTiles,
                                        mStats.mUnstampedPublishes);
    return llformat("navmesh: %u meshes, %u/%u tiles built (%u queued, %u in flight), %u faces, %u segments, %u links, %u builds, %u failures, last %.1f ms, worst %.1f ms, exclusion %.2f ms, raster %.1f + polys %.1f ms, held rasters %u tiles %.1f MB (%u dropped, %u re-rastered), local updates %u (%u whole, last %.1f ms), air %u tiles %.1f MB (%u evicted), sky %u cells %.1f MB, disk %.1f MB in %u, %u loads %u rebuilds %u corrupt, hit %.0f%%, load %.2f ms",
                    mStats.mMeshes, mStats.mTilesBuilt, mStats.mTiles, mStats.mQueued, mStats.mInFlight,
                    mStats.mFaces, mStats.mSegments, mStats.mLinks, mStats.mBuilds,
                    mStats.mFailures, mStats.mLastBuildMs, mStats.mWorstBuildMs, mStats.mExcludeMs,
                    mStats.mLastRasterMs, mStats.mLastPolyMs, mStats.mRasterTiles, mStats.mRasterBytes / 1048576.0, mStats.mRasterEvicted, mStats.mReRasters,
                    mStats.mLocalUpdates, mStats.mUpdateFallbacks, mStats.mLastUpdateMs, mStats.mAirTiles, mStats.mAirBytes / 1048576.0, mStats.mAirEvicted, static_cast<U32>(mSkyMap.cells()), mSkyMap.bytes() / 1048576.0,
                    mStats.mDiskBytes / 1048576.0, mStats.mDiskEntries, mStats.mDiskLoads, mStats.mDiskRebuilds, mStats.mDiskCorrupt,
                    looked ? 100.0 * mStats.mDiskLoads / looked : 0.0, mStats.mDiskLoadMs)
           + stamps + "; " + mRooms->statsLine() + "; " + mRoomProbes->statsLine() + "; " + gPipeline.mReflectionMapManager.clusterProbesStatsLine();
}

std::shared_ptr<const SKNavWorldView> SKNavMesh::view() const
{
    return mRooms->view();
}

U64 SKNavMesh::skyContributor(const LLUUID& mesh, const TileKey& key)
{
    return sknav::skyContributor(mesh, key);
}

void SKNavMesh::syncSkyMap()
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
            if (air) (*mSkyScan)[skyContributor(mesh.mId, key)] = {mesh.mOrigin, air};
        })) return;
    mSkyScanReady = true;
    auto seen = mSkyScan;
    auto result = std::make_shared<SkyResult>();
    result->mMap = mSkyMap; // SkyMap shares immutable blocks until the worker writes them.
    result->mRevision = mSkyScanRevision;
    auto previous = std::make_shared<std::map<U64, TileAirPtr>>(mSkyTiles);
    auto task = std::make_shared<std::packaged_task<std::shared_ptr<SkyResult>()>>([result, seen, previous]()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sknm - sync sky map worker");
        for (const auto& entry : *seen)
        {
            const auto old = previous->find(entry.first);
            const auto& air = entry.second.mAir;
            if (old == previous->end() || old->second != air)
                result->mMap.set(entry.first, entry.second.mOrigin + LLVector3d(air->mOrigin), *air);
            result->mTiles.emplace(entry.first, air);
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

SKNavMesh::Mesh* SKNavMesh::meshAt(const LLVector3d& pos, AgentClass cls, F32& z_out) const
{
    const S32 c = static_cast<S32>(cls);
    Mesh* best = nullptr;
    F32 bestDz = 1e9f;
    for (const auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        if (!mesh.mNav[c] || !mesh.mQuery[c])
        {
            continue;
        }
        const LLVector3d local64 = pos - mesh.mOrigin;
        const LLVector3 local(static_cast<F32>(local64.mdV[VX]), static_cast<F32>(local64.mdV[VY]), static_cast<F32>(local64.mdV[VZ]));
        F32 centre[3], nearest[3];
        toRecast(local, centre);
        const F32 extents[3] = { 0.5f, 2.f, 0.5f };
        const SpaceQueryFilter filter([this](const dtMeshTile* t, U32 p) { return polySpace(t, p); });
        dtPolyRef ref = 0;
        if (dtStatusSucceed(mesh.mQuery[c]->findNearestPoly(centre, extents, &filter, &ref, nearest)) && ref)
        {
            const F32 dz = std::fabs(nearest[1] - centre[1]);
            if (dz < bestDz)
            {
                bestDz = dz;
                best = &mesh;
                z_out = nearest[1] + static_cast<F32>(mesh.mOrigin.mdV[VZ]);
            }
        }
    }
    return best;
}

namespace
{
    /// The poly nearest a global position on a mesh, within the extents, mesh-local.
    dtPolyRef nearestPoly(dtNavMeshQuery& query, const LLVector3d& origin, const LLVector3d& pos, const dtQueryFilter& filter)
    {
        const LLVector3d l = pos - origin;
        F32 centre[3], nearest[3];
        toRecast(LLVector3(static_cast<F32>(l.mdV[VX]), static_cast<F32>(l.mdV[VY]), static_cast<F32>(l.mdV[VZ])), centre);
        const F32 extents[3] = { 0.5f, 2.f, 0.5f };
        dtPolyRef ref = 0;
        return dtStatusSucceed(query.findNearestPoly(centre, extents, &filter, &ref, nearest)) ? ref : 0;
    }
}

bool SKNavMesh::spaceAt(const LLVector3d& pos, LLUUID& mesh_out, U32& space_out) const
{
    F32 z = 0.f;
    Mesh* mesh = meshAt(pos, AgentClass::AVATAR, z);
    const S32 c = static_cast<S32>(AgentClass::AVATAR);
    if (!mesh || !mesh->mQuery[c])
    {
        return false;
    }
    const SpaceQueryFilter filter([this](const dtMeshTile* t, U32 p) { return polySpace(t, p); });
    const dtPolyRef ref = nearestPoly(*mesh->mQuery[c], mesh->mOrigin, pos, filter);
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    if (!ref || !dtStatusSucceed(mesh->mNav[c]->getTileAndPolyByRef(ref, &tile, &poly)))
    {
        return false;
    }
    space_out = polySpace(tile, static_cast<U32>(poly - tile->polys));
    mesh_out = mesh->mId;
    return space_out != 0;
}

void SKNavMesh::spacePolys(const LLUUID& mesh_id, U32 space, const OutlineFn& fn) const
{
    const auto it = mMeshes.find(mesh_id);
    const S32 c = static_cast<S32>(AgentClass::AVATAR);
    if (it == mMeshes.end() || !it->second->mNav[c])
    {
        return;
    }
    const Mesh& mesh = *it->second;
    const std::vector<std::vector<LLVector3>> polys = sknav::spacePolys(*mesh.mNav[c], [this](const dtMeshTile* t, U32 p) { return polySpace(t, p); }, space);
    std::vector<LLVector3d> outline;
    for (const std::vector<LLVector3>& poly : polys)
    {
        outline.clear();
        for (const LLVector3& v : poly)
        {
            outline.push_back(mesh.mOrigin + LLVector3d(v));
        }
        fn(outline);
    }
}

StoreyProductPtr SKNavMesh::storeys(const LLUUID& mesh_id) const
{
    const auto it = mMeshes.find(mesh_id);
    const S32 c = static_cast<S32>(AgentClass::AVATAR);
    if (it == mMeshes.end() || !it->second->mNav[c])
    {
        return nullptr;
    }
    const Mesh& mesh = *it->second;
    if (mesh.mStoreys && mesh.mStoreysAt == mRevision)
    {
        return mesh.mStoreys;
    }
    LL_PROFILE_ZONE_SCOPED;
    // Each stamped tile's polys' flights, by the slot its Detour header carries.
    std::unordered_map<U32, std::vector<const Flight*>> flights;
    for (const auto& entry : mesh.mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.mStampSlot || !tile.mFlights[c] || !tile.mPolySpace)
        {
            continue;
        }
        std::vector<const Flight*>& by_poly = flights[tile.mStampSlot];
        by_poly.assign(tile.mPolySpace->size(), nullptr);
        for (const Flight& f : *tile.mFlights[c])
        {
            for (const U16 p : f.mPolys)
            {
                if (p < by_poly.size())
                {
                    by_poly[p] = &f;
                }
            }
        }
    }
    auto out = std::make_shared<StoreyProduct>();
    static const RoomsProduct no_rooms;
    buildStoreys(*mesh.mNav[c], [this](const dtMeshTile* t, U32 p) { return polySpace(t, p); }, mesh.mStampRooms ? *mesh.mStampRooms : no_rooms, *out,
                 [&flights](const dtMeshTile* t, U32 p) -> const Flight* {
                     const auto f = t && t->header ? flights.find(t->header->userId) : flights.end();
                     return f != flights.end() && p < f->second.size() ? f->second[p] : nullptr;
                 });
    mesh.mStoreys = out;
    mesh.mStoreysAt = mRevision;
    return out;
}

bool SKNavMesh::storeyAt(const LLVector3d& pos, LLUUID& mesh_out, S32& storey_out) const
{
    F32 z = 0.f;
    Mesh* mesh = meshAt(pos, AgentClass::AVATAR, z);
    const S32 c = static_cast<S32>(AgentClass::AVATAR);
    if (!mesh || !mesh->mQuery[c])
    {
        return false;
    }
    const StoreyProductPtr st = storeys(mesh->mId);
    const SpaceQueryFilter filter([this](const dtMeshTile* t, U32 p) { return polySpace(t, p); });
    const dtPolyRef ref = nearestPoly(*mesh->mQuery[c], mesh->mOrigin, pos, filter);
    storey_out = st && ref ? st->storeyOfPoly(static_cast<U64>(ref)) : -1;
    mesh_out = mesh->mId;
    return storey_out >= 0;
}

bool SKNavMesh::floorUnder(const LLVector3d& pos, AgentClass cls, F32& z_out, LLUUID* mesh_out) const
{
    Mesh* mesh = meshAt(pos, cls, z_out);
    if (!mesh)
    {
        return false;
    }
    if (mesh_out)
    {
        *mesh_out = mesh->mId;
    }
    return true;
}

bool SKNavMesh::findPath(const LLVector3d& a, const LLVector3d& b, AgentClass cls, std::vector<LLVector3d>& out, bool& partial) const
{
    out.clear();
    partial = false;
    F32 z = 0.f;
    Mesh* mesh = meshAt(a, cls, z);
    if (!mesh)
    {
        return false;
    }
    const S32 c = static_cast<S32>(cls);
    dtNavMeshQuery* query = mesh->mQuery[c];
    auto toLocal = [&](const LLVector3d& p, F32* rc) {
        const LLVector3d l = p - mesh->mOrigin;
        toRecast(LLVector3(static_cast<F32>(l.mdV[VX]), static_cast<F32>(l.mdV[VY]), static_cast<F32>(l.mdV[VZ])), rc);
    };
    F32 sa[3], sb[3], na[3], nb[3];
    toLocal(a, sa);
    toLocal(b, sb);
    const F32 extents[3] = { 1.f, 3.f, 1.f };
    const SpaceQueryFilter filter([this](const dtMeshTile* t, U32 p) { return polySpace(t, p); });
    dtPolyRef ra = 0, rb = 0;
    query->findNearestPoly(sa, extents, &filter, &ra, na);
    query->findNearestPoly(sb, extents, &filter, &rb, nb);
    if (!ra || !rb)
    {
        return false;
    }
    dtPolyRef polys[512];
    int n = 0;
    const dtStatus st = query->findPath(ra, rb, na, nb, &filter, polys, &n, 512);
    if (!dtStatusSucceed(st) || n == 0)
    {
        return false;
    }
    partial = dtStatusDetail(st, DT_PARTIAL_RESULT);
    F32 straight[512 * 3];
    unsigned char flags[512];
    dtPolyRef refs[512];
    int ns = 0;
    query->findStraightPath(na, nb, polys, n, straight, flags, refs, &ns, 512);
    for (int i = 0; i < ns; ++i)
    {
        out.push_back(mesh->mOrigin + LLVector3d(fromRecast(&straight[i * 3])));
    }
    return !out.empty();
}

void SKNavMesh::forEachTileIn(const LLVector3d& min, const LLVector3d& max, const std::function<void(const Mesh&, const Tile&)>& fn) const
{
    LL_PROFILE_ZONE_SCOPED;
    U32 tiles_seen = 0;
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        visitTilesIn(mesh.mTiles, mesh.mOrigin, min, max, [&](const TileKey&, const Tile& tile)
        {
            ++tiles_seen;
            if (tile.mBuilt && overlapsXY(tile.mMin, tile.mMax, min, max))
            {
                fn(mesh, tile);
            }
        });
    }
    LL_PROFILE_ZONE_NUM(mMeshes.size());
    LL_PROFILE_ZONE_NUM(tiles_seen);
}

void SKNavMesh::polysIn(const LLVector3d& min, const LLVector3d& max, AgentClass cls, const PolyTileFn& fn) const
{
    // Keep the key for Detour lookup while querying only the requested rectangle.
    const S32 c = static_cast<S32>(cls);
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        visitTilesIn(mesh.mTiles, mesh.mOrigin, min, max, [&](const TileKey& key, const Tile& tile)
        {
            if (!tile.mBuilt || !overlapsXY(tile.mMin, tile.mMax, min, max) || !tile.mSegments[c])
            {
                return;
            }
            if (!tile.mPolyCache[c])
            {
                // Detour's x runs along Recast x, which is SL y.
                const dtMeshTile* dt_tile = mesh.mNav[c] ? mesh.mNav[c]->getTileAt(key.mY, key.mX, 0) : nullptr;
                if (!dt_tile || !dt_tile->header)
                {
                    return;   // nothing to hand out, so the cell is not counted as built
                }
                tile.mPolyCache[c] = extractTilePolys(*dt_tile);
            }
            PolyTile out;
            out.mMesh = mesh.mId;
            out.mTerrain = mesh.mTerrain;
            out.mRegion = mesh.mRegion;
            out.mOrigin = mesh.mOrigin;
            out.mKey = key;
            out.mContentKey = tile.mContentKey;
            out.mMin = tile.mMin;
            out.mMax = tile.mMax;
            out.mPolys = tile.mPolyCache[c];
            out.mSegments = tile.mSegments[c];
            out.mLinks = tile.mLinks[c];
            out.mFlights = tile.mFlights[c];
            if (c == static_cast<S32>(AgentClass::AVATAR))
            {
                out.mPolySpace = tile.mPolySpace;
                out.mPortalEdges = tile.mPortalEdges;
            }
            fn(out);
        });
    }
}

void SKNavMesh::wallFacesIn(const LLVector3d& min, const LLVector3d& max, const WallFaceFn& fn) const
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

void SKNavMesh::runoffIn(const LLVector3d& min, const LLVector3d& max, const SegmentFn& ledge_fn, const LinkFn& link_fn) const
{
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        if (tile.mLedges)
        {
            for (const WallSegment& seg : *tile.mLedges)
            {
                const LLVector3d a = mesh.mOrigin + LLVector3d(seg.mA), b = mesh.mOrigin + LLVector3d(seg.mB);
                if (overlapsXY(LLVector3d(llmin(a.mdV[VX], b.mdV[VX]), llmin(a.mdV[VY], b.mdV[VY]), 0.0),
                               LLVector3d(llmax(a.mdV[VX], b.mdV[VX]), llmax(a.mdV[VY], b.mdV[VY]), 0.0), min, max))
                {
                    ledge_fn(a, b, seg.mKind, seg.mValue);
                }
            }
        }
        if (tile.mRunoff)
        {
            for (const DropLink& link : *tile.mRunoff)
            {
                const LLVector3d from = mesh.mOrigin + LLVector3d(link.mFrom), to = mesh.mOrigin + LLVector3d(link.mTo);
                if (overlapsXY(from, from, min, max))
                {
                    link_fn(from, to, link.mHeight, link.mKind);
                }
            }
        }
    });
}

void SKNavMesh::segmentsIn(const LLVector3d& min, const LLVector3d& max, AgentClass cls, const SegmentFn& fn) const
{
    LL_PROFILE_ZONE_SCOPED;
    const S32 c = static_cast<S32>(cls);
    constexpr S32 ACROSS = SegmentCells::ACROSS;
    std::vector<U32> picked;
    U32 tiles_hit = 0, cells_built = 0, segments_read = 0;
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        if (!tile.mSegments[c] || tile.mSegments[c]->empty())
        {
            return;
        }
        ++tiles_hit;
        const F64 cell = (tile.mMax.mdV[VX] - tile.mMin.mdV[VX]) / ACROSS;
        if (cell <= 0.0)
        {
            return;
        }
        const auto cell_range = [&](F64 lo, F64 hi, F64 origin, S32& first, S32& last) {
            first = llclamp(static_cast<S32>(std::floor((lo - origin) / cell)), 0, ACROSS - 1);
            last = llclamp(static_cast<S32>(std::floor((hi - origin) / cell)), 0, ACROSS - 1);
        };
        // A hub tile holds segments by the hundred thousand; one linear pass an idle was
        // a quarter of a second, so they are read through cells built on the first query.
        if (!tile.mSegmentCells[c] || tile.mSegmentCells[c]->mSource != tile.mSegments[c])
        {
            LL_PROFILE_ZONE_NAMED("sknm - segment cells build");
            LL_PROFILE_ZONE_NUM(tile.mSegments[c]->size());
            ++cells_built;
            auto cells = std::make_shared<SegmentCells>();
            cells->mSource = tile.mSegments[c];
            const std::vector<WallSegment>& segments = *tile.mSegments[c];
            for (U32 i = 0; i < segments.size(); ++i)
            {
                const LLVector3d a = mesh.mOrigin + LLVector3d(segments[i].mA), b = mesh.mOrigin + LLVector3d(segments[i].mB);
                S32 x0, x1, y0, y1;
                cell_range(llmin(a.mdV[VX], b.mdV[VX]), llmax(a.mdV[VX], b.mdV[VX]), tile.mMin.mdV[VX], x0, x1);
                cell_range(llmin(a.mdV[VY], b.mdV[VY]), llmax(a.mdV[VY], b.mdV[VY]), tile.mMin.mdV[VY], y0, y1);
                for (S32 y = y0; y <= y1; ++y)
                {
                    for (S32 x = x0; x <= x1; ++x)
                    {
                        cells->mCells[y * ACROSS + x].push_back(i);
                    }
                }
            }
            tile.mSegmentCells[c] = std::move(cells);
        }
        const SegmentCells& cells = *tile.mSegmentCells[c];
        const std::vector<WallSegment>& segments = *cells.mSource;
        S32 x0, x1, y0, y1;
        cell_range(min.mdV[VX], max.mdV[VX], tile.mMin.mdV[VX], x0, x1);
        cell_range(min.mdV[VY], max.mdV[VY], tile.mMin.mdV[VY], y0, y1);
        picked.clear();
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                const std::vector<U32>& bucket = cells.mCells[y * ACROSS + x];
                picked.insert(picked.end(), bucket.begin(), bucket.end());
            }
        }
        // A segment crossing several of the query's cells is in each; once is enough.
        if (x1 > x0 || y1 > y0)
        {
            std::sort(picked.begin(), picked.end());
            picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        }
        segments_read += static_cast<U32>(picked.size());
        for (const U32 i : picked)
        {
            const WallSegment& seg = segments[i];
            const LLVector3d a = mesh.mOrigin + LLVector3d(seg.mA), b = mesh.mOrigin + LLVector3d(seg.mB);
            if (overlapsXY(LLVector3d(llmin(a.mdV[VX], b.mdV[VX]), llmin(a.mdV[VY], b.mdV[VY]), 0.0),
                           LLVector3d(llmax(a.mdV[VX], b.mdV[VX]), llmax(a.mdV[VY], b.mdV[VY]), 0.0), min, max))
            {
                fn(a, b, seg.mKind, seg.mValue);
            }
        }
    });
    LL_PROFILE_ZONE_NUM(tiles_hit);
    LL_PROFILE_ZONE_NUM(cells_built);
    LL_PROFILE_ZONE_NUM(segments_read);
}

void SKNavMesh::flightsIn(const LLVector3d& min, const LLVector3d& max, AgentClass cls, const FlightFn& fn) const
{
    const S32 c = static_cast<S32>(cls);
    struct Pick
    {
        LLVector3d mOrigin;
        const Flight* mFlight;
        bool mHome;
    };
    std::map<std::pair<LLUUID, U64>, Pick> picks;
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        if (!tile.mFlights[c])
        {
            return;
        }
        for (const Flight& f : *tile.mFlights[c])
        {
            LLVector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
            for (const LLVector2& p : f.mFootprint)
            {
                lo.set(llmin(lo.mV[0], p.mV[0]), llmin(lo.mV[1], p.mV[1]));
                hi.set(llmax(hi.mV[0], p.mV[0]), llmax(hi.mV[1], p.mV[1]));
            }
            if (!overlapsXY(mesh.mOrigin + LLVector3d(lo.mV[0], lo.mV[1], 0.0), mesh.mOrigin + LLVector3d(hi.mV[0], hi.mV[1], 0.0), min, max))
            {
                continue;
            }
            LLVector2 mid;
            for (const LLVector2& p : f.mFootprint)
            {
                mid += p / static_cast<F32>(f.mFootprint.size());
            }
            const LLVector3d centre = mesh.mOrigin + LLVector3d(mid.mV[0], mid.mV[1], 0.0);
            const bool home = centre.mdV[VX] >= tile.mMin.mdV[VX] && centre.mdV[VX] < tile.mMax.mdV[VX] &&
                              centre.mdV[VY] >= tile.mMin.mdV[VY] && centre.mdV[VY] < tile.mMax.mdV[VY];
            auto it = picks.find({ mesh.mId, f.mKey });
            if (it == picks.end())
            {
                picks.emplace(std::make_pair(mesh.mId, f.mKey), Pick { mesh.mOrigin, &f, home });
            }
            else if (home && !it->second.mHome)
            {
                it->second = { mesh.mOrigin, &f, home };
            }
        }
    });
    for (const auto& entry : picks)
    {
        fn(entry.second.mOrigin, *entry.second.mFlight);
    }
}

void SKNavMesh::linksIn(const LLVector3d& min, const LLVector3d& max, AgentClass cls, const LinkFn& fn) const
{
    const S32 c = static_cast<S32>(cls);
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        if (!tile.mLinks[c])
        {
            return;
        }
        for (const DropLink& link : *tile.mLinks[c])
        {
            const LLVector3d from = mesh.mOrigin + LLVector3d(link.mFrom), to = mesh.mOrigin + LLVector3d(link.mTo);
            if (overlapsXY(LLVector3d(llmin(from.mdV[VX], to.mdV[VX]), llmin(from.mdV[VY], to.mdV[VY]), 0.0),
                           LLVector3d(llmax(from.mdV[VX], to.mdV[VX]), llmax(from.mdV[VY], to.mdV[VY]), 0.0), min, max))
            {
                fn(from, to, link.mHeight, link.mKind);
            }
        }
    });
    for (const auto& region : mFeatureLinks)
    {
        for (const auto& entry : region.second)
        {
            const FeatureSpan& span = entry.second;
            if (!overlapsXY(span.mMin, span.mMax, min, max))
            {
                continue;
            }
            for (const auto& link : span.mLinks[c])
            {
                // Positive like a drop's: which way it goes is from and to.
                fn(link.first, link.second, static_cast<F32>(std::fabs(link.second.mdV[VZ] - link.first.mdV[VZ])), span.mKind);
            }
        }
    }
}

const SKNavMesh::FeatureSpan* SKNavMesh::featureSpan(const LLUUID& id) const
{
    for (const auto& region : mFeatureLinks)
    {
        const auto it = region.second.find(id);
        if (it != region.second.end())
        {
            return &it->second;
        }
    }
    return nullptr;
}

LLUUID SKNavMesh::featureId(const Feature& feature)
{
    // A ladder can span linksets and an elevator is its car: the smallest
    // root is stable between scans whichever order the parts came in.
    return feature.mRoots.empty() ? LLUUID::null : *std::min_element(feature.mRoots.begin(), feature.mRoots.end());
}

void SKNavMesh::doorsIn(const LLVector3d& min, const LLVector3d& max, const DoorFn& fn) const
{
    for (const auto& region : mFeatureDoors)
    {
        for (const DoorEntity& door : region.second)
        {
            if (overlapsXY(door.mMin, door.mMax, min, max) && door.mMin.mdV[VZ] <= max.mdV[VZ] && min.mdV[VZ] <= door.mMax.mdV[VZ])
            {
                fn(door);
            }
        }
    }
}

void SKNavMesh::syncFeatureLinks(const SKCensusSnapshotPtr& snap)
{
    if (snap && snap->mStamp != mFeatureStamp)
    {
        mFeatureStamp = snap->mStamp;
        std::map<U64, std::map<LLUUID, FeatureSpan>> next;
        for (const Feature& f : snap->mFeatures)
        {
            const bool ladder = f.mKind == FeatureKind::LADDER;
            if (!ladder && !(f.mKind == FeatureKind::ELEVATOR && f.mWorking))
            {
                continue;
            }
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal((f.mMin + f.mMax) * 0.5);
            if (!region)
            {
                continue;
            }
            FeatureSpan span;
            span.mKind = ladder ? LinkKind::LADDER : LinkKind::ELEVATOR;
            span.mMin = f.mMin;
            span.mMax = f.mMax;
            span.mMin.mdV[VZ] = f.mFoot;
            span.mMax.mdV[VZ] = f.mTop;
            // A span that has not changed, within a few centimetres of a car
            // settling, keeps the exits it has, unless a tile under it has
            // published since.
            const LLUUID id = featureId(f);
            const auto old_region = mFeatureLinks.find(region->getHandle());
            if (old_region != mFeatureLinks.end())
            {
                const auto old = old_region->second.find(id);
                if (old != old_region->second.end() && old->second.mKind == span.mKind
                    && sameSpan(old->second.mMin, span.mMin) && sameSpan(old->second.mMax, span.mMax))
                {
                    span = old->second;
                }
            }
            next[region->getHandle()][id] = std::move(span);
        }
        // A span the census no longer has, or whose region has gone, goes
        // with it; the overlay hears of any change.
        auto same_region = [](const auto& a, const auto& b) {
            return a.first == b.first
                && std::equal(a.second.begin(), a.second.end(), b.second.begin(), b.second.end(), [](const auto& x, const auto& y) {
                       return x.first == y.first && x.second.mKind == y.second.mKind && x.second.mMin == y.second.mMin
                           && x.second.mMax == y.second.mMax;
                   });
        };
        if (!std::equal(next.begin(), next.end(), mFeatureLinks.begin(), mFeatureLinks.end(), same_region))
        {
            mRevision++;
        }
        mFeatureLinks.swap(next);

        std::map<U64, std::vector<DoorEntity>> doors;
        for (const DoorEntity& door : snap->mDoors)
        {
            if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(door.mPlanePoint))
            {
                DoorEntity& d = doors[region->getHandle()].emplace_back(door);
                d.mRegion = region->getHandle();
            }
        }
        mFeatureDoors.swap(doors);
    }

    for (auto& region : mFeatureLinks)
    {
        for (auto& entry : region.second)
        {
            if (entry.second.mDirty)
            {
                findExits(entry.second);
                entry.second.mDirty = false;
                mRevision++;
            }
        }
    }
}

void SKNavMesh::findExits(FeatureSpan& span) const
{
    TileBuildInput probe;
    fillParams(probe);
    // Every floor of the class beside the span, from within a climb of its
    // foot to a little over its top, where a ladder's landing usually is:
    // probed round the span's plan an agent's radius out, every half metre.
    const F64 above = 1.25;
    const F64 step = 0.5;
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        span.mExits[cls].clear();
        span.mLinks[cls].clear();
        const F64 climb = probe.mParams[cls].mClimb;
        const F64 out = probe.mParams[cls].mRadius + 0.05;
        const F64 x0 = span.mMin.mdV[VX] - out, x1 = span.mMax.mdV[VX] + out;
        const F64 y0 = span.mMin.mdV[VY] - out, y1 = span.mMax.mdV[VY] + out;
        const F64 xm = (x0 + x1) * 0.5, ym = (y0 + y1) * 0.5;
        const LLVector3d ring[8] = { LLVector3d(x0, y0, 0), LLVector3d(xm, y0, 0), LLVector3d(x1, y0, 0), LLVector3d(x1, ym, 0),
                                     LLVector3d(x1, y1, 0), LLVector3d(xm, y1, 0), LLVector3d(x0, y1, 0), LLVector3d(x0, ym, 0) };
        std::vector<LLVector3d> found;
        for (F64 z = span.mMin.mdV[VZ]; z <= span.mMax.mdV[VZ] + above; z += step)
        {
            for (const LLVector3d& at : ring)
            {
                F32 floor = 0.f;
                const LLVector3d p(at.mdV[VX], at.mdV[VY], z);
                if (floorUnder(p, static_cast<AgentClass>(cls), floor) && std::fabs(floor - z) <= climb
                    && floor >= span.mMin.mdV[VZ] - climb && floor <= span.mMax.mdV[VZ] + above)
                {
                    found.emplace_back(at.mdV[VX], at.mdV[VY], floor);
                }
            }
        }
        std::stable_sort(found.begin(), found.end(), [](const LLVector3d& a, const LLVector3d& b) { return a.mdV[VZ] < b.mdV[VZ]; });
        for (const LLVector3d& p : found)
        {
            if (span.mExits[cls].empty() || p.mdV[VZ] - span.mExits[cls].back().mdV[VZ] > climb)
            {
                span.mExits[cls].push_back(p);
            }
        }
        // Consecutive exits both ways: a path may board or leave at any of them.
        for (size_t i = 1; i < span.mExits[cls].size(); ++i)
        {
            span.mLinks[cls].emplace_back(span.mExits[cls][i - 1], span.mExits[cls][i]);
            span.mLinks[cls].emplace_back(span.mExits[cls][i], span.mExits[cls][i - 1]);
        }
    }
}

bool SKNavMesh::coverAt(const LLVector3d& pos, const LLVector3& dir, AgentClass cls, F32 eye_height,
                        Cover& cover_out, F32& height_out, F32 reach) const
{
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    static LLCachedControl<F32> peek(gSavedSettings, "SKNavCoverPeek", 0.15f);
    LLVector3 heading(dir.mV[VX], dir.mV[VY], 0.f);
    if (heading.normVec() < 0.5f)
    {
        return false;
    }
    const LLVector3d box(reach, reach, reach);
    F32 best = -1.f;
    F32 bestDistance = reach;
    segmentsIn(pos - box, pos + box, cls, [&](const LLVector3d& a, const LLVector3d& b, EdgeKind kind, F32 value) {
        if (kind != EdgeKind::WALL)
        {
            return;
        }
        // The nearest point of the segment, and only if it lies ahead.
        const LLVector3d ab64 = b - a, ap64 = pos - a;
        const LLVector3 ab(static_cast<F32>(ab64.mdV[VX]), static_cast<F32>(ab64.mdV[VY]), 0.f);
        const LLVector3 ap(static_cast<F32>(ap64.mdV[VX]), static_cast<F32>(ap64.mdV[VY]), 0.f);
        const F32 len2 = ab.magVecSquared();
        const F32 t = len2 > 0.f ? llclamp(ab * ap / len2, 0.f, 1.f) : 0.f;
        LLVector3 to = ab * t - ap;
        to.mV[VZ] = 0.f;
        const F32 distance = to.normVec();
        if (distance > reach)
        {
            return;
        }
        // Ahead at all, and nearer than anything found so far.
        if (to * heading > 0.5f && distance < bestDistance)
        {
            bestDistance = distance;
            best = value;
        }
    });
    if (best < 0.f)
    {
        return false;
    }
    height_out = best;
    cover_out = coverClass(best, eye_height, climb, peek);
    return true;
}

std::string SKNavMesh::markUnderCamera()
{
    if (!gViewerWindow)
    {
        return "no window";
    }
    const LLRect& view = gViewerWindow->getWorldViewRectScaled();
    LLVector4a intersection;
    LLViewerObject* hit = gViewerWindow->cursorIntersect(view.getCenterX(), view.getCenterY(), 1024.f, nullptr, -1,
                                                         true, false, true, true, nullptr, &intersection);
    if (!hit)
    {
        mHasMark = false;
        return "nothing under the camera within 1024 m";
    }
    const LLVector3 agent_pos(intersection.getF32ptr());
    mMark = gAgent.getPosGlobalFromAgent(agent_pos);
    mHasMark = true;

    std::ostringstream out;
    out << "\n--- navmesh dump at " << mMark << " ---\n";
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(mMark))
    {
        out << "region " << region->getName() << " local " << LLVector3(mMark - region->getOriginGlobal()) << "\n";
    }

    // The avatar class's build parameters, read the way the build reads them:
    // whether erosion or the headroom rule ate the floor can only be judged
    // with the numbers it ran with.
    {
        static LLCachedControl<F32> cell(gSavedSettings, "SKNavCell", 0.125f);
        static LLCachedControl<F32> radius(gSavedSettings, "SKNavAvatarRadius", 0.3f);
        static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
        static LLCachedControl<F32> slope(gSavedSettings, "SKNavSlope", 74.f);
        F32 height = 2.f;
        if (isAgentAvatarValid())
        {
            height = llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f);
        }
        const F32 cell_m = llmax(static_cast<F32>(cell), 0.05f);
        height = std::ceil(height / cell_m) * cell_m;
        out << "agent height " << height << " radius " << radius << " climb " << climb << " slope " << slope
            << " cell " << cell << " (erode " << std::ceil(static_cast<F32>(radius) / cell_m) << " cells)\n";
    }

    LLViewerObject* root = hit;
    while (!root->isRoot() && root->getParent())
    {
        root = static_cast<LLViewerObject*>(root->getParent());
    }
    out << "object " << hit->getID() << " root " << root->getID() << " scale " << hit->getScale()
        << " physics type " << static_cast<S32>(hit->getPhysicsShapeType())
        << (hit->isPhysicsShapeUnknown() ? " (not yet told)" : "")
        << (root->flagPhantom() ? " phantom" : "") << (root->flagUsePhysics() ? " physical" : "")
        << (root->flagObjectPermanent() ? " permanent" : "") << (root->isLocalOnly() ? " local" : "") << "\n";

    // Where the part sits in its linkset (SL numbers the root 1) and the flags
    // the census gates on, so a door leaf inside a house can be named to its
    // owner without selecting it. Nothing here asks the sim for anything.
    out << "  link " << linkNumber(hit) << " of " << (root->numChildren() + 1) << " flags";
    if (hit->flagScripted()) out << " scripted";
    if (hit != root && root->flagScripted()) out << " root-scripted";
    if (hit->flagHandleTouch()) out << " touch";
    if (hit->flagTakesMoney()) out << " money";
    if (hit->flagVolumeDetect()) out << " volume-detect";
    if (hit->flagCharacter()) out << " character";
    if (hit->flagTemporaryOnRez()) out << " temp-on-rez";
    if (hit->flagInventoryEmpty()) out << " inventory-empty";
    if (hit->flagAllowInventoryAdd()) out << " inventory-drop";
    if (hit->flagIncludeInSearch()) out << " search";
    out << "\n";

    // Any name already in hand: the property cache holds what a hover, a
    // selection or the Scene Explorer has asked for, and a script that set a
    // NameValue leaves it on the object. Both are free; neither is requested.
    if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(hit->getID()))
    {
        out << "  props name \"" << props->mName << "\" desc \"" << props->mDescription << "\" creator "
            << props->mCreatorId << (props->mHasFullData ? " (full)" : " (family only)") << "\n";
    }
    if (LLNameValue* name_value = hit->getNVPair("Name"))
    {
        if (char* text = name_value->getString())
        {
            out << "  name value \"" << text << "\"\n";
        }
    }
    if (!hit->mHudText.empty())
    {
        out << "  hover text \"" << hit->mHudText << "\"\n";
    }

    // What the linkset reply says of the root, and what this session has seen
    // the linkset do: the inputs the feature rules read.
    SKWorldCensus& census = SKWorldCensus::instance();
    if (const SKWorldCensus::LinksetFacts* facts = census.linksetFactsOf(root->getID()))
    {
        out << "  linkset name \"" << facts->mName << "\" desc \"" << facts->mDescription << "\" scripted "
            << (facts->mScriptedKnown ? (facts->mScripted ? "yes" : "no") : "unknown") << "\n";
    }
    if (const SKWorldCensus::RootHistory* history = census.historyOf(root->getID()))
    {
        out << "  session moved " << history->mEverMoved << " travelled " << history->mTravel << " m crewed " << history->mEverCrewed << " vehicle " << history->mVehicle << " working car "
            << history->mWorkingElevator << " kind " << featureName(history->mKind) << " crew " << history->mCrew.size();
        for (const LLUUID& crew : history->mCrew)
        {
            out << " " << crew;
        }
        out << "\n";
    }

    // Whether each face can be seen through: a pane of glass and an invisible
    // hitbox are both solid to the sim and told apart from a wall only here.
    for (U8 f = 0; f < hit->getNumTEs(); ++f)
    {
        if (const LLTextureEntry* te = hit->getTE(f))
        {
            out << "  face " << static_cast<S32>(f) << " alpha " << te->getColor().mV[VALPHA] << " pbr alpha "
                << facePbrAlpha(te) << " mode " << faceAlphaMode(te) << " texture " << te->getID()
                << (te->getFullbright() ? " fullbright" : "") << "\n";
        }
    }

    // Everything that moves: a door swinging by steps or KFM, a gate turning
    // on its axis, and the sim-versus-rendered rotation the two disagree on.
    out << "  motion velocity " << hit->getVelocity() << " acceleration " << hit->getAcceleration() << " angular "
        << hit->getAngularVelocity() << " omega time " << hit->getRotTime() << " s\n";
    out << "  rotation sim " << hit->getRotationSim() << " render " << hit->getRotationRegion() << "\n";

    // What the shared resolver says this object's shape is, and which of the
    // three sources the census can take it from: a decomposition, the physics
    // mesh the sim actually collides with, or the base hull that covers the
    // wait. An object whose census record reads HULL but has no hulls is
    // being built from the base hull.
    if (LLVOVolume* volume = dynamic_cast<LLVOVolume*>(hit))
    {
        if (volume->getVolume())
        {
            SKPhysicsShape shape;
            const bool known = skPhysicsShapeClassify(volume, shape);
            const LLUUID sculpt = volume->getVolume()->getParams().getSculptID();
            LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(sculpt);
            out << "  shape spec " << (known ? static_cast<S32>(shape.mSpec.getType()) : -1)
                << " mesh " << sculpt << " decomp " << (decomp ? "yes" : "no");
            if (decomp)
            {
                S32 hull_points = 0;
                for (const std::vector<LLVector3>& hull : decomp->mHull)
                {
                    hull_points += static_cast<S32>(hull.size());
                }
                S32 hull_tris = 0;
                for (const LLModel::PhysicsMesh& hull : decomp->mMesh)
                {
                    hull_tris += static_cast<S32>(hull.mPositions.size() / 3);
                }
                out << " hulls " << decomp->mHull.size() << " (" << hull_points << " points, " << hull_tris << " tris built)"
                    << " physics mesh tris " << decomp->mPhysicsShapeMesh.mPositions.size() / 3
                    << " base hull tris " << decomp->mBaseHullMesh.mPositions.size() / 3;
            }
            out << "\n";
        }
    }

    // The part's own record, the one that decides what the navmesh has under
    // the camera, with the box it covers: it is often past the nearby list's
    // cap, and its extent is what says whether a ledge is too narrow to erode.
    if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        for (const RecordPtr& r : snap->mRecords)
        {
            if (r->mPart != hit->getID())
            {
                continue;
            }
            out << "  part record provenance " << static_cast<S32>(r->mProvenance) << " shape "
                << static_cast<S32>(r->mShape) << " role " << static_cast<S32>(r->mRole) << " nav "
                << static_cast<S32>(r->mNavRole) << " x " << r->mMin.mdV[VX] << ".." << r->mMax.mdV[VX] << " y "
                << r->mMin.mdV[VY] << ".." << r->mMax.mdV[VY] << " z " << r->mMin.mdV[VZ] << ".." << r->mMax.mdV[VZ]
                << " tris " << (r->mTriangles ? r->mTriangles->size() / 3 : 0) << "\n";
            break;
        }
    }

    // Any feature the marked part belongs to, or whose span the mark is in.
    if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        for (const Feature& f : snap->mFeatures)
        {
            const bool mine = std::find(f.mParts.begin(), f.mParts.end(), hit->getID()) != f.mParts.end()
                || std::find(f.mRoots.begin(), f.mRoots.end(), root->getID()) != f.mRoots.end();
            const bool inside = overlapsXY(f.mMin, f.mMax, mMark, mMark) && mMark.mdV[VZ] >= f.mMin.mdV[VZ] - 1.0
                && mMark.mdV[VZ] <= f.mMax.mdV[VZ] + 1.0;
            if (!mine && !inside)
            {
                continue;
            }
            out << "  feature " << featureName(f.mKind) << " confidence " << f.mConfidence << " (" << f.mEvidence << ") parts "
                << f.mParts.size() << " roots " << f.mRoots.size() << " x " << f.mMin.mdV[VX] << ".." << f.mMax.mdV[VX] << " y "
                << f.mMin.mdV[VY] << ".." << f.mMax.mdV[VY] << " z " << f.mMin.mdV[VZ] << ".." << f.mMax.mdV[VZ];
            if (f.mKind == FeatureKind::LADDER || f.mKind == FeatureKind::ELEVATOR)
            {
                out << " span " << f.mFoot << ".." << f.mTop;
            }
            if (f.mKind == FeatureKind::ELEVATOR)
            {
                out << (f.mWorking ? " working" : " no car") << " doors " << f.mDoors.size();
            }
            if (f.mKind == FeatureKind::VEHICLE)
            {
                out << (f.mCombat ? " combat" : "") << (f.mAir ? " air" : "");
            }
            out << (mine ? " <-- the marked linkset" : "") << "\n";
            if (const FeatureSpan* span = featureSpan(featureId(f)))
            {
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    out << "    class " << cls << " exits " << span->mExits[cls].size() << ":";
                    for (const LLVector3d& exit : span->mExits[cls])
                    {
                        out << " " << exit.mdV[VZ];
                    }
                    out << " links " << span->mLinks[cls].size() << "\n";
                }
            }
        }
    }

    // What the census made of everything around the mark.
    const LLVector3d reach(4.0, 4.0, 4.0);
    if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        // The physics shape type each end of the record carries. The sim only
        // tells the linkset root, so a child that reads Prim while its root
        // reads none is a part we should not have made a shape of.
        auto shape_type = [](const RecordPtr& r, bool root) -> std::string
        {
            LLViewerObject* object = gObjectList.findObject(root ? r->mRoot : r->mPart);
            if (!object)
            {
                return "gone";
            }
            static const char* names[] = { "prim", "none", "convex" };
            const U8 type = object->getPhysicsShapeType();
            return std::string(type < 3 ? names[type] : "other") + (object->isPhysicsShapeUnknown() ? "?" : "");
        };
        S32 shown = 0;
        for (const RecordPtr& r : snap->mRecords)
        {
            if (!overlapsXY(r->mMin, r->mMax, mMark - reach, mMark + reach) || mMark.mdV[VZ] + 4.0 < r->mMin.mdV[VZ] ||
                mMark.mdV[VZ] - 4.0 > r->mMax.mdV[VZ])
            {
                continue;
            }
            if (++shown > 24)
            {
                out << "  ... and more\n";
                break;
            }
            const bool covers = overlapsXY(r->mMin, r->mMax, mMark, mMark);
            out << "  record root " << r->mRoot << " provenance " << static_cast<S32>(r->mProvenance) << " shape "
                << static_cast<S32>(r->mShape) << " role " << static_cast<S32>(r->mRole) << " nav "
                << static_cast<S32>(r->mNavRole) << (r->mDynamic ? " moving" : "") << " x " << r->mMin.mdV[VX] << ".."
                << r->mMax.mdV[VX] << " y " << r->mMin.mdV[VY] << ".." << r->mMax.mdV[VY] << " z " << r->mMin.mdV[VZ]
                << ".." << r->mMax.mdV[VZ] << " tris " << (r->mTriangles ? r->mTriangles->size() / 3 : 0) << " part "
                << shape_type(r, false) << " root " << shape_type(r, true);
            if (r->mFeature != FeatureKind::NONE)
            {
                out << " feature " << static_cast<S32>(r->mFeature) << " exclude " << static_cast<S32>(r->mExclude);
            }
            if (covers)
            {
                out << " <-- covers the mark";
                if (r->mMin.mdV[VZ] > mMark.mdV[VZ])
                {
                    out << ", bottom " << (r->mMin.mdV[VZ] - mMark.mdV[VZ]) << " m above it";
                }
            }
            out << "\n";
        }
        if (!shown)
        {
            out << "  no census record within 4 m\n";
        }
        for (const ClusterPtr& c : snap->mClusters.mClusters)
        {
            if (overlapsXY(c->mMin, c->mMax, mMark - reach, mMark + reach))
            {
                out << "  cluster " << c->mId << " records " << c->mRecords.size() << " tiles " << c->mTiles.size()
                    << " z " << c->mMin.mdV[VZ] << ".." << c->mMax.mdV[VZ] << " grounded " << grounded(*c) << "\n";
            }
        }

        // The record triangles over the mark, with how flat each is: a floor
        // goes missing when a surface hangs over it by less than the agent's
        // height, and which surface does is the whole question. Lowest first,
        // and one line per distinct surface: a box standing there would
        // otherwise fill the list with its own twelve triangles.
        struct OverTri
        {
            F32 mMin;
            F32 mMax;
            F32 mNormalZ;
            F32 mWidth;
            F32 mDepth;
            S32 mProvenance;
            LLUUID mRoot;
        };
        std::vector<OverTri> over;
        const F64 slack = 0.15;
        for (const RecordPtr& r : snap->mRecords)
        {
            if (!r->mTriangles)
            {
                continue;
            }
            const LLVector3d local = mMark - r->mOrigin;
            for (size_t t = 0; t + 2 < r->mTriangles->size(); t += 3)
            {
                const LLVector3& a = (*r->mTriangles)[t];
                const LLVector3& b = (*r->mTriangles)[t + 1];
                const LLVector3& c = (*r->mTriangles)[t + 2];
                const F32 xmin = llmin(a.mV[VX], llmin(b.mV[VX], c.mV[VX]));
                const F32 xmax = llmax(a.mV[VX], llmax(b.mV[VX], c.mV[VX]));
                const F32 ymin = llmin(a.mV[VY], llmin(b.mV[VY], c.mV[VY]));
                const F32 ymax = llmax(a.mV[VY], llmax(b.mV[VY], c.mV[VY]));
                if (local.mdV[VX] < xmin - slack || local.mdV[VX] > xmax + slack || local.mdV[VY] < ymin - slack ||
                    local.mdV[VY] > ymax + slack)
                {
                    continue;
                }
                const F32 zmin = llmin(a.mV[VZ], llmin(b.mV[VZ], c.mV[VZ]));
                const F32 zmax = llmax(a.mV[VZ], llmax(b.mV[VZ], c.mV[VZ]));
                if (zmax < mMark.mdV[VZ] - 2.0)
                {
                    continue;
                }
                LLVector3 n = (b - a) % (c - a);
                F32 nz = 0.f;
                if (n.normVec() > 0.f)
                {
                    nz = n.mV[VZ];
                }
                bool dup = false;
                const F32 width = xmax - xmin, depth = ymax - ymin;
                for (const OverTri& o : over)
                {
                    if (o.mProvenance == static_cast<S32>(r->mProvenance) && o.mRoot == r->mRoot &&
                        std::fabs(o.mMin - zmin) < 0.01f && std::fabs(o.mMax - zmax) < 0.01f && std::fabs(o.mNormalZ - nz) < 0.02f &&
                        std::fabs(o.mWidth - width) < 0.05f && std::fabs(o.mDepth - depth) < 0.05f)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                {
                    over.push_back({ zmin, zmax, nz, width, depth, static_cast<S32>(r->mProvenance), r->mRoot });
                }
            }
        }
        std::sort(over.begin(), over.end(), [](const OverTri& a, const OverTri& b) { return a.mMin < b.mMin; });
        S32 tris_shown = 0;
        for (const OverTri& o : over)
        {
            if (++tris_shown > 16)
            {
                out << "  ... and more triangles over the mark\n";
                break;
            }
            out << "  tri over the mark z " << o.mMin << ".." << o.mMax << " normal z " << o.mNormalZ << " footprint "
                << o.mWidth << " x " << o.mDepth << " m provenance " << o.mProvenance << " root " << o.mRoot << "\n";
        }

        // The vertical profile of what is over the mark: a ladder, a stair of
        // thin ledges or a sliced-up wall reads as a stack of small horizontal
        // surfaces on a vertical face. Printing the count, the spacing and the
        // footprints makes a marked sample in the wild a number to compare
        // with the next one instead of a squint at the list above.
        std::vector<F32> levels;
        S32 vertical = 0;
        F32 widest = 0.f;
        F32 deepest = 0.f;
        for (const OverTri& o : over)
        {
            if (o.mMax < mMark.mdV[VZ] - 0.5)
            {
                continue;
            }
            if (std::fabs(o.mNormalZ) > 0.7f)
            {
                levels.push_back(o.mMin);
                widest = llmax(widest, o.mWidth);
                deepest = llmax(deepest, o.mDepth);
            }
            else if (std::fabs(o.mNormalZ) < 0.3f)
            {
                ++vertical;
            }
        }
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end(),
                                 [](F32 a, F32 b) { return std::fabs(a - b) < 0.05f; }),
                     levels.end());
        if (levels.size() >= 2)
        {
            std::vector<F32> gaps;
            for (size_t i = 1; i < levels.size(); ++i)
            {
                gaps.push_back(levels[i] - levels[i - 1]);
            }
            std::sort(gaps.begin(), gaps.end());
            out << "  step profile " << levels.size() << " horizontal surfaces z " << levels.front() << ".."
                << levels.back() << " spacing " << gaps.front() << "/" << gaps[gaps.size() / 2] << "/" << gaps.back()
                << " m (min/median/max) footprints up to " << widest << " x " << deepest << " m, " << vertical
                << " vertical\n";
        }
    }

    // The door the mark is on, or within half a metre of.
    {
        const LLVector3d near_mark(0.5, 0.5, 0.5);
        S32 doors = 0;
        doorsIn(mMark - near_mark, mMark + near_mark, [&](const DoorEntity& d) {
            const bool leaf = std::find(d.mLeaves.begin(), d.mLeaves.end(), hit->getID()) != d.mLeaves.end();
            out << "door " << d.mId << (leaf ? " (marked leaf)" : "") << " width " << d.mWidth << " height " << d.mHeight
                << " sill " << d.mSill << " normal " << d.mNormal << (d.mPlaneKnown ? "" : " (plane unknown)") << (d.mDouble ? " double" : " single") << " leaves "
                << d.mLeaves.size() << " confidence " << d.mConfidence << (d.mDamageable ? " damageable" : "") << " name \""
                << d.mName << "\"\n";
            ++doors;
        });
        if (!doors)
        {
            out << "no door entity at the mark\n";
        }
    }

    // And what the navmesh built from it.
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        const S32 ox = tileIndex(mesh.mOrigin.mdV[VX]), oy = tileIndex(mesh.mOrigin.mdV[VY]);
        const TileKey key{ tileIndex(mMark.mdV[VX]) - ox, tileIndex(mMark.mdV[VY]) - oy };
        const auto it = mesh.mTiles.find(key);
        if (it == mesh.mTiles.end())
        {
            continue;
        }
        const Tile& tile = it->second;
        out << (mesh.mTerrain ? "region mesh " : "cluster mesh ") << mesh.mId << " tile " << key.mX << "," << key.mY
            << " records " << tile.mRecords.size() << " z " << tile.mMin.mdV[VZ] << ".." << tile.mMax.mdV[VZ]
            << " polys " << tile.mPolys[0] << " built " << (tile.mBuilt == tile.mSignature)
            << " in flight " << tile.mInFlight << " " << tile.mMillis << " ms\n";
        // What the tile itself was built from, as far as the mark reaches. The
        // census's list above reads the live snapshot; a surface in the mesh
        // with no census record under it means the tile holds a record the
        // census no longer does, and only this list can say which.
        S32 tile_shown = 0;
        for (const RecordPtr& r : tile.mRecords)
        {
            if (!overlapsXY(r->mMin, r->mMax, mMark - reach, mMark + reach) || mMark.mdV[VZ] + 4.0 < r->mMin.mdV[VZ] ||
                mMark.mdV[VZ] - 4.0 > r->mMax.mdV[VZ])
            {
                continue;
            }
            if (++tile_shown > 24)
            {
                out << "    ... and more tile records\n";
                break;
            }
            out << "    tile record root " << r->mRoot << " provenance " << static_cast<S32>(r->mProvenance) << " shape "
                << static_cast<S32>(r->mShape) << " role " << static_cast<S32>(r->mRole) << " nav "
                << static_cast<S32>(r->mNavRole) << (r->mDynamic ? " moving" : "") << " x " << r->mMin.mdV[VX] << ".."
                << r->mMax.mdV[VX] << " y " << r->mMin.mdV[VY] << ".." << r->mMax.mdV[VY] << " z " << r->mMin.mdV[VZ]
                << ".." << r->mMax.mdV[VZ] << " tris " << (r->mTriangles ? r->mTriangles->size() / 3 : 0);
            if (r->mFeature != FeatureKind::NONE)
            {
                out << " feature " << static_cast<S32>(r->mFeature) << " exclude " << static_cast<S32>(r->mExclude);
            }
            out << "\n";
        }
        // Whether the tile was built with the marked part, and as what: a
        // doorway that does not connect is a leaf the build did not leave out.
        const auto marked = std::find_if(tile.mRecords.begin(), tile.mRecords.end(),
                                         [&](const RecordPtr& r) { return r->mPart == hit->getID(); });
        if (marked == tile.mRecords.end())
        {
            out << "    marked part not in this tile\n";
        }
        else
        {
            out << "    marked part in this tile, feature " << static_cast<S32>((*marked)->mFeature) << " exclude "
                << static_cast<S32>((*marked)->mExclude) << "\n";
        }
        if (tile.mFaces)
        {
            S32 shown = 0;
            for (const WallFace& f : *tile.mFaces)
            {
                const LLVector3d fmin = mesh.mOrigin + LLVector3d(f.mMin), fmax = mesh.mOrigin + LLVector3d(f.mMax);
                if (!overlapsXY(fmin, fmax, mMark - reach, mMark + reach))
                {
                    continue;
                }
                if (++shown > 8)
                {
                    out << "    ... and more faces\n";
                    break;
                }
                out << "    face side " << static_cast<S32>(f.mSide) << " area " << static_cast<S32>(f.mArea) << " z "
                    << fmin.mdV[VZ] << ".." << fmax.mdV[VZ] << " (" << (fmax.mdV[VZ] - fmin.mdV[VZ]) << " m tall)\n";
            }
        }
    }

    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        F32 z = 0.f;
        LLUUID which;
        if (floorUnder(mMark, static_cast<AgentClass>(cls), z, &which))
        {
            out << "floor for class " << cls << " at " << z << " in " << which << "\n";
        }
        else
        {
            out << "no floor for class " << cls << "\n";
        }
    }

    // The polygon each class actually has here, with its corners: a shape that
    // reads as degenerate on screen is one to measure rather than squint at.
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        for (const auto& entry : mMeshes)
        {
            const Mesh& mesh = *entry.second;
            if (!mesh.mNav[cls] || !mesh.mQuery[cls])
            {
                continue;
            }
            const LLVector3d local64 = mMark - mesh.mOrigin;
            const LLVector3 local(static_cast<F32>(local64.mdV[VX]), static_cast<F32>(local64.mdV[VY]),
                                  static_cast<F32>(local64.mdV[VZ]));
            F32 centre[3], nearest[3];
            toRecast(local, centre);
            const F32 extents[3] = { 2.f, 4.f, 2.f };
            const SpaceQueryFilter filter([this](const dtMeshTile* t, U32 p) { return polySpace(t, p); });
            dtPolyRef ref = 0;
            if (!dtStatusSucceed(mesh.mQuery[cls]->findNearestPoly(centre, extents, &filter, &ref, nearest)) || !ref)
            {
                continue;
            }
            const dtMeshTile* tile = nullptr;
            const dtPoly* poly = nullptr;
            if (!dtStatusSucceed(mesh.mNav[cls]->getTileAndPolyByRef(ref, &tile, &poly)) || !poly)
            {
                continue;
            }
            out << "class " << cls << " polygon in " << (mesh.mTerrain ? "region " : "cluster ") << mesh.mId
                << ": area " << static_cast<S32>(poly->getArea()) << " flags " << poly->flags << " verts "
                << static_cast<S32>(poly->vertCount);
            F64 area2 = 0.0;
            for (S32 v = 0; v < static_cast<S32>(poly->vertCount); ++v)
            {
                const LLVector3 p = fromRecast(&tile->verts[poly->verts[v] * 3]);
                const LLVector3 q = fromRecast(&tile->verts[poly->verts[(v + 1) % poly->vertCount] * 3]);
                area2 += static_cast<F64>(p.mV[VX]) * q.mV[VY] - static_cast<F64>(q.mV[VX]) * p.mV[VY];
                out << (v ? ", " : " [") << p;
            }
            out << "] area " << std::fabs(area2) * 0.5 << " m2, detail tris "
                << static_cast<S32>(tile->detailMeshes[poly - tile->polys].triCount) << "\n";

            // The nearest polygon of each area at the mark, so two surfaces
            // in one mesh - a platform and the terrain it meets - both show,
            // and the step between them with them.
            for (U8 area = AREA_GROUND; area <= AREA_SUBMERGED; ++area)
            {
                SpaceQueryFilter area_filter([this](const dtMeshTile* t, U32 p) { return polySpace(t, p); });
                area_filter.setIncludeFlags(static_cast<unsigned short>(1u << area));
                dtPolyRef area_ref = 0;
                F32 area_pt[3];
                if (dtStatusSucceed(mesh.mQuery[cls]->findNearestPoly(centre, extents, &area_filter, &area_ref, area_pt)) && area_ref)
                {
                    out << "    nearest area " << static_cast<S32>(area) << " at z " << area_pt[1] << "\n";
                }
            }
        }
    }

    LL_INFOS("SKNavDump") << out.str() << LL_ENDL;
    // The console keeps the two UUIDs and any name already in hand: that is
    // what makes a marked door or ladder findable in another tool without
    // opening the log.
    std::string label = hit->getID().asString();
    if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(hit->getID()))
    {
        if (!props->mName.empty())
        {
            label += " \"" + props->mName + "\"";
        }
    }
    return llformat("marked %.1f, %.1f, %.1f - object %s root %s - the rest is in the log under SKNavDump",
                    mMark.mdV[VX], mMark.mdV[VY], mMark.mdV[VZ], label.c_str(), root->getID().asString().c_str());
}

void SKNavMesh::writeTile(std::ostream& file, const Mesh& mesh, const TileKey& key, const Tile& tile, ExportCounts& counts) const
{
    TileExport out;
    out.mKey = key;
    out.mMin = tile.mMin;
    out.mMax = tile.mMax;
    out.mFaces = tile.mFaces.get();
    out.mAir = tile.mAir;
    if (tile.mAir)
    {
        out.mEnclosure = mRooms->enclosureTile(mesh.mId, key, tile.mAir, out.mEnclosureGeneration);
    }
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        out.mNav[cls] = mesh.mNav[cls];
        out.mSegments[cls] = tile.mSegments[cls].get();
        out.mLinks[cls] = tile.mLinks[cls].get();
        out.mFlights[cls] = tile.mFlights[cls].get();
    }
    out.mStamp = tile.mStampGeom.get();
    out.mBinding = tile.mBinding.get();
    writeExportTile(file, mesh.mOrigin, out, counts);
}

void SKNavMesh::writeFeatures(std::ostream& file, const LLVector3d* min, const LLVector3d* max, ExportCounts& counts) const
{
    // What the census recognised, with each span's exits and links per class.
    file << ",\"features\":[";
    if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        for (const Feature& f : snap->mFeatures)
        {
            if (min && max && !overlaps3D(f.mMin, f.mMax, *min, *max))
            {
                continue;
            }
            if (counts.mFeatures++)
            {
                file << ',';
            }
            const FeatureSpan* span = featureSpan(featureId(f));
            writeExportFeature(file, f, span ? span->mKind : LinkKind::LADDER, span ? span->mExits : nullptr, span ? span->mLinks : nullptr);
        }
    }
    file << ']';
}

void SKNavMesh::writeDoors(std::ostream& file, const LLVector3d* min, const LLVector3d* max, ExportCounts& counts) const
{
    // The door table as the rooms pass would read it. Names are anyone's
    // text, so they are escaped.
    file << ",\"doors\":[";
    for (const auto& region : mFeatureDoors)
    {
        for (const DoorEntity& d : region.second)
        {
            if (min && max && !overlaps3D(d.mMin, d.mMax, *min, *max))
            {
                continue;
            }
            if (counts.mDoors++)
            {
                file << ',';
            }
            writeExportDoor(file, d);
        }
    }
    file << ']';
}

namespace
{
    /// Every root the records name, with what the viewer already knows of it: the
    /// region's linkset reply first, else the object properties cache. `named` counts those with a name.
    std::vector<CensusRoot> rootTable(const std::vector<RecordPtr>& records, size_t& named)
    {
        std::vector<CensusRoot> out;
        std::set<LLUUID> seen;
        named = 0;
        const SKWorldCensus* census = SKWorldCensus::instanceExists() ? SKWorldCensus::getInstance() : nullptr;
        for (const RecordPtr& record : records)
        {
            if (!seen.insert(record->mRoot).second)
            {
                continue;
            }
            CensusRoot root;
            root.mId = record->mRoot;
            if (const SKWorldCensus::LinksetFacts* facts = census ? census->linksetFactsOf(root.mId) : nullptr)
            {
                root.mScriptedKnown = facts->mScriptedKnown;
                root.mScripted = facts->mScripted;
                if (!facts->mName.empty())
                {
                    root.mName = facts->mName;
                    root.mDescription = capDescription(facts->mDescription);
                    root.mSource = NameSource::LINKSET;
                }
            }
            if (root.mSource == NameSource::NONE)
            {
                if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(root.mId))
                {
                    if (!props->mName.empty())
                    {
                        root.mName = props->mName;
                        root.mDescription = capDescription(props->mDescription);
                        root.mSource = NameSource::PROPERTIES;
                    }
                }
            }
            named += root.mSource != NameSource::NONE;
            out.push_back(std::move(root));
        }
        return out;
    }
}

std::string SKNavMesh::exportAll() const
{
    // Written straight to the file rather than through an LLSD tree: a built
    // region is hundreds of thousands of numbers, and holding them as LLSD
    // first would stall the main thread for seconds before a byte landed.
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sknavmesh-export.json");
    llofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return llformat("could not write %s", path.c_str());
    }
    file << std::setprecision(10);

    F32 height = 2.f;
    if (isAgentAvatarValid())
    {
        height = llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f);
    }
    static LLCachedControl<F32> cell(gSavedSettings, "SKNavCell", 0.125f);
    static LLCachedControl<F32> radius(gSavedSettings, "SKNavAvatarRadius", 0.3f);
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    static LLCachedControl<F32> slope(gSavedSettings, "SKNavSlope", 74.f);

    U32 meshes_written = 0;
    ExportCounts counts;

    writeExportHeader(file, LLDate::now().asString(), gAgent.getPositionGlobal(), height, radius, climb, slope, cell);

    bool first_mesh = true;
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        if (!first_mesh)
        {
            file << ',';
        }
        first_mesh = false;
        ++meshes_written;
        file << "{\"id\":\"" << mesh.mId << "\",\"terrain\":" << (mesh.mTerrain ? "true" : "false") << ",\"origin\":";
        writeVec3d(file, mesh.mOrigin);
        file << ",\"tiles\":[";
        bool first_tile = true;
        for (const auto& t : mesh.mTiles)
        {
            if (!t.second.mBuilt)
            {
                continue;
            }
            if (!first_tile)
            {
                file << ',';
            }
            first_tile = false;
            writeTile(file, mesh, t.first, t.second, counts);
        }
        file << ']';
        mRooms->writeSpaces(file, mesh.mId);
        file << '}';
    }
    file << ']';

    writeFeatures(file, nullptr, nullptr, counts);
    writeDoors(file, nullptr, nullptr, counts);
    // The census's roots, named where the viewer knows them, so a capture says what it holds.
    size_t named = 0;
    std::vector<CensusRoot> roots;
    if (SKWorldCensus::instanceExists())
    {
        if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
        {
            roots = rootTable(snap->mRecords, named);
        }
    }
    file << ",\"roots\":";
    writeRootTable(file, roots);
    file << ',';
    SKNavAnnotator::instance().writeMarks(file);
    file << "}\n";

    const std::streamoff bytes = file.tellp();
    file.close();

    LL_INFOS("SKNavExport") << "wrote " << bytes << " bytes to " << path << LL_ENDL;
    return llformat("%u meshes, %u tiles: %u polys, %u segments, %u links, %u faces, %u features, %u doors, %u air, %zu marks, %zu/%zu roots named (%.1f MB) -> %s",
                    meshes_written, counts.mTiles, counts.mPolys, counts.mSegments, counts.mLinks, counts.mFaces, counts.mFeatures, counts.mDoors, counts.mAirs,
                    SKNavAnnotator::instance().marks().marks().size(), named, roots.size(), static_cast<F64>(bytes) / (1024.0 * 1024.0), path.c_str());
}

std::string SKNavMesh::exportCensus() const
{
    if (!SKWorldCensus::instanceExists() || !LLWorld::instanceExists())
    {
        return "the census is not running";
    }
    SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
    if (!snap)
    {
        return "no census yet";
    }
    static LLCachedControl<F32> air_radius(gSavedSettings, "SKNavAirRadius", 128.f);
    static LLCachedControl<F32> gap(gSavedSettings, "SKNavClusterGap", 0.5f);
    static LLCachedControl<F32> grounded_margin(gSavedSettings, "SKNavGroundedMargin", 5.f);
    static LLCachedControl<F32> void_margin(gSavedSettings, "SKNavVoidMargin", 256.f);
    static LLCachedControl<bool> sealed(gSavedSettings, "SKNavSealedIndoors", true);

    LLTimer timer;
    CensusFile census;
    census.mExported = LLDate::now().asString();
    census.mStamp = snap->mStamp;
    census.mAgent = gAgent.getPositionGlobal();
    census.mCamera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    census.mRange = snap->mRange;
    fillParams(census.mParams);
    census.mAirRadius = air_radius;
    census.mClusterGap = gap;
    census.mGroundedMargin = grounded_margin;
    census.mVoidMargin = void_margin;
    // As the rooms pass sets them up.
    census.mEnclosure.mWalkHeadM = census.mParams.mParams[static_cast<S32>(AgentClass::AVATAR)].mHeight;
    census.mEnclosure.mSealedIndoors = sealed;

    // Every window samples on whole metres from its padded corner, and every
    // corner is on the lattice: one phase covers all of a region's windows.
    const F32 pad = tilePad(census.mParams);
    const F64 phase = std::ceil(static_cast<F64>(pad)) - pad;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        CensusRegion r;
        r.mId = region->getRegionID();
        r.mHandle = region->getHandle();
        r.mOrigin = region->getOriginGlobal();
        r.mName = region->getName();
        r.mWidth = region->getWidth();
        r.mWater = region->getWaterHeight();
        LLSurface& land = region->getLand();
        r.mLandMin = land.getMinZ();
        r.mLandMax = land.getMaxZ();
        r.mPhase = static_cast<F32>(phase);
        r.mSamples = static_cast<S32>(std::ceil(r.mWidth - phase));
        r.mZ.resize(static_cast<size_t>(r.mSamples) * r.mSamples);
        for (S32 y = 0; y < r.mSamples; ++y)
        {
            for (S32 x = 0; x < r.mSamples; ++x)
            {
                const LLVector3d p(r.mOrigin.mdV[VX] + phase + x, r.mOrigin.mdV[VY] + phase + y, 0.0);
                r.mZ[static_cast<size_t>(y) * r.mSamples + x] = land.resolveHeightGlobal(p);
            }
        }
        census.mRegions.push_back(std::move(r));
    }

    census.mRecords = snap->mRecords;
    std::unordered_map<const Record*, U32> index;
    for (size_t i = 0; i < census.mRecords.size(); ++i)
    {
        index[census.mRecords[i].get()] = static_cast<U32>(i);
    }
    for (const ClusterPtr& c : snap->mClusters.mClusters)
    {
        CensusCluster cl;
        cl.mId = c->mId;
        cl.mGrounded = grounded(*c);
        for (const RecordPtr& r : c->mRecords)
        {
            const auto found = index.find(r.get());
            if (found != index.end())
            {
                cl.mRecords.push_back(found->second);
            }
        }
        census.mClusters.push_back(std::move(cl));
    }
    census.mFeatures = snap->mFeatures;
    size_t named = 0;
    census.mRoots = rootTable(census.mRecords, named);
    census.mMarks = SKNavAnnotator::instance().marks().marks();
    const LLReflectionMapManager& probes = gPipeline.mReflectionMapManager;
    static_assert(static_cast<U8>(ProbeKind::NAV_BOX) == LLReflectionMapManager::SK_PROBE_NAV_BOX, "census probe kinds are the manager's");
    for (const LLPointer<LLReflectionMap>& probe : probes.getSKProbes())
    {
        CensusProbe p;
        p.mKind = static_cast<ProbeKind>(probes.getSKProbeKind(probe));
        p.mOrigin = gAgent.getPosGlobalFromAgent(LLVector3(probe->mOrigin.getF32ptr()));
        p.mRadius = probe->mRadius;
        p.mPriority = static_cast<S32>(probe->mPriority);
        p.mRelevant = probe->isRelevant();
        p.mSuppressed = probe->mSuppressedByNavRooms;
        p.mInsideManual = probe->mInsideManualProbe;
        p.mResident = probe->isActive();
        if (probe->mViewerObject)
        {
            p.mObject = probe->mViewerObject->getID();
            p.mRotation = probe->mViewerObject->getRenderRotation();
            p.mScale = probe->mViewerObject->getScale();
        }
        census.mProbes.push_back(p);
    }
    for (const auto& region : mFeatureDoors)
    {
        census.mDoors.insert(census.mDoors.end(), region.second.begin(), region.second.end());
    }
    for (const auto& region : mFeatureLinks)
    {
        for (const auto& entry : region.second)
        {
            CensusSpan s;
            s.mId = entry.first;
            s.mKind = entry.second.mKind;
            s.mMin = entry.second.mMin;
            s.mMax = entry.second.mMax;
            for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
            {
                s.mExits[cls] = entry.second.mExits[cls];
                s.mLinks[cls] = entry.second.mLinks[cls];
            }
            census.mSpans.push_back(std::move(s));
        }
    }

    // Milliseconds in the name, so two exports in one second both survive.
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const S32 millis = static_cast<S32>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", std::gmtime(&secs));
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, llformat("sknav-census-%s.%03dZ.json", stamp, millis));
    llofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return llformat("could not write %s", path.c_str());
    }
    writeCensus(file, census);
    const std::streamoff bytes = file.tellp();
    file.close();

    // All of it on the main thread, which a large census keeps well under half a second; the time is reported to keep that honest.
    const F32 ms = timer.getElapsedTimeF32() * 1000.f;
    LL_INFOS("SKNavExport") << "wrote " << bytes << " bytes to " << path << " in " << ms << " ms" << LL_ENDL;
    LL_INFOS("SKNavExport") << named << " of " << census.mRoots.size() << " roots named" << LL_ENDL;
    return llformat("%zu records, %zu clusters, %zu regions, %zu doors, %zu features, %zu marks, %zu probes, %zu/%zu roots named (%.1f MB, %.0f ms) -> %s",
                    census.mRecords.size(), census.mClusters.size(), census.mRegions.size(), census.mDoors.size(), census.mFeatures.size(),
                    census.mMarks.size(), census.mProbes.size(), named, census.mRoots.size(),
                    static_cast<F64>(bytes) / (1024.0 * 1024.0), ms, path.c_str());
}

std::string SKNavMesh::exportObject() const
{
    // The selection first, so a vehicle can be picked in the edit tools; else
    // whatever the screen centre is on, as the mark casts.
    LLViewerObject* target = LLSelectMgr::getInstance()->getSelection()->getFirstRootObject(true);
    bool have_hit = false;
    LLVector4a hit_point;
    if (!target && gViewerWindow)
    {
        const LLRect& view = gViewerWindow->getWorldViewRectScaled();
        target = gViewerWindow->cursorIntersect(view.getCenterX(), view.getCenterY(), 1024.f, nullptr, -1, true,
                                                false, true, true, nullptr, &hit_point);
        have_hit = target != nullptr;
    }
    if (!target)
    {
        return "select an object, or point the camera at one";
    }
    // Pointing at a pilot or at what they wear means the thing they sit on.
    if (target->isAttachment())
    {
        target = target->getAvatar();
    }
    while (target && !target->isRoot() && target->getParent())
    {
        target = static_cast<LLViewerObject*>(target->getParent());
    }
    if (target && target->isAvatar() && target->getParent())
    {
        target = static_cast<LLViewerObject*>(target->getParent());
        while (!target->isRoot() && target->getParent())
        {
            target = static_cast<LLViewerObject*>(target->getParent());
        }
    }
    if (!target)
    {
        return "select an object, or point the camera at one";
    }
    LLViewerObject* root = target;

    const std::string name = llformat("sknav-object-%s.json", root->getID().asString().c_str());
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
    llofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return llformat("could not write %s", path.c_str());
    }
    file << std::setprecision(6);

    // Everything is written in the root's own frame, so a vehicle caught
    // banked, pitched or upside down still reads as its plan view down -z.
    const LLVector3 origin = root->getRenderPosition();
    const LLQuaternion rotation = root->getRenderRotation();
    const LLQuaternion inverse = ~rotation;

    auto vec3 = [&file](const LLVector3& v) {
        file << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']';
    };
    auto text = [&file](const std::string& s) {
        file << '"';
        for (char c : s)
        {
            if (c == '"' || c == '\\')
            {
                file << '\\' << c;
            }
            else if (c == '\n')
            {
                file << "\\n";
            }
            else if (static_cast<unsigned char>(c) >= 0x20)
            {
                file << c;
            }
        }
        file << '"';
    };

    U32 parts_written = 0, triangles_written = 0;

    // A frame to write triangles in: agent-space origin, the rotation into
    // it, and a reach past which triangles are left out (0 keeps them all).
    struct Frame
    {
        LLVector3 mOrigin;
        LLQuaternion mInverse;
        F32 mReach;
    };

    // One prim: its load state and, per face, how it blends and its
    // triangles in the given frame. A rigged mesh follows a skeleton, so its
    // rest positions would be wrong; it is listed without triangles.
    auto part = [&](LLViewerObject* object, S32 link, const Frame& frame) {
        ++parts_written;
        file << "{\"id\":\"" << object->getID() << "\",\"link\":" << link;
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object);
        const LLVolume* v = volume ? volume->getVolume() : nullptr;
        const bool rigged = object->isRiggedMesh();
        file << ",\"rigged\":" << (rigged ? "true" : "false") << ",\"lod\":" << object->getLOD()
             << ",\"mesh\":" << (volume && volume->isMesh() ? "true" : "false")
             << ",\"loaded\":" << (v && v->getNumVolumeFaces() > 0 ? "true" : "false") << ",\"scale\":";
        vec3(object->getScale());
        file << ",\"faces\":[";
        const S32 faces = v ? v->getNumVolumeFaces() : 0;
        const LLVector3 scale = object->getScale();
        const LLVector3 position = object->getRenderPosition();
        const LLQuaternion object_rotation = object->getRenderRotation();
        for (S32 f = 0; f < faces; ++f)
        {
            if (f)
            {
                file << ',';
            }
            const LLTextureEntry* te = object->getTE(static_cast<U8>(f));
            file << "{\"te\":" << f;
            if (te)
            {
                file << ",\"alpha\":" << te->getColor().mV[VALPHA] << ",\"pbrAlpha\":" << facePbrAlpha(te)
                     << ",\"mode\":\"" << faceAlphaMode(te) << "\",\"texture\":\"" << te->getID()
                     << "\",\"fullbright\":" << (te->getFullbright() ? "true" : "false");
            }
            file << ",\"tris\":[";
            if (!rigged)
            {
                const LLVolumeFace& face = v->getVolumeFace(f);
                bool first_tri = true;
                for (S32 i = 0; i + 2 < face.mNumIndices; i += 3)
                {
                    LLVector3 local[3];
                    for (S32 k = 0; k < 3; ++k)
                    {
                        LLVector3 world(face.mPositions[face.mIndices[i + k]].getF32ptr());
                        world.scaleVec(scale);
                        world = world * object_rotation + position;
                        local[k] = (world - frame.mOrigin) * frame.mInverse;
                    }
                    if (frame.mReach > 0.f && !withinReach(local, frame.mReach))
                    {
                        continue;
                    }
                    for (S32 k = 0; k < 3; ++k)
                    {
                        file << (first_tri && !k ? "" : ",") << local[k].mV[VX] << ',' << local[k].mV[VY] << ','
                             << local[k].mV[VZ];
                    }
                    first_tri = false;
                    ++triangles_written;
                }
            }
            file << "]}";
        }
        file << "]}";
    };

    // The linkset's prims in link order: the root, then its children that are
    // not people sitting on it.
    auto linkset = [&](LLViewerObject* object, const Frame& frame) {
        file << '[';
        part(object, 1, frame);
        for (const LLPointer<LLViewerObject>& child : object->getChildren())
        {
            if (!child->isAvatar())
            {
                file << ',';
                part(child.get(), linkNumber(child.get()), frame);
            }
        }
        file << ']';
    };
    const Frame own { origin, inverse, 0.f };

    // The heading is the root's forward axis flattened onto the ground; the
    // map turns the outline by that alone.
    const LLVector3 forward = LLVector3::x_axis * rotation;
    const LLVector3 up = LLVector3::z_axis * rotation;
    const F32 heading = std::atan2(forward.mV[VY], forward.mV[VX]) * RAD_TO_DEG;

    file << "{\"format\":\"sknav-object\",\"version\":1,\"exported\":\"" << LLDate::now().asString() << '"';
    file << ",\"root\":\"" << root->getID() << '"';
    if (root->isAvatar())
    {
        // The avatar exports itself: the body part carries no faces, what
        // it wears follows as its children, and the name comes from the
        // name cache rather than the object properties.
        file << ",\"avatar\":true,\"name\":";
        LLAvatarName av_name;
        text(LLAvatarNameCache::get(root->getID(), &av_name) ? av_name.getCompleteName() : std::string());
    }
    else if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(root->getID()))
    {
        file << ",\"name\":";
        text(props->mName);
        file << ",\"description\":";
        text(props->mDescription);
    }
    file << ",\"hover\":";
    text(root->mHudText);
    file << ",\"position\":";
    const LLVector3d global = root->getPositionGlobal();
    file << '[' << std::setprecision(10) << global.mdV[VX] << ',' << global.mdV[VY] << ',' << global.mdV[VZ] << ']'
         << std::setprecision(6);
    file << ",\"rotation\":[" << rotation.mQ[VX] << ',' << rotation.mQ[VY] << ',' << rotation.mQ[VZ] << ','
         << rotation.mQ[VW] << "],\"up\":";
    vec3(up);
    file << ",\"heading\":" << heading << ",\"velocity\":";
    vec3(root->getVelocity());
    file << ",\"physical\":" << (root->flagUsePhysics() ? "true" : "false")
         << ",\"scripted\":" << (root->flagScripted() ? "true" : "false")
         << ",\"phantom\":" << (root->flagPhantom() ? "true" : "false");
    file << ",\"parts\":";
    if (root->isAvatar())
    {
        // The body is not a prim, and what the avatar wears hangs off it
        // as its own crew entry below.
        file << "[]";
    }
    else
    {
        linkset(root, own);
    }

    // Everyone sitting anywhere on the linkset, and what each of them wears:
    // a turret or the whole visible aircraft can be an attachment. An
    // avatar exporting itself is its own crew, seat none.
    file << ",\"crew\":[";
    bool first_crew = true;
    auto crew_member = [&](LLVOAvatar* avatar, S32 seat_link) {
        if (!first_crew)
        {
            file << ',';
        }
        first_crew = false;
        LLAvatarName av_name;
        const bool named = LLAvatarNameCache::get(avatar->getID(), &av_name);
        file << "{\"id\":\"" << avatar->getID() << "\",\"name\":";
        text(named ? av_name.getCompleteName() : std::string());
        file << ",\"self\":" << (avatar->isSelf() ? "true" : "false")
             << ",\"friend\":" << (LLAvatarTracker::instance().isBuddy(avatar->getID()) ? "true" : "false");
        LLNameValue* title = avatar->getNVPair("Title");
        file << ",\"title\":";
        text(title && title->getString() ? title->getString() : "");
        file << ",\"seatLink\":" << seat_link << ",\"position\":";
        vec3((avatar->getRenderPosition() - origin) * inverse);
        file << ",\"attachments\":[";
        bool first_attachment = true;
        for (const auto& point : avatar->mAttachmentPoints)
        {
            for (const LLPointer<LLViewerObject>& attached : point.second->mAttachedObjects)
            {
                if (attached.isNull())
                {
                    continue;
                }
                if (!first_attachment)
                {
                    file << ',';
                }
                first_attachment = false;
                // The worn linkset's own pose in the vehicle's frame: a
                // turret turns about its root, and only its rotation says
                // which way it pointed when this was taken.
                const LLQuaternion worn = attached->getRenderRotation() * inverse;
                file << "{\"point\":" << point.first << ",\"item\":\"" << attached->getAttachmentItemID()
                     << "\",\"rootPosition\":";
                vec3((attached->getRenderPosition() - origin) * inverse);
                file << ",\"rootRotation\":[" << worn.mQ[VX] << ',' << worn.mQ[VY] << ',' << worn.mQ[VZ] << ','
                     << worn.mQ[VW] << "],\"parts\":";
                linkset(attached.get(), own);
                file << '}';
            }
        }
        file << "]}";
    };
    if (root->isAvatar())
    {
        crew_member(static_cast<LLVOAvatar*>(root), 0);
    }
    std::vector<LLViewerObject*> prims { root };
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        prims.push_back(child.get());
    }
    for (LLViewerObject* prim : prims)
    {
        for (const LLPointer<LLViewerObject>& child : prim->getChildren())
        {
            LLVOAvatar* avatar = child->isAvatar() ? static_cast<LLVOAvatar*>(child.get()) : nullptr;
            if (avatar)
            {
                crew_member(avatar, linkNumber(prim));
            }
        }
    }
    file << ']';

    // The navmesh's input around the point taken, in world axes about it: the
    // census records' collision triangles and the visible faces of their
    // linksets, so a build can be replayed offline and a staircase's treads
    // read against the shape the sim collides with.
    const LLVector3 center = have_hit ? LLVector3(hit_point.getF32ptr()) : origin;
    const LLVector3d center_global = gAgent.getPosGlobalFromAgent(center);
    const Frame world { center, LLQuaternion::DEFAULT, EXPORT_REACH_M };
    file << ",\"surroundings\":{\"reach\":" << EXPORT_REACH_M << ",\"center\":" << std::setprecision(10) << '['
         << center_global.mdV[VX] << ',' << center_global.mdV[VY] << ',' << center_global.mdV[VZ] << ']'
         << std::setprecision(6) << ",\"records\":[";
    U32 records_written = 0;
    std::vector<LLUUID> roots;
    if (SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot())
    {
        std::vector<LLVector3> tris;
        for (const RecordPtr& r : snap->mRecords)
        {
            bool outside = false;
            for (S32 axis = 0; axis < 3; ++axis)
            {
                outside |= r->mMax.mdV[axis] < center_global.mdV[axis] - EXPORT_REACH_M ||
                           r->mMin.mdV[axis] > center_global.mdV[axis] + EXPORT_REACH_M;
            }
            if (outside)
            {
                continue;
            }
            tris.clear();
            sknav::emitRecord(*r, center_global, tris);
            bool first_tri = true;
            for (size_t t = 0; t + 2 < tris.size(); t += 3)
            {
                if (!withinReach(&tris[t], EXPORT_REACH_M))
                {
                    continue;
                }
                if (first_tri)
                {
                    file << (records_written ? "," : "") << "{\"root\":\"" << r->mRoot << "\",\"part\":\"" << r->mPart
                         << "\",\"role\":" << static_cast<S32>(r->mRole) << ",\"provenance\":" << static_cast<S32>(r->mProvenance)
                         << ",\"navRole\":" << static_cast<S32>(r->mNavRole) << ",\"shape\":" << static_cast<S32>(r->mShape)
                         << ",\"solid\":" << (r->mSolid ? "true" : "false") << ",\"exclude\":" << static_cast<S32>(r->mExclude)
                         << ",\"tris\":[";
                    ++records_written;
                    if (std::find(roots.begin(), roots.end(), r->mRoot) == roots.end())
                    {
                        roots.push_back(r->mRoot);
                    }
                }
                for (S32 k = 0; k < 3; ++k)
                {
                    const LLVector3& p = tris[t + k];
                    file << (first_tri && !k ? "" : ",") << p.mV[VX] << ',' << p.mV[VY] << ',' << p.mV[VZ];
                }
                first_tri = false;
            }
            if (!first_tri)
            {
                file << "]}";
            }
        }
    }
    file << "],\"linksets\":[";
    bool first_linkset = true;
    for (const LLUUID& id : roots)
    {
        LLViewerObject* object = gObjectList.findObject(id);
        if (!object || object->isAvatar())
        {
            continue;
        }
        file << (first_linkset ? "" : ",") << "{\"root\":\"" << id << "\",\"parts\":";
        linkset(object, world);
        file << '}';
        first_linkset = false;
    }
    file << "]}}\n";

    const std::streamoff bytes = file.tellp();
    file.close();

    LL_INFOS("SKNavExport") << "wrote " << bytes << " bytes to " << path << LL_ENDL;
    return llformat("%u parts, %u triangles, %u nearby records (%.1f MB) -> %s", parts_written, triangles_written,
                    records_written, static_cast<F64>(bytes) / (1024.0 * 1024.0), path.c_str());
}

void SKNavMesh::refreshOverlay()
{
    static LLCachedControl<F32> range(gSavedSettings, "SKNavOverlayRange", 96.f);
    static LLCachedControl<F32> edge_width(gSavedSettings, "SKNavOverlayEdgeWidth", 3.f);
    static LLCachedControl<bool> show_world(gSavedSettings, "SKNavOverlayWorld", true);
    static LLCachedControl<bool> show_walls(gSavedSettings, "SKNavOverlayWalls", true);
    static LLCachedControl<bool> show_faces(gSavedSettings, "SKNavOverlayFaces", false);
    static LLCachedControl<bool> show_links(gSavedSettings, "SKNavOverlayLinks", true);
    static LLCachedControl<U32> show_enclosure(gSavedSettings, "SKNavOverlayEnclosure", 0);
    static LLCachedControl<bool> show_spaces(gSavedSettings, "SKNavOverlaySpaces", false);
    static LLCachedControl<bool> show_wind(gSavedSettings, "SKNavOverlayWind", false);
    static LLCachedControl<F32> wind_heading(gSavedSettings, "SKNavOverlayWindHeading", -1.f);
    static LLCachedControl<bool> show_runoff(gSavedSettings, "SKNavOverlayRunoff", false);

    // The surface itself hangs off the pipeline's debug mask, the same switch
    // as Develop > Render Metadata > Navmesh. The layers below are switches of
    // their own and draw whether or not the surface is in the way.
    const bool navmesh = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH);

    const AgentClass agent_class = AgentClass::AVATAR;
    const S32 cls = static_cast<S32>(agent_class);
    // The overlay is read by camming about, so it draws around the camera
    // alone: the agent parks wherever it was left, and a copy of the drawing
    // around it too only got in the way of what the camera came to look at.
    // Its last fifth falls away rather than stopping, so the edge reads as
    // distance; the fade bakes into the colours as they are gathered.
    LLViewerCamera* view = LLViewerCamera::getInstance();
    const LLVector3 camera_agent = view->getOrigin();
    const LLVector3 camera_forward = view->getAtAxis();
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(camera_agent);
    const F64 reach = range;
    // How wide a pixel is on screen, which is what an edge ribbon is built
    // against: the eye's vertical field of view and the viewport it fills.
    const F32 fov_y = view->getView();
    const S32 viewport_height = gViewerWindow ? gViewerWindow->getWorldViewHeightRaw() : 0;
    auto within = [&](const LLVector3d& p) {
        return dist_vec_squared2D(p, camera) <= reach * reach;
    };

    // With the world wiped there is nothing to see through: the mesh draws
    // opaque and writes depth, so stacked floors sort by depth rather than by
    // the order their tiles happen to come out of Detour. Only the surface
    // wipes the world; a layer on its own draws over it.
    const bool opaque = navmesh && !show_world;
    auto strength = [opaque](LLColor4 color, F32 scale) {
        color.mV[VALPHA] = opaque ? 1.f : color.mV[VALPHA] * scale;
        return color;
    };
    // Over the world the surface is only slightly transparent: enough to see
    // what it is lying on, not so much that it stops reading as a surface.
    const F32 FILL = 0.65f;
    // What a wall segment's cover is judged against: the reader's own eye
    // height. The wall pass below uses these, and the key needs them, because
    // they colour what is drawn.
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    static LLCachedControl<F32> peek(gSavedSettings, "SKNavCoverPeek", 0.15f);
    const F32 eye = isAgentAvatarValid() ? llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f) : 1.8f;

    // Everything the gather depends on besides the camera, hashed into one
    // value: a revision over the built mesh, the switches that shape the
    // drawing, and the mark. A new value means a new gather.
    U64 key = SEED;
    key = mix(key, mRevision);
    key = mix(key, static_cast<U64>(cls));
    key = mix(key, (navmesh ? 1ull : 0ull) | (show_world ? 2ull : 0ull) | (show_walls ? 4ull : 0ull)
                       | (show_faces ? 8ull : 0ull) | (show_links ? 16ull : 0ull) | (show_runoff ? 32ull : 0ull));
    key = mixF32(key, static_cast<F32>(reach));
    key = mixF32(key, llclamp(static_cast<F32>(edge_width), 1.f, 8.f));
    key = mixF32(key, fov_y);
    key = mix(key, static_cast<U64>(viewport_height));
    key = mixF32(key, static_cast<F32>(climb));
    key = mixF32(key, static_cast<F32>(peek));
    key = mixF32(key, eye);
    key = mix(key, mHasMark ? 1ull : 0ull);
    key = mix(key, static_cast<U64>(show_enclosure));
    key = mix(key, show_enclosure || show_spaces ? mRooms->revision() : 0ull);
    key = mix(key, show_spaces ? 1ull : 0ull);
    // The spaces layer greys the portals an annotation has crossed out, so
    // it re-gathers when the marks move.
    key = mix(key, show_spaces ? SKNavAnnotator::instance().revision() : 0ull);
    // The wind layer shows one heading: the setting's, or the region's wind where the camera is.
    F32 heading = wind_heading;
    if (show_wind && heading < 0.f)
    {
        heading = 0.f;
        if (LLViewerRegion* region = gAgent.getRegion())
        {
            const LLVector3 v = region->mWind.getVelocity(region->getPosRegionFromGlobal(camera));
            heading = v.lengthSquared() > 1e-6f ? std::atan2(v.mV[VY], v.mV[VX]) * RAD_TO_DEG : 0.f;
        }
    }
    const S32 wind_bucket = static_cast<S32>(std::lround(heading / (360.f / sknav::WIND_HEADINGS)));
    key = mix(key, show_wind ? static_cast<U64>(wind_bucket + 1000) : 0ull);
    key = mix(key, show_wind ? mRooms->revision() : 0ull);
    if (mHasMark)
    {
        key = mixF64(key, mMark.mdV[VX]);
        key = mixF64(key, mMark.mdV[VY]);
        key = mixF64(key, mMark.mdV[VZ]);
    }

    // The camera may move a metre and a half without the fade band, nineteen
    // metres deep, reading any different, and turn ten degrees without the
    // edge ribbons losing the way they face. A distance and a dot, not
    // buckets, so a camera parked at a boundary does not rebuild every frame.
    static const F32 MOVE_M = 1.5f;
    static const F32 TURN_DOT = 0.985f;
    const bool rebuild = !mOverlayFresh || key != mOverlayKey
        || dist_vec_squared(camera_agent, mOverlayCamera) > MOVE_M * MOVE_M
        || camera_forward * mOverlayForward < TURN_DOT;
    if (!rebuild)
    {
        return;
    }

    mOverlay.clear();
    // The ribbon faces the camera and widens with depth, so it is built from
    // where the camera is now; the fade waits for upload() and bakes there.
    mOverlay.setEdgeWidth(llclamp(static_cast<F32>(edge_width), 1.f, 8.f), camera_agent, fov_y, viewport_height);
    mOverlay.setFade(camera_agent, static_cast<F32>(reach), static_cast<F32>(reach * 0.8));
    // Floors tinted by how enclosed their air is; a layer of its own, like the walls.
    mRooms->gatherOverlay(mOverlay, camera, reach, show_enclosure, opaque);
    if (show_spaces)
    {
        const SKNavAnnotator& annotator = SKNavAnnotator::instance();
        mRooms->gatherSpaces(mOverlay, camera, reach, opaque,
                             [&annotator](const LLUUID& mesh, U32 space_a, U32 space_b, const LLVector3d& centre) {
                                 return annotator.portalCrossed(mesh, space_a, space_b, centre);
                             });
    }
    if (show_wind)
    {
        mRooms->gatherWind(mOverlay, camera, reach, wind_bucket * (360.f / sknav::WIND_HEADINGS), opaque);
    }

    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        const dtNavMesh* nav = mesh.mNav[cls];
        if (!navmesh || !nav)
        {
            continue;
        }
        const LLVector3 originAgent = gAgent.getPosAgentFromGlobal(mesh.mOrigin);
        for (int i = 0; i < nav->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = nav->getTile(i);
            if (!tile || !tile->header)
            {
                continue;
            }
            const LLVector3d tmin = mesh.mOrigin + LLVector3d(fromRecast(tile->header->bmin));
            const LLVector3d tmax = mesh.mOrigin + LLVector3d(fromRecast(tile->header->bmax));
            if (!within((tmin + tmax) * 0.5))
            {
                continue;
            }
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                {
                    continue;
                }
                LLColor4 color;
                switch (polyArea(poly.flags))
                {
                    case AREA_GROUND: color = skNavSwatchColor(SKNavSwatch::GROUND); break;
                    case AREA_OBJECT: color = skNavSwatchColor(SKNavSwatch::OBJECT); break;
                    case AREA_SUBMERGED: color = skNavSwatchColor(SKNavSwatch::SUBMERGED); break;
                    default: color = skNavSwatchColor(SKNavSwatch::OTHER); break;
                }
                const LLColor4 fill = strength(color, FILL);
                const LLColor4 edge = strength(color, 1.f);
                if (show_links)
                {
                    // Tile-boundary edges by whether Detour found a neighbour
                    // to join them to: an open one is a hole in the surface,
                    // and the only way to tell it from a real outside edge.
                    const LLVector3 lift(0.f, 0.f, 0.09f);
                    for (int v = 0; v < static_cast<int>(poly.vertCount); ++v)
                    {
                        if (!(poly.neis[v] & DT_EXT_LINK))
                        {
                            continue;
                        }
                        bool joined = false;
                        for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                        {
                            if (tile->links[l].edge == v && tile->links[l].side != 0xff)
                            {
                                joined = true;
                                break;
                            }
                        }
                        const LLVector3 a = originAgent + fromRecast(&tile->verts[poly.verts[v] * 3]) + lift;
                        const LLVector3 b = originAgent + fromRecast(&tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3]) + lift;
                        // full width, in place of the outline the detail pass skips on
                        // the tile's border, so the seam is one edge and not two stacked
                        mOverlay.edge(a, b, strength(skNavSwatchColor(joined ? SKNavSwatch::TILE_JOINED : SKNavSwatch::TILE_OPEN), 1.f));
                    }
                }

                // With the seams shown, an outline edge lying on the tile's border
                // is the seam's to draw
                const F32* tb0 = tile->header->bmin;
                const F32* tb1 = tile->header->bmax;
                const auto onBorder = [&](const F32* p, const F32* q) {
                    const F32 eps = 0.01f;
                    for (int axis : { 0, 2 })
                    {
                        for (F32 edge_at : { tb0[axis], tb1[axis] })
                        {
                            if (std::fabs(p[axis] - edge_at) < eps && std::fabs(q[axis] - edge_at) < eps)
                            {
                                return true;
                            }
                        }
                    }
                    return false;
                };
                const dtPolyDetail& detail = tile->detailMeshes[p];
                for (unsigned int t = 0; t < detail.triCount; ++t)
                {
                    const unsigned char* tri = &tile->detailTris[(detail.triBase + t) * 4];
                    LLVector3 v[3];
                    const F32* rcs[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const F32* rc = tri[k] < poly.vertCount ? &tile->verts[poly.verts[tri[k]] * 3]
                                                                : &tile->detailVerts[(detail.vertBase + tri[k] - poly.vertCount) * 3];
                        rcs[k] = rc;
                        v[k] = originAgent + fromRecast(rc);
                    }
                    const LLVector3 fill_lift(0.f, 0.f, 0.04f);
                    mOverlay.tri(v[0] + fill_lift, v[1] + fill_lift, v[2] + fill_lift, fill);
                    // Only the polygon outline: the detail triangles under it
                    // are the same surface and add nothing but noise.
                    const LLVector3 lift(0.f, 0.f, 0.06f);
                    for (int k = 0; k < 3; ++k)
                    {
                        if ((dtGetDetailTriEdgeFlags(tri[3], k) & DT_DETAIL_EDGE_BOUNDARY)
                            && !(show_links && onBorder(rcs[k], rcs[(k + 1) % 3])))
                        {
                            mOverlay.edge(v[k] + lift, v[(k + 1) % 3] + lift, edge);
                        }
                    }
                }
            }
        }
    }

    // Walls: the floor edges by what lies beyond them, cover coloured against
    // the user's own eye height, and a tick from each ledge to where it lands.
    if (show_walls)
    {
        const LLVector3 lift(0.f, 0.f, 0.08f);
        // One query over the camera's reach, then the round test per segment: the
        // box is only there to keep the query small.
        LLVector3d qmin, qmax;
        for (S32 axis = 0; axis < 3; ++axis)
        {
            qmin.mdV[axis] = camera.mdV[axis] - reach;
            qmax.mdV[axis] = camera.mdV[axis] + reach;
        }
        segmentsIn(qmin, qmax, agent_class, [&](const LLVector3d& a, const LLVector3d& b, EdgeKind kind, F32 value) {
            if (!within(a))
            {
                return;
            }
            LLColor4 color;
            switch (kind)
            {
                case EdgeKind::WALL:
                    switch (coverClass(value, eye, climb, peek))
                    {
                        case Cover::FULL: color = skNavSwatchColor(SKNavSwatch::COVER_FULL); break;
                        case Cover::HALF: color = skNavSwatchColor(SKNavSwatch::COVER_HALF); break;
                        default: color = skNavSwatchColor(SKNavSwatch::COVER_LOW); break;
                    }
                    break;
                case EdgeKind::LEDGE: color = skNavSwatchColor(SKNavSwatch::LEDGE); break;
                case EdgeKind::OVERHANG: color = skNavSwatchColor(SKNavSwatch::OVERHANG); break;
                default: return;
            }
            color = strength(color, 1.f);
            const LLVector3 pa = gAgent.getPosAgentFromGlobal(a) + lift;
            const LLVector3 pb = gAgent.getPosAgentFromGlobal(b) + lift;
            mOverlay.line(pa, pb, color);
            // A wall is drawn again at its own height, so how tall it is shows.
            if (kind == EdgeKind::WALL)
            {
                const LLVector3 up(0.f, 0.f, value);
                mOverlay.line(pa + up, pb + up, color);
            }
        });
        if (show_faces)
        {
            // The vertical surfaces themselves, the whole exposed height of
            // them. The cover segments above stop at four metres because that
            // is all a cover class can mean; these do not.
            const LLColor4 base = skNavSwatchColor(SKNavSwatch::WALL_FACE);
            wallFacesIn(qmin, qmax, [&](const LLVector3d& fmin, const LLVector3d& fmax, U8 side, U8) {
                if (!within((fmin + fmax) * 0.5))
                {
                    return;
                }
                const LLVector3 a = gAgent.getPosAgentFromGlobal(fmin), b = gAgent.getPosAgentFromGlobal(fmax);
                // A face is a thin box; the thin axis is the one it faces along.
                const LLVector3 size = b - a;
                S32 thin = 0;
                for (S32 k = 1; k < 3; ++k)
                {
                    thin = (size.mV[k] < size.mV[thin]) ? k : thin;
                }
                const S32 u = (thin + 1) % 3, v = (thin + 2) % 3;
                LLVector3 corner[4];
                for (S32 c = 0; c < 4; ++c)
                {
                    corner[c] = a;
                    corner[c].mV[thin] = (a.mV[thin] + b.mV[thin]) * 0.5f;
                    corner[c].mV[u] = (c == 1 || c == 2) ? b.mV[u] : a.mV[u];
                    corner[c].mV[v] = (c >= 2) ? b.mV[v] : a.mV[v];
                }
                // Which way it faces, by brightness, so a corner reads as two
                // surfaces rather than one.
                LLColor4 color = base;
                const F32 tint = 0.7f + 0.1f * static_cast<F32>(side & 3);
                for (S32 k = 0; k < 3; ++k)
                {
                    color.mV[k] *= tint;
                }
                mOverlay.tri(corner[0], corner[1], corner[2], strength(color, 1.f));
                mOverlay.tri(corner[0], corner[2], corner[3], strength(color, 1.f));
                LLColor4 line = color;
                line.mV[VALPHA] = 0.9f;
                for (S32 c = 0; c < 4; ++c)
                {
                    mOverlay.line(corner[c], corner[(c + 1) % 4], strength(line, 1.f));
                }
            });
        }

        const LLColor4 drop = strength(skNavSwatchColor(SKNavSwatch::DROP), 1.f);
        const LLColor4 climb_link = strength(skNavSwatchColor(SKNavSwatch::CLIMB), 1.f);
        linksIn(qmin, qmax, agent_class, [&](const LLVector3d& from, const LLVector3d& to, F32, LinkKind kind) {
            if (within(from))
            {
                mOverlay.line(gAgent.getPosAgentFromGlobal(from) + lift, gAgent.getPosAgentFromGlobal(to) + lift,
                              kind == LinkKind::DROP ? drop : climb_link);
            }
        });
    }
    if (show_runoff)
    {
        // The raster's own layer: where the air's floors end over a drop, and the run-off down them.
        const LLVector3 lift(0.f, 0.f, 0.06f);
        LLVector3d qmin, qmax;
        for (S32 axis = 0; axis < 3; ++axis)
        {
            qmin.mdV[axis] = camera.mdV[axis] - reach;
            qmax.mdV[axis] = camera.mdV[axis] + reach;
        }
        const LLColor4 ledge = strength(skNavSwatchColor(SKNavSwatch::LEDGE), 0.8f);
        const LLColor4 runoff = strength(skNavSwatchColor(SKNavSwatch::SUBMERGED), 1.f);
        runoffIn(
            qmin, qmax,
            [&](const LLVector3d& a, const LLVector3d& b, EdgeKind, F32) {
                if (within(a))
                {
                    mOverlay.line(gAgent.getPosAgentFromGlobal(a) + lift, gAgent.getPosAgentFromGlobal(b) + lift, ledge);
                }
            },
            [&](const LLVector3d& from, const LLVector3d& to, F32, LinkKind) {
                if (within(from))
                {
                    mOverlay.line(gAgent.getPosAgentFromGlobal(from) + lift, gAgent.getPosAgentFromGlobal(to), runoff);
                }
            });
    }

    if (mHasMark)
    {
        const LLVector3 m = gAgent.getPosAgentFromGlobal(mMark);
        const LLColor4 pink(1.f, 0.2f, 1.f, 0.95f);
        mOverlay.edge(m - LLVector3(0.f, 0.f, 5.f), m + LLVector3(0.f, 0.f, 5.f), pink);
        mOverlay.edge(m - LLVector3(1.f, 0.f, 0.f), m + LLVector3(1.f, 0.f, 0.f), pink);
        mOverlay.edge(m - LLVector3(0.f, 1.f, 0.f), m + LLVector3(0.f, 1.f, 0.f), pink);
    }

    mOverlay.upload();
    mOverlayKey = key;
    mOverlayCamera = camera_agent;
    mOverlayForward = camera_forward;
    mOverlayFresh = true;
}

void SKNavMesh::renderDebug()
{
    static LLCachedControl<bool> xray(gSavedSettings, "SKNavOverlayXRay", false);
    static LLCachedControl<bool> show_world(gSavedSettings, "SKNavOverlayWorld", true);
    const bool navmesh = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH);
    // Whether the surface wipes the world or lies over it decides both the
    // alpha baked into the cache and the depth state below, so read it here.
    const bool opaque = navmesh && !show_world;

    refreshOverlay();

    if (opaque)
    {
        // Wipe the frame, as the stock pathfinding console does, and leave the
        // world in the pipeline: it is still rendered and still picked, so the
        // camera goes on working. Taking the render types out instead would
        // give alt-cam nothing to aim at.
        const LLColor4 clear = gSavedSettings.getColor4("SKNavOverlayBackdrop");
        gGL.flush();
        gGL.setColorMask(true, true);
        glClearColor(clear.mV[VRED], clear.mV[VGREEN], clear.mV[VBLUE], 0.f);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        gGL.setColorMask(true, false);
    }
    if (mOverlay.empty())
    {
        return;
    }

    SKNavOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        {
            LLGLDepthTest depth(GL_TRUE, opaque ? GL_TRUE : GL_FALSE);
            mOverlay.draw();
        }
        if (xray && !opaque)
        {
            // What is behind geometry, darker and fainter, so it reads as being
            // seen through the wall rather than in front of it.
            LLGLDepthTest behind(GL_TRUE, GL_FALSE, GL_GREATER);
            mOverlay.draw(0.55f, 0.45f);
        }
    }
    SKNavOverlayGeom::unbindShader();
}
