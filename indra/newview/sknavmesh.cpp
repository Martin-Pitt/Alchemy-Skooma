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

#include "sknavoverlay.h"
#include "sknavpalette.h"

#include "alobjectproperties.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "llagent.h"
#include "llappviewer.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llglslshader.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llviewershadermgr.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "skphysicsshape.h"
#include "workqueue.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <set>
#include <thread>

namespace
{
    using namespace sknav;

    // Detour packs tile, poly and salt bits into 32: 10 + 12 leaves the 10 salt
    // bits init insists on. 1024 tiles is a square kilometre per structure.
    const S32 MAX_TILES_PER_MESH = 1024;
    const S32 MAX_POLYS_PER_TILE = 4096;

    /// How far past a tile geometry and terrain are taken: Recast's border
    /// (radius cells + 3) for the widest class, plus a metre of terrain grid.
    F32 padFor(const TileBuildInput& params)
    {
        F32 border = 0.f;
        for (S32 c = 0; c < NUM_CLASSES; ++c)
        {
            if (params.mClasses[c])
            {
                border = std::max(border, (std::ceil(params.mParams[c].mRadius / params.mCell) + 3.f) * params.mCell);
            }
        }
        return border + 1.f;
    }

    LLVector3d tileMin(const LLVector3d& origin, const TileKey& key)
    {
        return LLVector3d(origin.mdV[VX] + key.mX * TILE_M, origin.mdV[VY] + key.mY * TILE_M, 0.0);
    }

    bool overlapsXY(const LLVector3d& amin, const LLVector3d& amax, const LLVector3d& bmin, const LLVector3d& bmax)
    {
        return !(amin.mdV[VX] > bmax.mdV[VX] || bmin.mdV[VX] > amax.mdV[VX] || amin.mdV[VY] > bmax.mdV[VY] || bmin.mdV[VY] > amax.mdV[VY]);
    }

    /// The records each lattice tile can see, bucketed by the cells their
    /// padded AABB touches. A tile is one cell, so a lookup is one hash: the
    /// per-tile scan over every record in the region is gone, and a record is
    /// kept only by the tiles it can actually reach.
    struct RecordGrid
    {
        std::unordered_map<TileKey, std::vector<RecordPtr>, TileKeyHash> mCells;

        void insert(const RecordPtr& r, F32 pad)
        {
            const LLVector3d pmin = r->mMin - LLVector3d(pad, pad, 0);
            const LLVector3d pmax = r->mMax + LLVector3d(pad, pad, 0);
            const TileKey k0 = tileOf(pmin), k1 = tileOf(pmax);
            for (S32 y = k0.mY; y <= k1.mY; ++y)
            {
                for (S32 x = k0.mX; x <= k1.mX; ++x)
                {
                    mCells[{ x, y }].push_back(r);
                }
            }
        }

        void query(const TileKey& key, std::vector<RecordPtr>& out) const
        {
            const auto it = mCells.find(key);
            if (it == mCells.end())
            {
                out.clear();
                return;
            }
            out = it->second;
        }
    };

    /// The lattice origin a mesh gets: the tile corner at or below the point, z zero.
    LLVector3d latticeOrigin(const LLVector3d& p)
    {
        const TileKey k = tileOf(p);
        return LLVector3d(k.mX * TILE_M, k.mY * TILE_M, 0.0);
    }

    U8 areaOf(unsigned short flags)
    {
        for (U8 a = 1; a < 8; ++a)
        {
            if (flags & (1u << a))
            {
                return a;
            }
        }
        return 0;
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
    return gSavedSettings.getBOOL("SKNavOverlayWalls") || gSavedSettings.getBOOL("SKNavOverlayFaces")
        || gSavedSettings.getBOOL("SKNavOverlayCensus");
}

bool SKNavMesh::wanted() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavMeshEnabled", true);
    static LLCachedControl<bool> always(gSavedSettings, "SKNavMeshAlways", false);
    return enabled && (always || overlayActive());
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

void SKNavMesh::cleanupSingleton()
{
    // Join the pool before the singleton dies: a job in flight carries this
    // pointer in its publish callback, and the main loop stops pumping first.
    if (mWorkers)
    {
        mWorkers->close();
        mWorkers.reset();
    }
}

bool SKNavMesh::ensureNav(Mesh& mesh, S32 cls)
{
    if (mesh.mNav[cls])
    {
        return true;
    }
    dtNavMesh* nav = dtAllocNavMesh();
    dtNavMeshParams p;
    p.orig[0] = p.orig[1] = p.orig[2] = 0.f;    // coordinates are mesh-local already
    p.tileWidth = static_cast<F32>(TILE_M);
    p.tileHeight = static_cast<F32>(TILE_M);
    p.maxTiles = MAX_TILES_PER_MESH;
    p.maxPolys = MAX_POLYS_PER_TILE;
    if (!dtStatusSucceed(nav->init(&p)))
    {
        dtFreeNavMesh(nav);
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
    h = mixF32(h, probe.mDropSpacing);
    for (S32 c = 0; c < NUM_CLASSES; ++c)
    {
        h = mix(h, probe.mClasses[c]);
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
    static LLCachedControl<F32> weatherSlope(gSavedSettings, "SKNavWeatherSlope", 89.f);
    static LLCachedControl<F32> weatherRadius(gSavedSettings, "SKNavWeatherRadius", 0.125f);
    static LLCachedControl<F32> weatherHeight(gSavedSettings, "SKNavWeatherHeight", 0.5f);
    static LLCachedControl<bool> weather(gSavedSettings, "SKNavBuildWeather", false);
    static LLCachedControl<bool> walls(gSavedSettings, "SKNavBuildWalls", true);
    static LLCachedControl<F32> dropAvatar(gSavedSettings, "SKNavDropMax", 10.f);
    static LLCachedControl<F32> dropWeather(gSavedSettings, "SKNavWeatherDropMax", 24.f);
    static LLCachedControl<F32> dropSpacing(gSavedSettings, "SKNavDropSpacing", 2.f);

    static LLCachedControl<U32> partition(gSavedSettings, "SKNavPartition", 0);
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

    input.mClasses[static_cast<S32>(AgentClass::AVATAR)] = true;
    input.mParams[static_cast<S32>(AgentClass::AVATAR)] = { height, radius, climb,
                                                            llclamp(static_cast<F32>(slope), 1.f, 89.9f) };
    input.mClasses[static_cast<S32>(AgentClass::WEATHER)] = weather;
    // Weather runs down whatever it lands on, so nearly everything is floor to
    // it. A face that is truly vertical still cannot be a walkable polygon of
    // a heightfield - it has no top - which is what the wall faces are for.
    //
    // Its radius is zero: weather has no width, and erosion would hold it a
    // cell short of the walls it is supposed to drain down. That does skip
    // rcErodeWalkableArea, which is a suspect whenever this class produces
    // degenerate polygons - SKNavWeatherRadius at 0.125 is the experiment.
    input.mParams[static_cast<S32>(AgentClass::WEATHER)] = { weatherHeight, llmax(static_cast<F32>(weatherRadius), 0.f),
                                                             climb,
                                                             llclamp(static_cast<F32>(weatherSlope), 1.f, 89.9f) };

    // Faces are for the weather class's readers, edges and drops for both.
    input.mWallFaces = walls && weather;
    input.mEdges[static_cast<S32>(AgentClass::AVATAR)] = walls;
    input.mEdges[static_cast<S32>(AgentClass::WEATHER)] = walls && weather;
    input.mDropMax[static_cast<S32>(AgentClass::AVATAR)] = dropAvatar;
    input.mDropMax[static_cast<S32>(AgentClass::WEATHER)] = dropWeather;
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

void SKNavMesh::sampleTerrain()
{
    static LLCachedControl<F32> budget(gSavedSettings, "SKNavTerrainBudgetMS", 1.f);
    LLTimer timer;
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = padFor(probe);
    const F32 budget_ms = llmax(0.1f, static_cast<F32>(budget));
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        if (!mesh.mTerrain)
        {
            continue;
        }
        for (auto& t : mesh.mTiles)
        {
            Tile& tile = t.second;
            if (!tile.mOverLand || !tile.mTerrainStale)
            {
                continue;
            }
            if (timer.getElapsedTimeF32() * 1000.f > budget_ms)
            {
                return;
            }
            auto window = std::make_shared<TerrainWindow>();
            fillTerrain(mesh.mOrigin, tile.mMin, tile.mMax, pad, *window);
            tile.mTerrain = window;
            tile.mTerrainStale = false;
        }
    }
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

bool SKNavMesh::floatsClear(const sknav::Record& record) const
{
    // The sim takes the navmesh out of an exclusion volume whatever its size
    // or height; everything else here is a judgement about walking.
    if (record.mNavRole == sknav::NavRole::EXCLUSION_VOLUME)
    {
        return false;
    }
    if (!LLWorld::instanceExists())
    {
        return false;
    }
    static LLCachedControl<F32> margin(gSavedSettings, "SKNavFloatMargin", 0.5f);
    static LLCachedControl<F32> shelf(gSavedSettings, "SKNavFloatShelf", 1.f);
    // A plan narrower than a shelf cannot carry a surface worth a drop link;
    // the erosion would take most of it back anyway. The world AABB, so a
    // rotated part is judged by the span it actually covers. Cheapest test
    // first: most parts this throws out early are wide ones.
    if (record.mMax.mdV[VX] - record.mMin.mdV[VX] >= shelf
        || record.mMax.mdV[VY] - record.mMin.mdV[VY] >= shelf)
    {
        return false;
    }
    LLVector3d foot = (record.mMin + record.mMax) * 0.5;
    foot.mdV[VZ] = record.mMin.mdV[VZ];
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(foot);
    if (region)
    {
        // The heightfield first, as the cheapest answer: right where nothing
        // is built over it, wrong wherever the land people walk on is mesh
        // and prim standing on the heightfield - which the built mesh knows
        // instead, asked below.
        const F32 land = LLWorld::getInstance()->resolveLandHeightGlobal(foot);
        if (record.mMin.mdV[VZ] - land <= margin)
        {
            // It meets the ground: an obstacle at walking height, however small.
            return false;
        }
        // The tile under it has never been built: no surface exists there to
        // meet, and this part is not why the tile will be built. Leave it
        // out; the scan after the tile publishes puts it back if the surface
        // there reaches it. Without this the first build-out pays a query
        // per part into meshes that hold nothing.
        const auto mit = mMeshes.find(region->getRegionID());
        if (mit != mMeshes.end())
        {
            const Mesh& mesh = *mit->second;
            const TileKey key{ tileIndex(foot.mdV[VX]) - tileIndex(mesh.mOrigin.mdV[VX]),
                               tileIndex(foot.mdV[VY]) - tileIndex(mesh.mOrigin.mdV[VY]) };
            const auto tit = mesh.mTiles.find(key);
            if (tit == mesh.mTiles.end() || !tit->second.mBuilt)
            {
                return true;
            }
        }
    }
    // What is actually under it to meet: the nearest walkable polygon of any
    // built mesh, whatever that surface stands on. Asked at the record's
    // foot, so the surface a part rests on is the one found, not one metres
    // over a tall thin record's middle.
    F32 z = 0.f;
    if (floorUnder(foot, AgentClass::AVATAR, z) && record.mMin.mdV[VZ] - z <= margin)
    {
        // It meets a built floor: an obstacle or a shelf at walking height.
        return false;
    }
    // Hanging clear of every walkable surface, and too small ever to be one.
    return true;
}

void SKNavMesh::reconcile(const SKCensusSnapshotPtr& snap)
{
    const U64 params = paramsSignature();
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = padFor(probe);
    const U64 world_stamp = worldStamp();
    mStats.mSkipped = 0;

    // Grounded structures are meshed with the region they stand in, so that
    // one mesh holds the ground, the walls that block it and the floors that
    // lead off it. Meshing them separately meant every cluster sharing a
    // lattice tile rasterised that tile's ground again.
    std::vector<RecordPtr> ground_records;
    std::vector<const Cluster*> airborne;
    for (const ClusterPtr& c : snap->mClusters.mClusters)
    {
        if (grounded(*c))
        {
            for (const RecordPtr& r : c->mRecords)
            {
                // A part hanging in the air, too small ever to be walked on,
                // used to cost every tile it hung over a rebuild for a sliver
                // the erosion was going to take back. Leave it out; if it
                // later settles onto the ground it comes back as an ordinary
                // record and the tile re-signs once, for something that lasts.
                if (floatsClear(*r))
                {
                    mStats.mSkipped++;
                    continue;
                }
                ground_records.push_back(r);
            }
        }
        else
        {
            airborne.push_back(c.get());
        }
    }

    std::set<LLUUID> live;

    // Clusters clear of the ground: a mesh each, and no terrain in it. A
    // skybox three kilometres up has nothing to do with the land below it, and
    // a heightfield spanning the gap would overflow Recast's span heights.
    for (const Cluster* c : airborne)
    {
        live.insert(c->mId);
        std::unique_ptr<Mesh>& mesh = mMeshes[c->mId];
        if (!mesh)
        {
            mesh = std::make_unique<Mesh>();
            mesh->mId = c->mId;
            mesh->mGeneration = mNextGeneration++;
            mesh->mOrigin = latticeOrigin(c->mMin);
            if (!c->mRecords.empty())
            {
                LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(c->mRecords.front()->mOrigin + LLVector3d(1, 1, 0));
                mesh->mRegion = region ? region->getHandle() : 0;
            }
        }
        mesh->mLastSeen = gFrameTimeSeconds;
        const S32 ox = tileIndex(mesh->mOrigin.mdV[VX]), oy = tileIndex(mesh->mOrigin.mdV[VY]);
        RecordGrid grid;
        for (const RecordPtr& r : c->mRecords)
        {
            grid.insert(r, pad);
        }
        std::map<TileKey, Tile> wanted;
        for (const auto& entry : c->mTiles)
        {
            const TileKey key{ entry.first.mX - ox, entry.first.mY - oy };
            Tile& tile = wanted[key];
            const auto old = mesh->mTiles.find(key);
            if (old != mesh->mTiles.end())
            {
                tile = old->second;
            }
            std::vector<RecordPtr> before = std::move(tile.mRecords);
            tile.mMin = tileMin(mesh->mOrigin, key);
            tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
            tile.mMin.mdV[VZ] = c->mMin.mdV[VZ];
            tile.mMax.mdV[VZ] = c->mMax.mdV[VZ];
            grid.query(entry.first, tile.mRecords);
            reSign(tile, before, recordSignature(tile.mRecords), 0, params);
        }
        if (dropTiles(*mesh, wanted))
        {
            mRevision++;
        }
        mesh->mTiles.swap(wanted);
    }

    // One mesh per region: its terrain, and every record that reaches over
    // that land. A record's own region names it for the void past every edge,
    // but where it stands on another region's land it is meshed with that
    // land, so a structure and the ground under it stay one mesh even when
    // the structure was rezzed across the border.
    if (LLWorld::instanceExists())
    {
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            live.insert(region->getRegionID());
            std::unique_ptr<Mesh>& mesh = mMeshes[region->getRegionID()];
            if (!mesh)
            {
                mesh = std::make_unique<Mesh>();
                mesh->mId = region->getRegionID();
                mesh->mTerrain = true;
                mesh->mRegion = region->getHandle();
                mesh->mGeneration = mNextGeneration++;
                mesh->mOrigin = latticeOrigin(region->getOriginGlobal());
            }
            mesh->mLastSeen = gFrameTimeSeconds;

            const LLVector3d& origin = region->getOriginGlobal();
            const F64 width = region->getWidth();
            const LLVector3d land_min = origin - LLVector3d(pad, pad, 0);
            const LLVector3d land_max = origin + LLVector3d(width + pad, width + pad, 0);
            const S32 ox = tileIndex(mesh->mOrigin.mdV[VX]), oy = tileIndex(mesh->mOrigin.mdV[VY]);
            std::vector<RecordPtr> land_records;    // reaches over this region's land
            std::vector<RecordPtr> own_records;     // this region's own, for the void
            for (const RecordPtr& r : ground_records)
            {
                if (overlapsXY(r->mMin, r->mMax, land_min, land_max))
                {
                    land_records.push_back(r);
                }
                if (r->mOrigin == origin)
                {
                    own_records.push_back(r);
                }
            }

            const S32 per_edge = static_cast<S32>(std::ceil(region->getWidth() / TILE_M));
            const LLSurface& land = region->getLand();
            const U64 land_stamp = landStamp(*region);
            RecordGrid land_grid;
            for (const RecordPtr& r : land_records)
            {
                land_grid.insert(r, pad);
            }
            RecordGrid void_grid;
            for (const RecordPtr& r : own_records)
            {
                void_grid.insert(r, pad);
            }

            // The region's own tiles, plus the void past its edge as far as
            // its own records reach, within SKNavVoidMargin: the surrounds and
            // the things hanging off the sim are out there, and the void is
            // exactly where they are.
            static LLCachedControl<F32> void_margin(gSavedSettings, "SKNavVoidMargin", 256.f);
            const S32 out = llclamp(static_cast<S32>(std::ceil(void_margin / TILE_M)), 0, 16);
            S32 x0 = 0, x1 = per_edge - 1, y0 = 0, y1 = per_edge - 1;
            for (const RecordPtr& r : own_records)
            {
                x0 = llmin(x0, tileIndex(r->mMin.mdV[VX]) - ox);
                x1 = llmax(x1, tileIndex(r->mMax.mdV[VX]) - ox);
                y0 = llmin(y0, tileIndex(r->mMin.mdV[VY]) - oy);
                y1 = llmax(y1, tileIndex(r->mMax.mdV[VY]) - oy);
            }
            x0 = llmax(x0, -out);
            y0 = llmax(y0, -out);
            x1 = llmin(x1, per_edge - 1 + out);
            y1 = llmin(y1, per_edge - 1 + out);

            std::map<TileKey, Tile> wanted;
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    const TileKey key{ x, y };
                    const TileKey global{ ox + x, oy + y };
                    const bool over_land = (x >= 0 && x < per_edge && y >= 0 && y < per_edge);
                    Tile& tile = wanted[key];
                    const auto old = mesh->mTiles.find(key);
                    if (old != mesh->mTiles.end())
                    {
                        tile = old->second;
                    }
                    std::vector<RecordPtr> before = std::move(tile.mRecords);
                    tile.mMin = tileMin(mesh->mOrigin, key);
                    tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
                    tile.mMin.mdV[VZ] = land.getMinZ();
                    tile.mMax.mdV[VZ] = land.getMaxZ();
                    if (over_land)
                    {
                        land_grid.query(global, tile.mRecords);
                        tile.mOverLand = true;
                        // The land under the tile stands until its region's
                        // stamp moves, and then only this tile re-signs and
                        // waits for its window to be sampled again. The world
                        // stamp catches a region arriving or leaving under the
                        // tile's padded edge.
                        if (!tile.mTerrain || tile.mLandStamp != land_stamp || tile.mWorldStamp != world_stamp)
                        {
                            tile.mLandStamp = land_stamp;
                            tile.mWorldStamp = world_stamp;
                            tile.mTerrainSignature = terrainSignature(tile.mMin, tile.mMax, pad);
                            tile.mTerrainStale = true;
                        }
                    }
                    else
                    {
                        // Past this region's edge only the void is this
                        // region's business. A tile over another region's land
                        // belongs to that region's mesh, records and all, so
                        // the ground and what stands on it stay together and
                        // the same patch is not meshed twice.
                        if (LLWorld::getInstance()->getRegionFromPosGlobal((tile.mMin + tile.mMax) * 0.5))
                        {
                            wanted.erase(key);
                            continue;
                        }
                        void_grid.query(global, tile.mRecords);
                        if (tile.mRecords.empty())
                        {
                            // Empty void: nothing to mesh out here.
                            wanted.erase(key);
                            continue;
                        }
                        tile.mOverLand = false;
                        tile.mTerrain.reset();
                        tile.mTerrainStale = false;
                        tile.mTerrainSignature = 0;
                    }
                    reSign(tile, before, recordSignature(tile.mRecords), tile.mTerrainSignature, params);
                }
            }
            if (dropTiles(*mesh, wanted))
            {
                mRevision++;
            }
            mesh->mTiles.swap(wanted);
        }
    }

    // A cluster that has gone, or that has come down to the ground and is now
    // the region's business, takes its mesh with it.
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
    {
        if (live.count(it->second->mId))
        {
            ++it;
        }
        else
        {
            it = mMeshes.erase(it);
            mRevision++;            // its tiles leave the overlay with it
        }
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
    for (const RecordPtr& r : after)
    {
        if (r && std::find(before.begin(), before.end(), r) == before.end())
        {
            out = (r->mMin + r->mMax) * 0.5;
            return true;
        }
    }
    for (const RecordPtr& r : before)
    {
        if (r && std::find(after.begin(), after.end(), r) == after.end())
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

bool SKNavMesh::dropTiles(Mesh& mesh, const std::map<TileKey, Tile>& wanted)
{
    bool dropped = false;
    for (const auto& old : mesh.mTiles)
    {
        if (wanted.count(old.first) == 0 && old.second.mBuilt)
        {
            for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
            {
                if (mesh.mNav[cls])
                {
                    mesh.mNav[cls]->removeTile(mesh.mNav[cls]->getTileRefAt(old.first.mY, old.first.mX, 0), nullptr, nullptr);
                }
            }
            dropped = true;
        }
    }
    return dropped;
}

bool SKNavMesh::launch(Mesh& mesh, const TileKey& key, Tile& tile)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t workers = workerQueue();
    if (!main_queue || !workers || workers->isClosed())
    {
        return false;
    }

    auto input = std::make_shared<TileBuildInput>();
    input->mMeshOrigin = mesh.mOrigin;
    input->mTileX = key.mX;
    input->mTileY = key.mY;
    input->mTileSize = static_cast<F32>(TILE_M);
    fillParams(*input);
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

    F32 zmin = 1e9f, zmax = -1e9f;
    for (F32 z : input->mTerrain.mZ)
    {
        if (z > -900.f)
        {
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
        }
    }
    const F32 land_min = zmin;
    for (const RecordPtr& r : tile.mRecords)
    {
        // An exclusion volume is not a shape: the sim takes the navmesh out
        // of its inside. Build it as a cut, not as something to stand on.
        if (r && r->mNavRole == sknav::NavRole::EXCLUSION_VOLUME)
        {
            input->mCuts.push_back(r);
        }
        else
        {
            input->mRecords.push_back(r);
        }
    }
    for (const RecordPtr& r : tile.mRecords)
    {
        zmin = std::min(zmin, static_cast<F32>(r->mMin.mdV[VZ] - mesh.mOrigin.mdV[VZ]));
        zmax = std::max(zmax, static_cast<F32>(r->mMax.mdV[VZ] - mesh.mOrigin.mdV[VZ]));
    }
    // The land's lowest sample is the deepest ground there is: a record
    // buried below it never becomes a floor, so the heightfield does not
    // reach down for it. Whatever still falls under the surface, the tile
    // build takes out of the spans.
    if (land_min < 1e9f)
    {
        zmin = std::max(zmin, land_min);
    }
    if (zmin > zmax)
    {
        zmin = zmax = 0.f;
    }
    input->mZMin = zmin - 1.f;
    input->mZMax = zmax + 3.f;

    const LLUUID id = mesh.mId;
    const U32 generation = mesh.mGeneration;
    const U64 signature = tile.mSignature;
    const bool posted = main_queue->postTo(
        workers,
        [input]() { return std::make_shared<TileBuildResult>(buildTile(*input)); },
        [this, id, generation, key, signature](std::shared_ptr<TileBuildResult> result) { publish(id, generation, key, signature, result); });
    if (posted)
    {
        tile.mInFlight = true;
        mInFlight++;
    }
    return posted;
}

void SKNavMesh::publish(const LLUUID& mesh_id, U32 generation, const TileKey& key, U64 signature, std::shared_ptr<TileBuildResult> result)
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
        return;     // superseded: the scheduler will launch it again
    }
    mStats.mBuilds++;
    if (result->mStatus == TileBuildResult::SUCCESS)
    {
        mStats.mLastBuildMs = result->mMillis;
        mStats.mWorstBuildMs = std::max(mStats.mWorstBuildMs, result->mMillis);
    }
    if (result->mStatus == TileBuildResult::FAILED)
    {
        mStats.mFailures++;
        LL_WARNS("SKNav") << "tile build failed: " << result->mError << LL_ENDL;
        // The walls of the build before this one describe geometry that has
        // changed; drop them rather than answer with them.
        tile.mFaces.reset();
        for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
        {
            tile.mSegments[cls].reset();
            tile.mLinks[cls].reset();
        }
        tile.mBuilt = signature;    // do not retry the same input
        tile.mCause = CAUSE_FAILED;
        tile.mHasCauseAt = false;
        tileBlip(mesh, tile);
        mRevision++;                // the tile's walls are gone: the overlay moved
        return;
    }
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        tile.mPolys[cls] = result->mPolys[cls];
        const std::vector<U8>& data = result->mData[cls];
        if (!mesh.mNav[cls] && data.empty())
        {
            continue;
        }
        if (!ensureNav(mesh, cls))
        {
            continue;
        }
        dtNavMesh* nav = mesh.mNav[cls];
        // Detour's x runs along Recast x, which is SL y.
        const dtTileRef old = nav->getTileRefAt(key.mY, key.mX, 0);
        if (old)
        {
            nav->removeTile(old, nullptr, nullptr);
        }
        if (!data.empty())
        {
            unsigned char* copy = static_cast<unsigned char*>(dtAlloc(static_cast<int>(data.size()), DT_ALLOC_PERM));
            std::memcpy(copy, data.data(), data.size());
            if (!dtStatusSucceed(nav->addTile(copy, static_cast<int>(data.size()), DT_TILE_FREE_DATA, 0, nullptr)))
            {
                dtFree(copy);
                tile.mPolys[cls] = 0;
                mStats.mFailures++;
                LL_WARNS("SKNav") << "addTile failed for mesh " << mesh_id << " tile " << key.mX << "," << key.mY << LL_ENDL;
            }
        }
    }
    tile.mFaces = std::make_shared<const std::vector<WallFace>>(std::move(result->mFaces));
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        tile.mSegments[cls] = std::make_shared<const std::vector<WallSegment>>(std::move(result->mSegments[cls]));
        tile.mLinks[cls] = std::make_shared<const std::vector<DropLink>>(std::move(result->mLinks[cls]));
    }
    tile.mBuilt = signature;
    tile.mMillis = result->mMillis;
    tileBlip(mesh, tile);
    mRevision++;                    // a published tile is one the overlay redraws
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

    struct Candidate
    {
        Mesh* mMesh;
        TileKey mKey;
        Tile* mTile;
        F64 mDistance;
    };
    std::vector<Candidate> candidates;
    // Nearest of the two: the ground under the avatar keeps its priority, and
    // camming across a sim builds what you are looking at.
    const LLVector3d agent = gAgent.getPositionGlobal();
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        for (auto& t : mesh.mTiles)
        {
            Tile& tile = t.second;
            if (tile.mInFlight || tile.mSignature == tile.mBuilt)
            {
                continue;
            }
            const LLVector3d centre = (tile.mMin + tile.mMax) * 0.5;
            const F64 nearest = llmin(dist_vec_squared2D(centre, agent), dist_vec_squared2D(centre, camera));
            candidates.push_back({ &mesh, t.first, &tile, nearest });
        }
    }
    mStats.mQueued = static_cast<U32>(candidates.size());
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.mDistance < b.mDistance; });

    S32 launched = 0;
    for (const Candidate& c : candidates)
    {
        if (launched >= perFrame || mInFlight >= in_flight_cap)
        {
            break;
        }
        if (launch(*c.mMesh, c.mKey, *c.mTile))
        {
            launched++;
        }
    }
}

void SKNavMesh::update()
{
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
        reconcile(snap);
    }

    // Meshes nothing has claimed for a while are gone with their structure.
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
    {
        if (gFrameTimeSeconds - it->second->mLastSeen > 30.f)
        {
            it = mMeshes.erase(it);
            mRevision++;
        }
        else
        {
            ++it;
        }
    }

    sampleTerrain();
    schedule();

    mStats.mMeshes = static_cast<U32>(mMeshes.size());
    mStats.mTiles = 0;
    mStats.mTilesBuilt = 0;
    mStats.mFaces = 0;
    mStats.mSegments = 0;
    mStats.mLinks = 0;
    mStats.mInFlight = mInFlight;
    for (const auto& entry : mMeshes)
    {
        for (const auto& t : entry.second->mTiles)
        {
            mStats.mTiles++;
            if (t.second.mBuilt)
            {
                mStats.mTilesBuilt++;
                mStats.mFaces += t.second.mFaces ? static_cast<U32>(t.second.mFaces->size()) : 0;
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    mStats.mSegments += t.second.mSegments[cls] ? static_cast<U32>(t.second.mSegments[cls]->size()) : 0;
                    mStats.mLinks += t.second.mLinks[cls] ? static_cast<U32>(t.second.mLinks[cls]->size()) : 0;
                }
            }
        }
    }

    static LLCachedControl<bool> log(gSavedSettings, "SKNavCensusLog", false);
    if (log && gFrameTimeSeconds - mLastLog > 5.f)
    {
        mLastLog = gFrameTimeSeconds;
        LL_INFOS("SKNav") << statsLine() << LL_ENDL;
    }
}

std::string SKNavMesh::statsLine() const
{
    return llformat("navmesh: %u meshes, %u/%u tiles built (%u queued, %u in flight), %u faces, %u segments, %u links, %u builds, %u failures, %u floats skipped, last %.1f ms, worst %.1f ms",
                    mStats.mMeshes, mStats.mTilesBuilt, mStats.mTiles, mStats.mQueued, mStats.mInFlight,
                    mStats.mFaces, mStats.mSegments, mStats.mLinks, mStats.mBuilds,
                    mStats.mFailures, mStats.mSkipped, mStats.mLastBuildMs, mStats.mWorstBuildMs);
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
        dtQueryFilter filter;
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
    dtQueryFilter filter;
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
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        for (const auto& t : mesh.mTiles)
        {
            const Tile& tile = t.second;
            if (tile.mBuilt && overlapsXY(tile.mMin, tile.mMax, min, max))
            {
                fn(mesh, tile);
            }
        }
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

void SKNavMesh::segmentsIn(const LLVector3d& min, const LLVector3d& max, AgentClass cls, const SegmentFn& fn) const
{
    const S32 c = static_cast<S32>(cls);
    forEachTileIn(min, max, [&](const Mesh& mesh, const Tile& tile) {
        if (!tile.mSegments[c])
        {
            return;
        }
        for (const WallSegment& seg : *tile.mSegments[c])
        {
            const LLVector3d a = mesh.mOrigin + LLVector3d(seg.mA), b = mesh.mOrigin + LLVector3d(seg.mB);
            if (overlapsXY(LLVector3d(llmin(a.mdV[VX], b.mdV[VX]), llmin(a.mdV[VY], b.mdV[VY]), 0.0),
                           LLVector3d(llmax(a.mdV[VX], b.mdV[VX]), llmax(a.mdV[VY], b.mdV[VY]), 0.0), min, max))
            {
                fn(a, b, seg.mKind, seg.mValue);
            }
        }
    });
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
                fn(from, to, link.mHeight);
            }
        }
    });
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
    S32 link = 1;
    if (LLViewerObject* parent = static_cast<LLViewerObject*>(hit->getParent()))
    {
        link = 2;
        for (const LLPointer<LLViewerObject>& child : parent->getChildren())
        {
            if (child.get() == hit)
            {
                break;
            }
            ++link;
        }
    }
    out << "  link " << link << " of " << (root->numChildren() + 1) << " flags";
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
            << " polys " << tile.mPolys[0] << "/" << tile.mPolys[1] << " built " << (tile.mBuilt == tile.mSignature)
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
                << ".." << r->mMax.mdV[VZ] << " tris " << (r->mTriangles ? r->mTriangles->size() / 3 : 0) << "\n";
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
            dtQueryFilter filter;
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
                dtQueryFilter area_filter;
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

    auto vec3d = [&file](const LLVector3d& v) {
        file << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']';
    };
    auto vec3 = [&file](const LLVector3& v) {
        file << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']';
    };

    F32 height = 2.f;
    if (isAgentAvatarValid())
    {
        height = llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f);
    }
    static LLCachedControl<F32> cell(gSavedSettings, "SKNavCell", 0.125f);
    static LLCachedControl<F32> radius(gSavedSettings, "SKNavAvatarRadius", 0.3f);
    static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
    static LLCachedControl<F32> slope(gSavedSettings, "SKNavSlope", 74.f);

    U32 meshes_written = 0, tiles_written = 0, polys_written = 0, segments_written = 0;
    U32 links_written = 0, faces_written = 0;

    file << "{\"format\":\"sknavmesh-export\",\"version\":1,\"exported\":\"" << LLDate::now().asString() << '"';
    file << ",\"agent\":";
    vec3d(gAgent.getPositionGlobal());
    file << ",\"params\":{\"height\":" << height << ",\"radius\":" << radius << ",\"climb\":" << climb
         << ",\"slope\":" << slope << ",\"cell\":" << cell << '}';
    file << ",\"classes\":[\"avatar\",\"weather\"]";
    file << ",\"edgeKinds\":[\"open\",\"wall\",\"ledge\",\"overhang\"]";
    file << ",\"areas\":[\"none\",\"ground\",\"object\",\"submerged\",\"drop\",\"5\",\"6\",\"7\"]";
    file << ",\"meshes\":[";

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
        vec3d(mesh.mOrigin);
        file << ",\"tiles\":[";
        bool first_tile = true;
        for (const auto& t : mesh.mTiles)
        {
            const Tile& tile = t.second;
            if (!tile.mBuilt)
            {
                continue;
            }
            if (!first_tile)
            {
                file << ',';
            }
            first_tile = false;
            ++tiles_written;
            file << "{\"key\":[" << t.first.mX << ',' << t.first.mY << "],\"min\":";
            vec3d(tile.mMin);
            file << ",\"max\":";
            vec3d(tile.mMax);
            if (tile.mFaces)
            {
                file << ",\"faces\":[";
                bool first = true;
                for (const WallFace& f : *tile.mFaces)
                {
                    if (!first)
                    {
                        file << ',';
                    }
                    first = false;
                    ++faces_written;
                    file << "{\"min\":";
                    vec3(f.mMin);
                    file << ",\"max\":";
                    vec3(f.mMax);
                    file << ",\"side\":" << static_cast<S32>(f.mSide) << ",\"area\":" << static_cast<S32>(f.mArea) << '}';
                }
                file << ']';
            }
            for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
            {
                file << ",\"" << (cls == 0 ? "avatar" : "weather") << "\":{";
                const dtNavMesh* nav = mesh.mNav[cls];
                // Detour x runs along Recast x, which is SL y, so the tile at
                // this lattice key sits at (mY, mX).
                const dtMeshTile* dt = nav ? nav->getTileAt(t.first.mY, t.first.mX, 0) : nullptr;
                file << "\"polys\":[";
                if (dt && dt->header)
                {
                    bool first = true;
                    for (S32 p = 0; p < dt->header->polyCount; ++p)
                    {
                        const dtPoly& poly = dt->polys[p];
                        if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                        {
                            continue;
                        }
                        if (!first)
                        {
                            file << ',';
                        }
                        first = false;
                        ++polys_written;
                        file << "{\"v\":[";
                        for (S32 v = 0; v < static_cast<S32>(poly.vertCount); ++v)
                        {
                            if (v)
                            {
                                file << ',';
                            }
                            vec3d(mesh.mOrigin + LLVector3d(fromRecast(&dt->verts[poly.verts[v] * 3])));
                        }
                        file << "],\"area\":" << static_cast<S32>(areaOf(poly.flags)) << '}';
                    }
                }
                file << "],\"segments\":[";
                if (tile.mSegments[cls])
                {
                    bool first = true;
                    for (const WallSegment& seg : *tile.mSegments[cls])
                    {
                        if (!first)
                        {
                            file << ',';
                        }
                        first = false;
                        ++segments_written;
                        file << "{\"a\":";
                        vec3d(mesh.mOrigin + LLVector3d(seg.mA));
                        file << ",\"b\":";
                        vec3d(mesh.mOrigin + LLVector3d(seg.mB));
                        file << ",\"kind\":" << static_cast<S32>(seg.mKind) << ",\"value\":" << seg.mValue
                             << ",\"area\":" << static_cast<S32>(seg.mArea) << '}';
                    }
                }
                file << "],\"links\":[";
                if (tile.mLinks[cls])
                {
                    bool first = true;
                    for (const DropLink& link : *tile.mLinks[cls])
                    {
                        if (!first)
                        {
                            file << ',';
                        }
                        first = false;
                        ++links_written;
                        file << "{\"from\":";
                        vec3d(mesh.mOrigin + LLVector3d(link.mFrom));
                        file << ",\"to\":";
                        vec3d(mesh.mOrigin + LLVector3d(link.mTo));
                        file << ",\"height\":" << link.mHeight << '}';
                    }
                }
                file << "]}";
            }
            file << '}';
        }
        file << "]}";
    }
    file << "]}\n";

    const std::streamoff bytes = file.tellp();
    file.close();

    LL_INFOS("SKNavExport") << "wrote " << bytes << " bytes to " << path << LL_ENDL;
    return llformat("%u meshes, %u tiles: %u polys, %u segments, %u links, %u faces (%.1f MB) -> %s", meshes_written,
                    tiles_written, polys_written, segments_written, links_written, faces_written,
                    static_cast<F64>(bytes) / (1024.0 * 1024.0), path.c_str());
}

void SKNavMesh::refreshOverlay()
{
    static LLCachedControl<F32> range(gSavedSettings, "SKNavOverlayRange", 96.f);
    static LLCachedControl<F32> edge_width(gSavedSettings, "SKNavOverlayEdgeWidth", 3.f);
    static LLCachedControl<bool> show_world(gSavedSettings, "SKNavOverlayWorld", true);
    static LLCachedControl<bool> show_walls(gSavedSettings, "SKNavOverlayWalls", true);
    static LLCachedControl<bool> show_faces(gSavedSettings, "SKNavOverlayFaces", false);
    static LLCachedControl<bool> show_links(gSavedSettings, "SKNavOverlayLinks", true);

    // The surface itself hangs off the pipeline's debug mask, the same switch
    // as Develop > Render Metadata > Navmesh. The layers below are switches of
    // their own and draw whether or not the surface is in the way.
    const bool navmesh = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH);

    // Which class the overlay shows. WEATHER is only there to look at when
    // it is being built (SKNavBuildWeather); the console says so.
    static LLCachedControl<U32> which(gSavedSettings, "SKNavOverlayClass", 0);
    const S32 cls = llclamp(static_cast<S32>(which), 0, NUM_CLASSES - 1);
    const AgentClass agent_class = static_cast<AgentClass>(cls);
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
                       | (show_faces ? 8ull : 0ull) | (show_links ? 16ull : 0ull));
    key = mixF32(key, static_cast<F32>(reach));
    key = mixF32(key, llclamp(static_cast<F32>(edge_width), 1.f, 8.f));
    key = mixF32(key, fov_y);
    key = mix(key, static_cast<U64>(viewport_height));
    key = mixF32(key, static_cast<F32>(climb));
    key = mixF32(key, static_cast<F32>(peek));
    key = mixF32(key, eye);
    key = mix(key, mHasMark ? 1ull : 0ull);
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
                switch (areaOf(poly.flags))
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
                        mOverlay.line(a, b, strength(skNavSwatchColor(joined ? SKNavSwatch::TILE_JOINED : SKNavSwatch::TILE_OPEN), 1.f));
                    }
                }

                const dtPolyDetail& detail = tile->detailMeshes[p];
                for (unsigned int t = 0; t < detail.triCount; ++t)
                {
                    const unsigned char* tri = &tile->detailTris[(detail.triBase + t) * 4];
                    LLVector3 v[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const F32* rc = tri[k] < poly.vertCount ? &tile->verts[poly.verts[tri[k]] * 3]
                                                                : &tile->detailVerts[(detail.vertBase + tri[k] - poly.vertCount) * 3];
                        v[k] = originAgent + fromRecast(rc);
                    }
                    const LLVector3 fill_lift(0.f, 0.f, 0.04f);
                    mOverlay.tri(v[0] + fill_lift, v[1] + fill_lift, v[2] + fill_lift, fill);
                    // Only the polygon outline: the detail triangles under it
                    // are the same surface and add nothing but noise.
                    const LLVector3 lift(0.f, 0.f, 0.06f);
                    for (int k = 0; k < 3; ++k)
                    {
                        if (dtGetDetailTriEdgeFlags(tri[3], k) & DT_DETAIL_EDGE_BOUNDARY)
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
        linksIn(qmin, qmax, agent_class, [&](const LLVector3d& from, const LLVector3d& to, F32) {
            if (within(from))
            {
                mOverlay.line(gAgent.getPosAgentFromGlobal(from) + lift, gAgent.getPosAgentFromGlobal(to) + lift, drop);
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
