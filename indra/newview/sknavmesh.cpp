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

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llglslshader.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
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
#include <sstream>
#include <set>

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

bool SKNavMesh::wanted() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavMeshEnabled", true);
    static LLCachedControl<bool> always(gSavedSettings, "SKNavMeshAlways", false);
    return enabled && (always || gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NAVMESH));
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
    input.mPartition = static_cast<Partition>(llclamp(static_cast<S32>(partition), 0, 2));
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
    const U64 params = paramsSignature();
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = padFor(probe);

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
            ground_records.insert(ground_records.end(), c->mRecords.begin(), c->mRecords.end());
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
            tile.mMin = tileMin(mesh->mOrigin, key);
            tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
            tile.mMin.mdV[VZ] = c->mMin.mdV[VZ];
            tile.mMax.mdV[VZ] = c->mMax.mdV[VZ];
            gatherRecords(c->mRecords, tile, pad);
            tile.mSignature = mix(recordSignature(tile.mRecords), params);
        }
        dropTiles(*mesh, wanted);
        mesh->mTiles.swap(wanted);
    }

    // One mesh per region: its terrain, and every structure standing on it. A
    // record belongs to the region its own origin names, so a build straddling
    // a border is split between the two rather than rasterised into both.
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
            const S32 ox = tileIndex(mesh->mOrigin.mdV[VX]), oy = tileIndex(mesh->mOrigin.mdV[VY]);
            std::vector<RecordPtr> mine;
            for (const RecordPtr& r : ground_records)
            {
                if (r->mOrigin == origin)
                {
                    mine.push_back(r);
                }
            }

            const S32 per_edge = static_cast<S32>(std::ceil(region->getWidth() / TILE_M));
            const LLSurface& land = region->getLand();

            // Past the region's own edge as far as its records reach, within
            // SKNavVoidMargin: the surrounds and the things hanging off the
            // sim are out there, and the void is exactly where they are.
            static LLCachedControl<F32> void_margin(gSavedSettings, "SKNavVoidMargin", 256.f);
            const S32 out = llclamp(static_cast<S32>(std::ceil(void_margin / TILE_M)), 0, 16);
            S32 x0 = 0, x1 = per_edge - 1, y0 = 0, y1 = per_edge - 1;
            for (const RecordPtr& r : mine)
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
                    Tile& tile = wanted[key];
                    const auto old = mesh->mTiles.find(key);
                    if (old != mesh->mTiles.end())
                    {
                        tile = old->second;
                    }
                    tile.mMin = tileMin(mesh->mOrigin, key);
                    tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
                    tile.mMin.mdV[VZ] = land.getMinZ();
                    tile.mMax.mdV[VZ] = land.getMaxZ();
                    gatherRecords(mine, tile, pad);
                    const bool over_land = (x >= 0 && x < per_edge && y >= 0 && y < per_edge);
                    if (!over_land && tile.mRecords.empty())
                    {
                        // Empty void: nothing to mesh out here.
                        wanted.erase(key);
                        continue;
                    }
                    tile.mSignature = mix(mix(recordSignature(tile.mRecords),
                                              terrainSignature(tile.mMin, tile.mMax, pad)), params);
                }
            }
            dropTiles(*mesh, wanted);
            mesh->mTiles.swap(wanted);
        }
    }

    // A cluster that has gone, or that has come down to the ground and is now
    // the region's business, takes its mesh with it.
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
    {
        it = live.count(it->second->mId) ? std::next(it) : mMeshes.erase(it);
    }
}

void SKNavMesh::gatherRecords(const std::vector<RecordPtr>& from, Tile& tile, F32 pad)
{
    const LLVector3d pmin = tile.mMin - LLVector3d(pad, pad, 0), pmax = tile.mMax + LLVector3d(pad, pad, 0);
    tile.mRecords.clear();
    for (const RecordPtr& r : from)
    {
        if (overlapsXY(r->mMin, r->mMax, pmin, pmax))
        {
            tile.mRecords.push_back(r);
        }
    }
}

U64 SKNavMesh::recordSignature(const std::vector<RecordPtr>& records)
{
    U64 h = SEED;
    for (const RecordPtr& r : records)
    {
        h = mix(h, r->mSignature);
    }
    return h;
}

void SKNavMesh::dropTiles(Mesh& mesh, const std::map<TileKey, Tile>& wanted)
{
    for (const auto& old : mesh.mTiles)
    {
        if (wanted.count(old.first) == 0 && old.second.mBuilt)
        {
            mesh.mLevelsStale = true;
            for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
            {
                if (mesh.mNav[cls])
                {
                    mesh.mNav[cls]->removeTile(mesh.mNav[cls]->getTileRefAt(old.first.mY, old.first.mX, 0), nullptr, nullptr);
                }
            }
        }
    }
}

bool SKNavMesh::launch(Mesh& mesh, const TileKey& key, Tile& tile)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general || general->isClosed())
    {
        return false;
    }

    auto input = std::make_shared<TileBuildInput>();
    input->mMeshOrigin = mesh.mOrigin;
    input->mTileX = key.mX;
    input->mTileY = key.mY;
    input->mTileSize = static_cast<F32>(TILE_M);
    fillParams(*input);
    const F32 pad = padFor(*input);
    // Only a region mesh has ground in it: a cluster clear of the ground has
    // nothing to do with the land far below it, and a heightfield spanning
    // the gap would overflow Recast's span heights.
    if (mesh.mTerrain)
    {
        fillTerrain(mesh.mOrigin, tile.mMin, tile.mMax, pad, input->mTerrain);
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
    input->mRecords = tile.mRecords;
    for (const RecordPtr& r : input->mRecords)
    {
        zmin = std::min(zmin, static_cast<F32>(r->mMin.mdV[VZ] - mesh.mOrigin.mdV[VZ]));
        zmax = std::max(zmax, static_cast<F32>(r->mMax.mdV[VZ] - mesh.mOrigin.mdV[VZ]));
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
        general,
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
    mesh.mPublishedAt = gFrameTimeSeconds;
    mesh.mLevelsStale = true;
}

void SKNavMesh::relabel()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SKNavBuildLevels", true);
    static LLCachedControl<F32> settle(gSavedSettings, "SKNavLevelSettleSeconds", 0.25f);
    static LLCachedControl<F32> band(gSavedSettings, "SKNavLevelBand", 0.75f);
    static LLCachedControl<F32> minArea(gSavedSettings, "SKNavLevelMinArea", 16.f);
    if (!enabled)
    {
        return;
    }
    // One structure a frame, the nearest first: the walk is cheap but a mall
    // is not, and nothing needs its storeys the instant a tile lands.
    Mesh* pick = nullptr;
    F32 oldest = 0.f;
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        if (!mesh.mLevelsStale || mesh.mPublishedAt < 0.f || gFrameTimeSeconds - mesh.mPublishedAt < settle)
        {
            continue;
        }
        const F32 waited = gFrameTimeSeconds - mesh.mRelabelledAt;
        if (!pick || waited > oldest)
        {
            pick = &mesh;
            oldest = waited;
        }
    }
    if (!pick)
    {
        return;
    }
    pick->mLevelsStale = false;
    pick->mRelabelledAt = gFrameTimeSeconds;

    LevelParams params;
    params.mBand = band;
    params.mMinArea = minArea;
    for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
    {
        if (!pick->mNav[cls] || !pick->mQuery[cls])
        {
            pick->mLevels[cls].reset();
            continue;
        }
        std::vector<DropLink> links;
        for (const auto& t : pick->mTiles)
        {
            if (t.second.mLinks[cls])
            {
                links.insert(links.end(), t.second.mLinks[cls]->begin(), t.second.mLinks[cls]->end());
            }
        }
        auto levels = std::make_shared<Levels>();
        buildLevels(*pick->mNav[cls], *pick->mQuery[cls], params, links, *levels);
        pick->mLevels[cls] = levels;
    }
}

void SKNavMesh::schedule()
{
    static LLCachedControl<S32> perFrame(gSavedSettings, "SKNavBuildsPerFrame", 2);
    static LLCachedControl<S32> inFlightMax(gSavedSettings, "SKNavBuildsInFlight", 2);

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
        if (launched >= perFrame || mInFlight >= inFlightMax)
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
        it = (gFrameTimeSeconds - it->second->mLastSeen > 30.f) ? mMeshes.erase(it) : std::next(it);
    }

    schedule();
    relabel();

    mStats.mMeshes = static_cast<U32>(mMeshes.size());
    mStats.mTiles = 0;
    mStats.mTilesBuilt = 0;
    mStats.mPolys = 0;
    mStats.mFaces = 0;
    mStats.mSegments = 0;
    mStats.mLinks = 0;
    mStats.mLevels = 0;
    mStats.mConnectors = 0;
    mStats.mInFlight = mInFlight;
    for (const auto& entry : mMeshes)
    {
        if (const Levels* levels = entry.second->mLevels[0].get())
        {
            for (const Level& level : levels->mLevels)
            {
                mStats.mLevels += level.mTransitional ? 0 : 1;
            }
            mStats.mConnectors += static_cast<U32>(levels->mConnectors.size());
        }
        for (const auto& t : entry.second->mTiles)
        {
            mStats.mTiles++;
            if (t.second.mBuilt)
            {
                mStats.mTilesBuilt++;
                mStats.mPolys += t.second.mPolys[0];
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
    return llformat("navmesh: %u meshes, %u/%u tiles built (%u queued, %u in flight), %u avatar polys, %u storeys, %u connectors, %u faces, %u segments, %u links, %u builds, %u failures, last %.1f ms, worst %.1f ms",
                    mStats.mMeshes, mStats.mTilesBuilt, mStats.mTiles, mStats.mQueued, mStats.mInFlight, mStats.mPolys,
                    mStats.mLevels, mStats.mConnectors, mStats.mFaces, mStats.mSegments, mStats.mLinks, mStats.mBuilds,
                    mStats.mFailures, mStats.mLastBuildMs, mStats.mWorstBuildMs);
}

SKNavMesh::Mesh* SKNavMesh::meshAt(const LLVector3d& pos, AgentClass cls, F32& z_out, U16* level_out) const
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
                if (level_out)
                {
                    *level_out = mesh.mLevels[c] ? mesh.mLevels[c]->levelOf(ref) : Levels::NO_LEVEL;
                }
            }
        }
    }
    return best;
}

bool SKNavMesh::floorUnder(const LLVector3d& pos, AgentClass cls, F32& z_out, LLUUID* mesh_out, U16* level_out) const
{
    Mesh* mesh = meshAt(pos, cls, z_out, level_out);
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

void SKNavMesh::connectorsOf(const LLUUID& mesh_id, AgentClass cls, U16 level, const ConnectorFn& fn) const
{
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end())
    {
        return;
    }
    const Mesh& mesh = *it->second;
    // A copy, so relabelling cannot swap it out under the walk.
    const std::shared_ptr<const Levels> levels = mesh.mLevels[static_cast<S32>(cls)];
    if (!levels)
    {
        return;
    }
    for (const Connector& c : levels->mConnectors)
    {
        if (c.mLow != level && c.mHigh != level)
        {
            continue;
        }
        fn(mesh.mOrigin + LLVector3d(c.mLowPoint), mesh.mOrigin + LLVector3d(c.mHighPoint), c.mKind, c.mOneWay);
    }
}

bool SKNavMesh::levelHeight(const LLUUID& mesh_id, AgentClass cls, U16 level, F32& z_out) const
{
    const auto it = mMeshes.find(mesh_id);
    if (it == mMeshes.end())
    {
        return false;
    }
    const std::shared_ptr<const Levels> levels = it->second->mLevels[static_cast<S32>(cls)];
    if (!levels || level >= levels->mLevels.size())
    {
        return false;
    }
    z_out = levels->mLevels[level].mZ + static_cast<F32>(it->second->mOrigin.mdV[VZ]);
    return true;
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
                << ".." << r->mMax.mdV[VZ] << " tris " << (r->mTriangles ? r->mTriangles->size() / 3 : 0);
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
        // height, and which surface does is the whole question.
        S32 tris_shown = 0;
        const F64 slack = 0.15;
        for (const RecordPtr& r : snap->mRecords)
        {
            if (!r->mTriangles)
            {
                continue;
            }
            const LLVector3d local = mMark - r->mOrigin;
            for (size_t t = 0; t + 2 < r->mTriangles->size() && tris_shown < 12; t += 3)
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
                LLVector3 n = (b - a) % (c - a);
                F32 nz = 0.f;
                if (n.normVec() > 0.f)
                {
                    nz = n.mV[VZ];
                }
                out << "  tri over the mark z " << zmin << ".." << zmax << " normal z " << nz << " provenance "
                    << static_cast<S32>(r->mProvenance) << " root " << r->mRoot << "\n";
                ++tris_shown;
            }
        }
        if (tris_shown >= 12)
        {
            out << "  ... and more triangles over the mark\n";
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
    return llformat("marked %.1f, %.1f, %.1f - the rest is in the log under SKNavDump", mMark.mdV[VX], mMark.mdV[VY],
                    mMark.mdV[VZ]);
}

void SKNavMesh::renderDebug()
{
    static LLCachedControl<F32> range(gSavedSettings, "SKNavOverlayRange", 96.f);
    static LLCachedControl<F32> edge_width(gSavedSettings, "SKNavOverlayEdgeWidth", 3.f);
    static LLCachedControl<bool> xray(gSavedSettings, "SKNavOverlayXRay", false);
    static LLCachedControl<bool> show_world(gSavedSettings, "SKNavOverlayWorld", true);
    static LLCachedControl<bool> by_level(gSavedSettings, "SKNavOverlayByLevel", false);
    static LLCachedControl<bool> show_walls(gSavedSettings, "SKNavOverlayWalls", true);
    static LLCachedControl<bool> show_faces(gSavedSettings, "SKNavOverlayFaces", false);
    static LLCachedControl<bool> show_links(gSavedSettings, "SKNavOverlayLinks", true);
    // Off by default: the storey model joins a stairwell to a floor at an
    // arbitrary polygon on it, so the lines land far from the stairs.
    static LLCachedControl<bool> show_connectors(gSavedSettings, "SKNavOverlayConnectors", false);

    // Which class the overlay shows. WEATHER is only there to look at when
    // it is being built (SKNavBuildWeather); the console says so.
    static LLCachedControl<U32> which(gSavedSettings, "SKNavOverlayClass", 0);
    const S32 cls = llclamp(static_cast<S32>(which), 0, NUM_CLASSES - 1);
    const AgentClass agent_class = static_cast<AgentClass>(cls);
    // The mesh is built around the agent, but the overlay is read by camming
    // about, so anything near either one is drawn.
    const LLVector3d agent = gAgent.getPositionGlobal();
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 reach = range;
    auto within = [&](const LLVector3d& p) {
        return dist_vec_squared2D(p, agent) <= reach * reach || dist_vec_squared2D(p, camera) <= reach * reach;
    };

    // With the world wiped there is nothing to see through: the mesh draws
    // opaque and writes depth, so stacked storeys sort by depth rather than by
    // the order their tiles happen to come out of Detour.
    const bool opaque = !show_world;
    auto strength = [opaque](LLColor4 color, F32 scale) {
        color.mV[VALPHA] = opaque ? 1.f : color.mV[VALPHA] * scale;
        return color;
    };
    // Over the world the surface is only slightly transparent: enough to see
    // what it is lying on, not so much that it stops reading as a surface.
    const F32 FILL = 0.65f;

    SKNavOverlayGeom geom;
    {
        LLViewerCamera* view = LLViewerCamera::getInstance();
        geom.setEdgeWidth(llclamp(static_cast<F32>(edge_width), 1.f, 8.f), view->getOrigin(), view->getView(),
                          gViewerWindow ? gViewerWindow->getWorldViewHeightRaw() : 0);
    }
    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        const dtNavMesh* nav = mesh.mNav[cls];
        if (!nav)
        {
            continue;
        }
        const std::shared_ptr<const Levels> levels = mesh.mLevels[cls];
        if (levels && show_connectors)
        {
            // The ways off each storey, drawn from the low end to the high one.
            const LLColor4 color = strength(skNavSwatchColor(SKNavSwatch::CONNECTOR), 1.f);
            const LLVector3 lift(0.f, 0.f, 0.1f);
            for (const Connector& c : levels->mConnectors)
            {
                const LLVector3d low = mesh.mOrigin + LLVector3d(c.mLowPoint);
                if (within(low))
                {
                    geom.line(gAgent.getPosAgentFromGlobal(low) + lift,
                              gAgent.getPosAgentFromGlobal(mesh.mOrigin + LLVector3d(c.mHighPoint)) + lift, color);
                }
            }
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
                if (by_level && levels)
                {
                    // Each storey its own colour, so a building's floors can be
                    // told apart at a glance.
                    const U16 level = levels->levelOf(nav->getPolyRefBase(tile) | static_cast<dtPolyRef>(p));
                    if (level == Levels::NO_LEVEL)
                    {
                        color = skNavSwatchColor(SKNavSwatch::LEVEL);
                    }
                    else
                    {
                        const U32 h = static_cast<U32>(mix(SEED, level));
                        color.set(0.3f + 0.7f * ((h & 255) / 255.f), 0.3f + 0.7f * (((h >> 8) & 255) / 255.f),
                                  0.3f + 0.7f * (((h >> 16) & 255) / 255.f), 0.9f);
                    }
                }
                else
                {
                    switch (areaOf(poly.flags))
                    {
                        case AREA_GROUND: color = skNavSwatchColor(SKNavSwatch::GROUND); break;
                        case AREA_OBJECT: color = skNavSwatchColor(SKNavSwatch::OBJECT); break;
                        case AREA_SUBMERGED: color = skNavSwatchColor(SKNavSwatch::SUBMERGED); break;
                        default: color = skNavSwatchColor(SKNavSwatch::LEVEL); break;
                    }
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
                        geom.line(a, b, strength(skNavSwatchColor(joined ? SKNavSwatch::TILE_JOINED : SKNavSwatch::TILE_OPEN), 1.f));
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
                    geom.tri(v[0] + fill_lift, v[1] + fill_lift, v[2] + fill_lift, fill);
                    // Only the polygon outline: the detail triangles under it
                    // are the same surface and add nothing but noise.
                    const LLVector3 lift(0.f, 0.f, 0.06f);
                    for (int k = 0; k < 3; ++k)
                    {
                        if (dtGetDetailTriEdgeFlags(tri[3], k) & DT_DETAIL_EDGE_BOUNDARY)
                        {
                            geom.edge(v[k] + lift, v[(k + 1) % 3] + lift, edge);
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
        static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
        static LLCachedControl<F32> peek(gSavedSettings, "SKNavCoverPeek", 0.15f);
        const F32 eye = isAgentAvatarValid() ? llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f) : 1.8f;
        const LLVector3 lift(0.f, 0.f, 0.08f);
        // One query over both centres, then the round test per segment: two
        // queries would draw whatever lies in both boxes twice.
        LLVector3d qmin, qmax;
        for (S32 axis = 0; axis < 3; ++axis)
        {
            qmin.mdV[axis] = llmin(agent.mdV[axis], camera.mdV[axis]) - reach;
            qmax.mdV[axis] = llmax(agent.mdV[axis], camera.mdV[axis]) + reach;
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
            geom.line(pa, pb, color);
            // A wall is drawn again at its own height, so how tall it is shows.
            if (kind == EdgeKind::WALL)
            {
                const LLVector3 up(0.f, 0.f, value);
                geom.line(pa + up, pb + up, color);
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
                geom.tri(corner[0], corner[1], corner[2], strength(color, 1.f));
                geom.tri(corner[0], corner[2], corner[3], strength(color, 1.f));
                LLColor4 line = color;
                line.mV[VALPHA] = 0.9f;
                for (S32 c = 0; c < 4; ++c)
                {
                    geom.line(corner[c], corner[(c + 1) % 4], strength(line, 1.f));
                }
            });
        }

        const LLColor4 drop = strength(skNavSwatchColor(SKNavSwatch::DROP), 1.f);
        linksIn(qmin, qmax, agent_class, [&](const LLVector3d& from, const LLVector3d& to, F32) {
            if (within(from))
            {
                geom.line(gAgent.getPosAgentFromGlobal(from) + lift, gAgent.getPosAgentFromGlobal(to) + lift, drop);
            }
        });
    }

    if (mHasMark)
    {
        const LLVector3 m = gAgent.getPosAgentFromGlobal(mMark);
        const LLColor4 pink(1.f, 0.2f, 1.f, 0.95f);
        geom.edge(m - LLVector3(0.f, 0.f, 5.f), m + LLVector3(0.f, 0.f, 5.f), pink);
        geom.edge(m - LLVector3(1.f, 0.f, 0.f), m + LLVector3(1.f, 0.f, 0.f), pink);
        geom.edge(m - LLVector3(0.f, 1.f, 0.f), m + LLVector3(0.f, 1.f, 0.f), pink);
    }

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
    if (geom.empty())
    {
        return;
    }

    SKNavOverlayGeom::bindShader();
    {
        LLGLEnable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        {
            LLGLDepthTest depth(GL_TRUE, opaque ? GL_TRUE : GL_FALSE);
            geom.draw();
        }
        if (xray && !opaque)
        {
            // What is behind geometry, darker and fainter, so it reads as being
            // seen through the wall rather than in front of it.
            LLGLDepthTest behind(GL_TRUE, GL_FALSE, GL_GREATER);
            geom.draw(0.55f, 0.45f);
        }
    }
    SKNavOverlayGeom::unbindShader();
}
