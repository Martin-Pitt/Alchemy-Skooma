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
#include "threadpool.h"

#include <functional>
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

    /// Close the build pool before the singleton goes: a job in flight holds
    /// this pointer to publish its result.
    void cleanupSingleton() override;

    /// Polygon edges of the avatar class near the agent, from the debug pass.
    void renderDebug();

    /// The floor under a global position within the class's reach: the nearest
    /// walkable polygon's height. Cluster meshes at the position first, then
    /// the region's terrain.
    bool floorUnder(const LLVector3d& pos, sknav::AgentClass cls, F32& z_out, LLUUID* mesh_out = nullptr) const;

    /// A path within one mesh (the one under a). Points are global; partial
    /// when b was not reachable and the path ends at the closest point found.
    bool findPath(const LLVector3d& a, const LLVector3d& b, sknav::AgentClass cls, std::vector<LLVector3d>& out, bool& partial) const;

    /// Walls (sknavwalls.h). Positions handed to the callback are global.
    /// Wall faces are what Atmo paints and drains along; segments are the
    /// floor edges the map draws; links are where a floor can be left.
    typedef std::function<void(const LLVector3d& min, const LLVector3d& max, U8 side, U8 area)> WallFaceFn;
    typedef std::function<void(const LLVector3d& a, const LLVector3d& b, sknav::EdgeKind kind, F32 value)> SegmentFn;
    typedef std::function<void(const LLVector3d& from, const LLVector3d& to, F32 height)> LinkFn;
    void wallFacesIn(const LLVector3d& min, const LLVector3d& max, const WallFaceFn& fn) const;
    void segmentsIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const SegmentFn& fn) const;
    void linksIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const LinkFn& fn) const;

    /// The cover a segment ahead of this position gives someone of that eye
    /// height, and how tall it is. False when nothing is within reach.
    bool coverAt(const LLVector3d& pos, const LLVector3& dir, sknav::AgentClass cls, F32 eye_height,
                 sknav::Cover& cover_out, F32& height_out, F32 reach = 1.f) const;

    std::string statsLine() const;

    /// Cast from the middle of the screen, mark what it hits, and write down
    /// everything the census and the navmesh know there. Returns a line for
    /// the console; the report itself goes to the log under SKNavDump.
    std::string markUnderCamera();

    struct Stats
    {
        U32 mMeshes = 0;
        U32 mTiles = 0;
        U32 mTilesBuilt = 0;
        U32 mQueued = 0;
        U32 mInFlight = 0;
        U32 mBuilds = 0;
        U32 mFailures = 0;
        U32 mFaces = 0;
        U32 mSegments = 0;
        U32 mLinks = 0;
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
        /// What the build rasterises here, gathered when the tile's signature
        /// is worked out rather than walked again on the way to the worker.
        std::vector<sknav::RecordPtr> mRecords;
        /// The terrain window the tile is built from, sampled on the main
        /// thread under a budget and reused until the land under the tile
        /// moves (mLandStamp). mTerrainStale means the window is not the
        /// current land yet, so the tile waits rather than build on old ground.
        std::shared_ptr<const sknav::TerrainWindow> mTerrain;
        U64 mTerrainSignature = 0;
        U64 mLandStamp = 0;
        U64 mWorldStamp = 0;
        bool mTerrainStale = false;
        /// False for a tile past the region's own edge: the land there is
        /// another region's, and that region meshes it, so this tile carries
        /// only the records that reach out of the region (a surround hanging
        /// over the edge) and no ground at all.
        bool mOverLand = true;
        /// Published with the tile, mesh-local; shared so a read hands them
        /// out without copying and a rebuild never rewrites one in place.
        std::shared_ptr<const std::vector<sknav::WallFace>> mFaces;
        std::shared_ptr<const std::vector<sknav::WallSegment>> mSegments[sknav::NUM_CLASSES];
        std::shared_ptr<const std::vector<sknav::DropLink>> mLinks[sknav::NUM_CLASSES];
    };

    struct Mesh
    {
        LLUUID mId;                         ///< cluster id, or the region id for terrain
        /// The region's own mesh: its terrain and everything standing on it.
        /// A cluster only gets a mesh of its own when it sits clear of the
        /// ground, and then it has no terrain in it at all.
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
    /// The pool the tile builds run on, created on first use. A pool of its
    /// own, so the builds neither queue behind nor starve the viewer's shared
    /// work: they are pure and independent, one per core.
    LL::WorkQueue::ptr_t workerQueue();
    void reconcile(const SKCensusSnapshotPtr& snap);
    /// Whether a cluster stands on the ground under it, within
    /// SKNavGroundedMargin. A grounded structure is meshed with the region so
    /// that one mesh holds the ground, the walls that block it and the floors
    /// that lead off it; anything clear of the ground gets a mesh to itself.
    bool grounded(const sknav::Cluster& cluster) const;
    static U64 recordSignature(const std::vector<sknav::RecordPtr>& records);
    /// Tiles the mesh no longer wants leave its Detour meshes.
    static void dropTiles(Mesh& mesh, const std::map<sknav::TileKey, Tile>& wanted);
    void schedule();
    bool launch(Mesh& mesh, const sknav::TileKey& key, Tile& tile);
    void publish(const LLUUID& mesh_id, U32 generation, const sknav::TileKey& key, U64 signature, std::shared_ptr<sknav::TileBuildResult> result);
    void fillTerrain(const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad, sknav::TerrainWindow& out) const;
    U64 terrainSignature(const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad) const;
    /// What the region's land looks like now, over all its patches. A tile's
    /// cached terrain signature stands until this moves.
    U64 landStamp(LLViewerRegion& region) const;
    /// Which regions there are, for the same purpose.
    U64 worldStamp() const;
    /// Sample the terrain window of the tiles whose land moved, under
    /// SKNavTerrainBudgetMS, so a first build-out never stalls a frame.
    void sampleTerrain();
    U64 paramsSignature() const;
    void fillParams(sknav::TileBuildInput& input) const;
    bool ensureNav(Mesh& mesh, S32 cls);
    Mesh* meshAt(const LLVector3d& pos, sknav::AgentClass cls, F32& z_out) const;
    /// Every published tile whose footprint meets the box, with its mesh.
    void forEachTileIn(const LLVector3d& min, const LLVector3d& max, const std::function<void(const Mesh&, const Tile&)>& fn) const;

    std::map<LLUUID, std::unique_ptr<Mesh>> mMeshes;
    std::unique_ptr<LL::ThreadPool> mWorkers;
    U64 mCensusStamp = 0;
    U32 mNextGeneration = 1;
    S32 mInFlight = 0;
    Stats mStats;
    F32 mLastLog = 0.f;
    bool mDemanding = false;
    LLVector3d mMark;
    bool mHasMark = false;
};

#endif // SK_NAVMESH_H
