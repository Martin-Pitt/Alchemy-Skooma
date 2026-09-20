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

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"
#include "workqueue.h"

#include <algorithm>
#include <cmath>

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
    static LLCachedControl<F32> slope(gSavedSettings, "SKNavSlope", 45.f);
    static LLCachedControl<F32> weatherHeight(gSavedSettings, "SKNavWeatherHeight", 0.5f);
    static LLCachedControl<bool> weather(gSavedSettings, "SKNavBuildWeather", false);
    static LLCachedControl<bool> walls(gSavedSettings, "SKNavBuildWalls", true);
    static LLCachedControl<F32> dropAvatar(gSavedSettings, "SKNavDropMax", 10.f);
    static LLCachedControl<F32> dropWeather(gSavedSettings, "SKNavWeatherDropMax", 24.f);
    static LLCachedControl<F32> dropSpacing(gSavedSettings, "SKNavDropSpacing", 2.f);

    input.mCell = llclamp(static_cast<F32>(cell), 0.05f, 1.f);
    // The avatar class is as tall as the user's own avatar, rounded up to the
    // next cell, so a tall avatar never gets floor it cannot stand on.
    F32 height = 2.f;
    if (isAgentAvatarValid())
    {
        height = llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f);
    }
    height = std::ceil(height / input.mCell) * input.mCell;

    input.mClasses[static_cast<S32>(AgentClass::AVATAR)] = true;
    input.mParams[static_cast<S32>(AgentClass::AVATAR)] = { height, radius, climb, slope };
    input.mClasses[static_cast<S32>(AgentClass::WEATHER)] = weather;
    input.mParams[static_cast<S32>(AgentClass::WEATHER)] = { weatherHeight, 0.f, climb, slope };

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

void SKNavMesh::reconcile(const SKCensusSnapshotPtr& snap)
{
    const U64 params = paramsSignature();
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = padFor(probe);
    for (const ClusterPtr& c : snap->mClusters.mClusters)
    {
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
        // The cluster's tiles are on the global lattice; ours are relative to
        // the mesh origin, which is on it too.
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
            // The build takes every record within the pad, so the signature
            // covers that same set rather than the cluster's unpadded one.
            const LLVector3d pmin = tile.mMin - LLVector3d(pad, pad, 0), pmax = tile.mMax + LLVector3d(pad, pad, 0);
            U64 members = SEED;
            for (const RecordPtr& r : c->mRecords)
            {
                if (overlapsXY(r->mMin, r->mMax, pmin, pmax))
                {
                    members = mix(members, r->mSignature);
                }
            }
            tile.mSignature = mix(mix(members, terrainSignature(tile.mMin, tile.mMax, pad)), params);
        }
        // Tiles the cluster no longer covers leave the Detour meshes.
        for (const auto& old : mesh->mTiles)
        {
            if (wanted.count(old.first) == 0 && old.second.mBuilt)
            {
                for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
                {
                    if (mesh->mNav[cls])
                    {
                        mesh->mNav[cls]->removeTile(mesh->mNav[cls]->getTileRefAt(old.first.mY, old.first.mX, 0), nullptr, nullptr);
                    }
                }
            }
        }
        mesh->mTiles.swap(wanted);
    }
}

void SKNavMesh::reconcileTerrain()
{
    const U64 params = paramsSignature();
    TileBuildInput probe;
    fillParams(probe);
    const F32 pad = padFor(probe);
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
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
        const S32 per_edge = static_cast<S32>(std::ceil(region->getWidth() / TILE_M));
        const LLSurface& land = region->getLand();
        for (S32 y = 0; y < per_edge; ++y)
        {
            for (S32 x = 0; x < per_edge; ++x)
            {
                const TileKey key{ x, y };
                Tile& tile = mesh->mTiles[key];
                tile.mMin = tileMin(mesh->mOrigin, key);
                tile.mMax = tile.mMin + LLVector3d(TILE_M, TILE_M, 0.0);
                tile.mMin.mdV[VZ] = land.getMinZ();
                tile.mMax.mdV[VZ] = land.getMaxZ();
                tile.mSignature = mix(terrainSignature(tile.mMin, tile.mMax, pad), params);
            }
        }
    }
}

bool SKNavMesh::launch(Mesh& mesh, const TileKey& key, Tile& tile, const ClusterPtr& cluster)
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
    fillTerrain(mesh.mOrigin, tile.mMin, tile.mMax, pad, input->mTerrain);

    F32 zmin = 1e9f, zmax = -1e9f;
    for (F32 z : input->mTerrain.mZ)
    {
        if (z > -900.f)
        {
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
        }
    }
    if (cluster)
    {
        const LLVector3d pmin = tile.mMin - LLVector3d(pad, pad, 0), pmax = tile.mMax + LLVector3d(pad, pad, 0);
        for (const RecordPtr& r : cluster->mRecords)
        {
            if (overlapsXY(r->mMin, r->mMax, pmin, pmax))
            {
                input->mRecords.push_back(r);
                zmin = std::min(zmin, static_cast<F32>(r->mMin.mdV[VZ] - mesh.mOrigin.mdV[VZ]));
                zmax = std::max(zmax, static_cast<F32>(r->mMax.mdV[VZ] - mesh.mOrigin.mdV[VZ]));
            }
        }
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
}

void SKNavMesh::schedule(const SKCensusSnapshotPtr& snap)
{
    static LLCachedControl<S32> perFrame(gSavedSettings, "SKNavBuildsPerFrame", 2);
    static LLCachedControl<S32> inFlightMax(gSavedSettings, "SKNavBuildsInFlight", 2);

    struct Candidate
    {
        Mesh* mMesh;
        TileKey mKey;
        Tile* mTile;
        ClusterPtr mCluster;
        F64 mDistance;
    };
    std::vector<Candidate> candidates;
    const LLVector3d agent = gAgent.getPositionGlobal();
    for (auto& entry : mMeshes)
    {
        Mesh& mesh = *entry.second;
        ClusterPtr cluster;
        if (!mesh.mTerrain)
        {
            for (const ClusterPtr& c : snap->mClusters.mClusters)
            {
                if (c->mId == mesh.mId)
                {
                    cluster = c;
                    break;
                }
            }
            if (!cluster)
            {
                continue;
            }
        }
        for (auto& t : mesh.mTiles)
        {
            Tile& tile = t.second;
            if (tile.mInFlight || tile.mSignature == tile.mBuilt)
            {
                continue;
            }
            const LLVector3d centre = (tile.mMin + tile.mMax) * 0.5;
            candidates.push_back({ &mesh, t.first, &tile, cluster, dist_vec_squared(LLVector3d(centre.mdV[VX], centre.mdV[VY], agent.mdV[VZ]), agent) });
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
        if (launch(*c.mMesh, c.mKey, *c.mTile, c.mCluster))
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
    if (snap->mStamp != mCensusStamp)
    {
        mCensusStamp = snap->mStamp;
        reconcile(snap);
    }
    // Terrain follows the land patches, which change without the census noticing.
    static F32 lastTerrain = -1.f;
    if (gFrameTimeSeconds - lastTerrain > 1.f)
    {
        lastTerrain = gFrameTimeSeconds;
        reconcileTerrain();
    }

    // Meshes nothing has claimed for a while are gone with their structure.
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
    {
        it = (gFrameTimeSeconds - it->second->mLastSeen > 30.f) ? mMeshes.erase(it) : std::next(it);
    }

    schedule(snap);

    mStats.mMeshes = static_cast<U32>(mMeshes.size());
    mStats.mTiles = 0;
    mStats.mTilesBuilt = 0;
    mStats.mPolys = 0;
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
    return llformat("navmesh: %u meshes, %u/%u tiles built (%u queued, %u in flight), %u avatar polys, %u faces, %u segments, %u links, %u builds, %u failures, last %.1f ms, worst %.1f ms",
                    mStats.mMeshes, mStats.mTilesBuilt, mStats.mTiles, mStats.mQueued, mStats.mInFlight, mStats.mPolys,
                    mStats.mFaces, mStats.mSegments, mStats.mLinks, mStats.mBuilds,
                    mStats.mFailures, mStats.mLastBuildMs, mStats.mWorstBuildMs);
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
            fn(mesh.mOrigin + LLVector3d(seg.mA), mesh.mOrigin + LLVector3d(seg.mB), seg.mKind, seg.mValue);
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
            fn(mesh.mOrigin + LLVector3d(link.mFrom), mesh.mOrigin + LLVector3d(link.mTo), link.mHeight);
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
    F32 bestDot = 0.f;
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
        const F32 ahead = to * heading;
        if (ahead > 0.5f && ahead > bestDot)
        {
            bestDot = ahead;
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

void SKNavMesh::renderDebug()
{
    static LLCachedControl<F32> range(gSavedSettings, "SKNavOverlayRange", 96.f);
    const S32 cls = static_cast<S32>(AgentClass::AVATAR);
    const LLVector3d agent = gAgent.getPositionGlobal();

    gGL.flush();
    gDebugProgram.bind();
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTextureSlot(0)->unbind();

    for (const auto& entry : mMeshes)
    {
        const Mesh& mesh = *entry.second;
        const dtNavMesh* nav = mesh.mNav[cls];
        if (!nav)
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
            const LLVector3d centre = (tmin + tmax) * 0.5;
            if (dist_vec(LLVector3d(centre.mdV[VX], centre.mdV[VY], agent.mdV[VZ]), agent) > range)
            {
                continue;
            }
            for (U8 area = 1; area <= 3; ++area)
            {
                bool any = false;
                for (int p = 0; p < tile->header->polyCount && !any; ++p)
                {
                    any = areaOf(tile->polys[p].flags) == area;
                }
                if (!any)
                {
                    continue;
                }
                switch (area)
                {
                    case AREA_GROUND: gGL.diffuseColor4f(0.3f, 0.9f, 0.3f, 0.8f); break;
                    case AREA_OBJECT: gGL.diffuseColor4f(0.3f, 0.6f, 1.f, 0.8f); break;
                    default: gGL.diffuseColor4f(0.2f, 0.4f, 0.9f, 0.5f); break;
                }
                gGL.begin(LLRender::LINES);
                for (int p = 0; p < tile->header->polyCount; ++p)
                {
                    const dtPoly& poly = tile->polys[p];
                    if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || areaOf(poly.flags) != area)
                    {
                        continue;
                    }
                    const dtPolyDetail& detail = tile->detailMeshes[p];
                    for (unsigned int t = 0; t < detail.triCount; ++t)
                    {
                        const unsigned char* tri = &tile->detailTris[(detail.triBase + t) * 4];
                        LLVector3 v[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            const F32* rc = tri[k] < poly.vertCount ? &tile->verts[poly.verts[tri[k]] * 3] : &tile->detailVerts[(detail.vertBase + tri[k] - poly.vertCount) * 3];
                            v[k] = originAgent + fromRecast(rc) + LLVector3(0.f, 0.f, 0.05f);
                        }
                        for (int k = 0; k < 3; ++k)
                        {
                            gGL.vertex3fv(v[k].mV);
                            gGL.vertex3fv(v[(k + 1) % 3].mV);
                        }
                    }
                }
                gGL.end();
            }
        }
    }

    // Walls: the floor edges by what lies beyond them, cover coloured against
    // the user's own eye height, and a tick from each ledge to where it lands.
    static LLCachedControl<bool> show_walls(gSavedSettings, "SKNavOverlayWalls", true);
    if (show_walls)
    {
        static LLCachedControl<F32> climb(gSavedSettings, "SKNavClimb", 0.75f);
        static LLCachedControl<F32> peek(gSavedSettings, "SKNavCoverPeek", 0.15f);
        const F32 eye = isAgentAvatarValid() ? llclamp(gAgentAvatarp->mBodySize.mV[VZ], 1.f, 3.f) : 1.8f;
        const LLVector3d box(range, range, range);
        gGL.setLineWidth(2.f);
        gGL.begin(LLRender::LINES);
        segmentsIn(agent - box, agent + box, AgentClass::AVATAR, [&](const LLVector3d& a, const LLVector3d& b, EdgeKind kind, F32 value) {
            switch (kind)
            {
                case EdgeKind::WALL:
                    switch (coverClass(value, eye, climb, peek))
                    {
                        case Cover::FULL: gGL.diffuseColor4f(1.f, 1.f, 1.f, 0.9f); break;
                        case Cover::HALF: gGL.diffuseColor4f(1.f, 0.8f, 0.2f, 0.9f); break;
                        default: gGL.diffuseColor4f(0.6f, 0.6f, 0.6f, 0.7f); break;
                    }
                    break;
                case EdgeKind::LEDGE: gGL.diffuseColor4f(1.f, 0.3f, 0.3f, 0.9f); break;
                case EdgeKind::OVERHANG: gGL.diffuseColor4f(0.7f, 0.3f, 1.f, 0.8f); break;
                default: return;
            }
            const LLVector3 pa = gAgent.getPosAgentFromGlobal(a) + LLVector3(0.f, 0.f, 0.08f);
            const LLVector3 pb = gAgent.getPosAgentFromGlobal(b) + LLVector3(0.f, 0.f, 0.08f);
            gGL.vertex3fv(pa.mV);
            gGL.vertex3fv(pb.mV);
            // A wall is drawn again at its own height, so how tall it is shows.
            if (kind == EdgeKind::WALL)
            {
                const LLVector3 up(0.f, 0.f, value);
                gGL.vertex3fv((pa + up).mV);
                gGL.vertex3fv((pb + up).mV);
            }
        });
        gGL.diffuseColor4f(0.2f, 1.f, 1.f, 0.9f);
        linksIn(agent - box, agent + box, AgentClass::AVATAR, [&](const LLVector3d& from, const LLVector3d& to, F32) {
            const LLVector3 pf = gAgent.getPosAgentFromGlobal(from) + LLVector3(0.f, 0.f, 0.08f);
            const LLVector3 pt = gAgent.getPosAgentFromGlobal(to) + LLVector3(0.f, 0.f, 0.08f);
            gGL.vertex3fv(pf.mV);
            gGL.vertex3fv(pt.mV);
        });
        gGL.end();
        gGL.setLineWidth(1.f);
    }

    gGL.flush();
    gDebugProgram.unbind();
}
