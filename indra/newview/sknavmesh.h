/**
 * @file sknavmesh.h
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

#ifndef SK_NAVMESH_H
#define SK_NAVMESH_H

#include "llsingleton.h"
#include "sknavbuild.h"
#include "sknavtypes.h"
#include "skworldcensus.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class LLViewerRegion;

/// Main thread only. Every mesh has its own F64 origin on the global 32 m
/// lattice; tile indices are relative to it and may be negative when a
/// cluster grows towards the origin. Tiles rebuild when their signature (member
/// records, terrain patches, class parameters) moves, and never otherwise.
class SKNavMesh : public LLSingleton<SKNavMesh>
{
    LLSINGLETON(SKNavMesh);
    ~SKNavMesh() override;

public:
    /// Once per frame after the census.
    void update();

    /// Polygon edges of the avatar class near the agent, from the debug pass.
    void renderDebug();

    /// The floor under a global position within the class's reach: the nearest
    /// walkable polygon's height. Cluster meshes at the position first, then
    /// the region's terrain.
    bool floorUnder(const LLVector3d& pos, sknav::AgentClass cls, F32& z_out, LLUUID* mesh_out = nullptr) const;

    /// A path within one mesh (the one under a). Points are global; partial
    /// when b was not reachable and the path ends at the closest point found.
    bool findPath(const LLVector3d& a, const LLVector3d& b, sknav::AgentClass cls, std::vector<LLVector3d>& out, bool& partial) const;

    std::string statsLine() const;

    struct Stats
    {
        U32 mMeshes = 0;
        U32 mTiles = 0;
        U32 mTilesBuilt = 0;
        U32 mQueued = 0;
        U32 mInFlight = 0;
        U32 mPolys = 0;
        U32 mBuilds = 0;
        U32 mFailures = 0;
        F32 mLastBuildMs = 0.f;
        F32 mWorstBuildMs = 0.f;
    };
    const Stats& stats() const { return mStats; }

private:
    struct Tile
    {
        U64 mSignature = 0;     ///< what the tile should be built from
        U64 mBuilt = 0;         ///< what it was built from; 0 = nothing published
        bool mInFlight = false;
        S32 mPolys[sknav::NUM_CLASSES] = { 0, 0 };
        F32 mMillis = 0.f;
        LLVector3d mMin;        ///< global, for scheduling by distance and drawing
        LLVector3d mMax;
    };

    struct Mesh
    {
        LLUUID mId;                         ///< cluster id, or the region id for terrain
        bool mTerrain = false;
        U64 mRegion = 0;                    ///< the region that owns it, for lifetime
        U32 mGeneration = 0;                ///< results carry it; a torn-down mesh rejects old ones
        LLVector3d mOrigin;                 ///< global, on the lattice
        dtNavMesh* mNav[sknav::NUM_CLASSES] = { nullptr, nullptr };
        dtNavMeshQuery* mQuery[sknav::NUM_CLASSES] = { nullptr, nullptr };
        std::map<sknav::TileKey, Tile> mTiles;
        F32 mLastSeen = 0.f;
        ~Mesh();
    };

    bool wanted() const;
    void reconcile(const SKCensusSnapshotPtr& snap);
    void reconcileTerrain();
    void schedule(const SKCensusSnapshotPtr& snap);
    bool launch(Mesh& mesh, const sknav::TileKey& key, Tile& tile, const sknav::ClusterPtr& cluster);
    void publish(const LLUUID& mesh_id, U32 generation, const sknav::TileKey& key, U64 signature, std::shared_ptr<sknav::TileBuildResult> result);
    void fillTerrain(const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad, sknav::TerrainWindow& out) const;
    U64 terrainSignature(const LLVector3d& tile_min, const LLVector3d& tile_max) const;
    U64 paramsSignature() const;
    void fillParams(sknav::TileBuildInput& input) const;
    bool ensureNav(Mesh& mesh, S32 cls);
    Mesh* meshAt(const LLVector3d& pos, sknav::AgentClass cls, F32& z_out) const;

    std::map<LLUUID, std::unique_ptr<Mesh>> mMeshes;
    U64 mCensusStamp = 0;
    U32 mNextGeneration = 1;
    S32 mInFlight = 0;
    Stats mStats;
    F32 mLastLog = 0.f;
    bool mDemanding = false;
};

#endif // SK_NAVMESH_H
