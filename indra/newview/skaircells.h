/**
 * @file skaircells.h
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

#ifndef SK_AIRCELLS_H
#define SK_AIRCELLS_H

#include "llsingleton.h"
#include "skaircensus.h"
#include "skairlayout.h"
#include "skairraster.h"
#include "skairskymap.h"
#include "skcensuslattice.h"
#include "skworldcensus.h"
#include "skworldoverlay.h"
#include "threadpool.h"

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

class LLViewerRegion;

/// Main thread only. Every mesh has its own F64 origin on the global 32 m
/// lattice; tile indices are relative to it and may be negative when a
/// cluster grows towards the origin. A tile builds again when its signature
/// (member records, terrain patches, parameters) moves, and never otherwise.
/// Tiles near the camera and the agent build their detail air; the rest read
/// coarse air, and the air budget takes detail back from the least near.
class SKAirCells : public LLSingleton<SKAirCells>
{
    LLSINGLETON(SKAirCells);
    ~SKAirCells() override;

public:
    /// Once per frame after the census.
    void update();

    /// Close the build pool and drop what is queued on it: a job in flight
    /// holds this pointer to publish its result.
    void cleanupSingleton() override;

    /// The air overlays (SKAirOverlay*), under the pipeline's world debug mask.
    void renderDebug();

    /// SKAirCellsEnabled.
    static bool enabled();
    /// Whether an air overlay is drawn: the world debug mask and one of the air's own switches.
    static bool overlayActive();

    /// Keep the air building while a consumer is registered, whatever SKAirAlways says.
    void addDemand(const std::string& who);
    void removeDemand(const std::string& who);
    /// Treat a global box like the camera's SKAirRadius: its tiles build and keep detail air while asked for
    /// within SKAirRequestHoldSeconds. One box per consumer.
    void requestAir(const std::string& who, const LLVector3d& min, const LLVector3d& max) const;

    /// Moves whenever the published air changes: a tile publishes, fails or
    /// leaves, a mesh is torn down, or air is evicted.
    U64 revision() const { return mRevision; }

    /// What every tile is built with now, records and land left empty.
    skworld::TileInput params() const;
    /// The pool the tile builds run on, created on first use. Consumers of the
    /// air (rooms, probes) may post their own pure jobs to it.
    LL::WorkQueue::ptr_t workerQueue();

    /// One mesh as a consumer walks them.
    struct MeshInfo
    {
        LLUUID mId;                     ///< the cluster's, or the region's for its own mesh
        bool mTerrain = false;
        U64 mRegion = 0;                ///< the region handle it belongs to
        LLVector3d mOrigin;             ///< global, on the lattice
        U32 mGeneration = 0;            ///< moves when the mesh is torn down and made again
        U8 mPriority = 3;               ///< meshPriority
    };
    void snapshotMeshes(std::vector<MeshInfo>& out) const;

    /// What moved a mesh's air since a consumer last drained the list, so it walks only those meshes.
    struct AirChange
    {
        enum Kind : U8 { PUBLISHED, EVICTED, DROPPED, MESH_GONE };
        LLUUID mMesh;
        skworld::TileKey mKey;
        U64 mAirId = 0;
        LLVector3d mMin, mMax;
        Kind mKind = PUBLISHED;
        bool mWholeMesh = true;     ///< no one tile: mKey, mAirId and the box say nothing
    };
    /// Moves the list into out; false when it overflowed since the last drain, so any mesh may have moved.
    bool drainChanges(std::vector<AirChange>& out) const;

    /// A mesh's air by value, so a pass on a worker reads no tile of the driver.
    struct AirSnapshotTile
    {
        skworld::TileKey mKey;
        skworld::TileAirPtr mAir;
        skworld::TileAirPtr mCoarse;
        U64 mAirId = 0;
        U64 mContributor = 0;
        LLVector3d mMin, mMax;
    };
    struct AirSnapshot
    {
        bool mFound = false;
        LLVector3d mOrigin;
        U8 mPriority = 3;
        std::vector<AirSnapshotTile> mTiles;
    };
    AirSnapshot airSnapshot(const LLUUID& mesh) const;

    /// Which mesh the work queues serve first: 0 the agent's region's own
    /// terrain, 1 the terrain of the regions touching it, 2 the clusters
    /// owned by either, 3 everything else (and a mesh that is not there).
    U8 meshPriority(const LLUUID& mesh) const;

    /// The content keys of a mesh's tiles whose footprint meets the global
    /// box, 0 for a tile not built yet (skairlayout.h tileContentKey).
    void tileContentKeysIn(const LLUUID& mesh, const LLVector3d& min, const LLVector3d& max, std::vector<U64>& out) const;
    /// Whether a tile meeting the global box is queued or building.
    bool settlingIn(const LLVector3d& min, const LLVector3d& max) const;

    /// Wall faces (skairwalls.h) meeting the box, global.
    typedef std::function<void(const LLVector3d& min, const LLVector3d& max, U8 side, U8 area)> WallFaceFn;
    void wallFacesIn(const LLVector3d& min, const LLVector3d& max, const WallFaceFn& fn) const;
    /// The air's floor edges over a drop, and the run-off down them, global.
    typedef std::function<void(const LLVector3d& a, const LLVector3d& b, skworld::EdgeKind kind, F32 value)> LedgeFn;
    typedef std::function<void(const LLVector3d& a, const LLVector3d& b, F32 drop_a, F32 drop_b, U8 side)> RunoffFn;
    void runoffIn(const LLVector3d& min, const LLVector3d& max, const LedgeFn& ledge_fn, const RunoffFn& runoff_fn) const;

    /// No solid of any published tile's air starts above pos within reach:
    /// the sky map, which forgets a roof as soon as its tile does.
    bool skyOpenAt(const LLVector3d& pos, F32 reach = 50.f) const { return mSkyMap.skyOpenAt(pos, reach); }
    const skworld::SkyMap& skyMap() const { return mSkyMap; }
    /// The key a tile's air gives the sky map under.
    static U64 skyContributor(const LLUUID& mesh, const skworld::TileKey& key) { return skworld::skyContributor(mesh, key); }

    /// One line for a console: meshes, tiles built, queued and in flight, the last raster, air bytes and evictions.
    std::string statsLine() const;

    struct Stats
    {
        U32 mMeshes = 0;
        U32 mTiles = 0;
        U32 mTilesBuilt = 0;
        U32 mQueued = 0;
        U32 mInFlight = 0;
        U32 mBuilds = 0;
        U32 mFailures = 0;
        U32 mRefused = 0;           ///< layouts over MAX_TILES_PER_MESH, left unbuilt
        U32 mFaces = 0;
        U32 mAirTiles = 0;
        size_t mAirBytes = 0;
        U32 mAirEvicted = 0;        ///< tiles whose detail air went for the budget
        F32 mLastBuildMs = 0.f;
        F32 mWorstBuildMs = 0.f;
        F32 mExcludeMs = 0.f;       ///< of the last build that left records out of AVATAR, what that cost
        U32 mAirOverlayVerts = 0;   ///< vertices the air overlay last gathered
        U32 mAirOverlayTiles = 0;   ///< tiles it drew from
        U32 mAirOverlayCut = 0;     ///< tiles the vertex budget cut short or left out
    };
    const Stats& stats() const { return mStats; }

private:
    /// Why a tile needed building, for the update blip. A record beats the
    /// land and both beat the parameters, because a signature that moved with
    /// all three moved for the most specific reason.
    enum TileCause : U8
    {
        CAUSE_NONE = 0,     ///< nothing to say: no cause was recorded
        CAUSE_CREATED,      ///< the tile was never built
        CAUSE_RECORDS,      ///< a record appeared, left or changed
        CAUSE_TERRAIN,      ///< the land under the tile moved
        CAUSE_PARAMS,       ///< the parameters moved
        CAUSE_FAILED,       ///< the last build of it failed
    };

    struct Tile
    {
        U64 mSignature = 0;     ///< what the tile should be built from
        U64 mBuilt = 0;         ///< what it was built from; 0 = nothing published
        bool mInFlight = false;
        F32 mMillis = 0.f;
        LLVector3d mMin;        ///< global, for scheduling by distance and drawing
        LLVector3d mMax;
        /// Why mSignature last moved away from what was built, and where what caused it sits (global).
        U8 mCause = CAUSE_NONE;
        LLVector3d mCauseAt;
        bool mHasCauseAt = false;
        /// What the build rasterises here, gathered when the signature is worked out.
        std::vector<skworld::RecordPtr> mRecords;
        /// The terrain window, sampled on the main thread under a budget and
        /// reused until the land under the tile moves (mLandStamp). While
        /// mTerrainStale the tile waits rather than build on old ground.
        std::shared_ptr<const skworld::TerrainWindow> mTerrain;
        U64 mTerrainSignature = 0;
        U64 mRecordSignature = 0;
        U64 mLandStamp = 0;
        U64 mWorldStamp = 0;
        bool mTerrainStale = false;
        /// False for a tile past the region's own edge: that region builds the
        /// land there, so this tile carries only the records reaching out.
        bool mOverLand = true;
        /// Published with the tile, mesh-local; shared so a read hands them out without copying.
        std::shared_ptr<const std::vector<skworld::WallFace>> mFaces;
        std::shared_ptr<const std::vector<skworld::WallSegment>> mLedges;
        std::shared_ptr<const std::vector<skworld::RunoffSegment>> mRunoff;
        skworld::TileAirPtr mAir;           ///< detail within the air radius, coarse past it, until evicted
        skworld::TileAirPtr mAirCoarse;     ///< the solid tops an evicted tile keeps for the sky map
        U64 mContentKey = 0;                ///< tileContentKey of what was published
        F64 mNearness = 0.0;                ///< camera nearness over time, for the air budget
        U64 mAirBuilt = 0;                  ///< the signature the detail air was built from; 0 = none
        U64 mAirId = 0;                     ///< names mAir for consumers that key on it
    };

    struct Layout
    {
        std::vector<skworld::LaidTile> mTiles;
        std::vector<U64> mSignatures;
        std::set<skworld::TileKey> mKeys;
        size_t mNext = 0;
        bool mRefused = false;      ///< more tiles than a mesh holds, so none
        U64 mCensus = 0, mWorld = 0, mLand = 0, mParams = 0;
        F32 mPad = 0.f, mVoidMargin = 0.f;
        F64 mMinZ = 0., mMaxZ = 0.;
    };

    struct Mesh
    {
        LLUUID mId;                         ///< cluster id, or the region id for terrain
        /// The region's own mesh: its terrain and everything standing on it.
        /// A cluster gets a mesh of its own only when it sits clear of the ground.
        bool mTerrain = false;
        U64 mRegion = 0;                    ///< the region that owns it, for priority
        U32 mGeneration = 0;                ///< results carry it; a torn-down mesh rejects old ones
        LLVector3d mOrigin;                 ///< global, on the lattice
        std::map<skworld::TileKey, Tile> mTiles;
        F64 mLastSeen = 0.f;
        // What the layout was last laid against; while all stand, a new one would lay the same.
        U64 mLayoutCensus = 0;
        U64 mLayoutWorld = 0;
        U64 mLayoutLand = 0;
        U64 mLayoutParams = 0;
        F32 mLayoutVoidMargin = 0.f;
        std::future<std::shared_ptr<Layout>> mLayoutJob;
        std::shared_ptr<Layout> mPendingLayout;
        std::map<skworld::TileKey, Tile>::iterator mDropCursor;
        bool mDropping = false;
    };

    struct TileCursor
    {
        LLUUID mMesh;
        skworld::TileKey mKey;
        bool mStarted = false;
    };
    /// Visit tiles from the cursor for at most seconds; true once the walk wrapped.
    bool scanTiles(TileCursor& cursor, F32 seconds, const std::function<void(Mesh&, const skworld::TileKey&, Tile&)>& visit);
    TileCursor mTerrainCursor, mScheduleCursor, mStatsCursor, mEvictionCursor, mSkyCursor;

    struct AirCandidate
    {
        LLUUID mMesh;
        U32 mGeneration;
        skworld::TileKey mKey;
        skworld::TileAirPtr mAir, mCoarse;
        F64 mNearness, mDistance;
        U8 mPriority = 3;
    };
    std::shared_ptr<std::vector<AirCandidate>> mEvictionScan;
    std::future<std::shared_ptr<std::vector<AirCandidate>>> mEvictionJob;
    std::shared_ptr<std::vector<AirCandidate>> mPendingEvictions;
    size_t mNextEviction = 0;
    struct SkyContributor { LLVector3d mOrigin; skworld::TileTopsPtr mTops; };
    std::shared_ptr<std::map<U64, SkyContributor>> mSkyScan;
    U64 mSkyScanRevision = 0;
    bool mSkyScanReady = false, mEvictionScanReady = false;
    struct BuildCandidate
    {
        LLUUID mMesh;
        U32 mGeneration;
        skworld::TileKey mKey;
        F64 mDistance;
        U8 mPriority = 3;
    };
    std::shared_ptr<std::vector<BuildCandidate>> mScheduleScan, mBuildCandidates;
    std::future<std::shared_ptr<std::vector<BuildCandidate>>> mScheduleJob;
    bool mScheduleScanReady = false;
    size_t mNextCandidate = 0;
    F64 mCandidateTime = 0.f;
    Stats mScanningStats;

    bool wanted() const;
    /// What inAirRadius reads, taken once for a pass over many tiles.
    struct AirReach
    {
        LLVector3d mCamera;
        LLVector3d mAgent;
        F64 mRadius2 = 0.0;
        F64 mAgentRadius2 = 0.0;
        std::vector<std::pair<LLVector3d, LLVector3d>> mBoxes;
    };
    AirReach airReach() const;
    /// Whether a tile builds its detail air: near the camera or the agent, under the agent's cluster, or asked for.
    bool inAirRadius(const Tile& tile, const AirReach& reach) const;
    U8 meshPriority(const Mesh& mesh) const;
    void noteAirChange(AirChange::Kind kind, const LLUUID& mesh);
    void noteAirChange(AirChange::Kind kind, const Mesh& mesh, const skworld::TileKey& key, const Tile& tile);
    mutable std::vector<AirChange> mAirChanges;
    mutable bool mAirChangesLost = false;
    /// The plan boxes of the clusters around the agent, whose tiles all get detail air.
    std::vector<std::pair<LLVector3d, LLVector3d>> mHomeBoxes;
    struct AirRequest
    {
        LLVector3d mMin, mMax;
        F64 mAt = 0.0;
    };
    mutable std::map<std::string, AirRequest> mAirRequests;
    void findHomeBoxes(const SKCensusSnapshotPtr& snap);
    /// Bring the sky map in line with the published tiles' air.
    void syncSkyMap();
    void reconcile(const SKCensusSnapshotPtr& snap);
    void applyLayouts();
    /// Whether a cluster stands on the ground under it, within SKAirGroundedMargin.
    /// A grounded structure is built with its region; anything clear of the ground gets a mesh to itself.
    bool grounded(const skworld::Cluster& cluster) const;
    /// Work out what a tile is built from and note why it moved away from what was built.
    void reSign(Tile& tile, const std::vector<skworld::RecordPtr>& before, U64 records, U64 terrain, U64 params);
    /// Where the first record that was not here before sits, or the first that left.
    static bool changedRecord(const std::vector<skworld::RecordPtr>& before, const std::vector<skworld::RecordPtr>& after, LLVector3d& out);
    /// A rising blip for a tile that was just built, coloured by why.
    void tileBlip(const Mesh& mesh, const Tile& tile) const;
    /// Moves mRevision, telling the trace why.
    void bumpRevision(const char* why);
    void schedule();
    /// What a tile is built from, gathered on the main thread; false while its land is not sampled.
    bool prepareInput(const Mesh& mesh, const skworld::TileKey& key, const Tile& tile, std::shared_ptr<skworld::TileInput>& input,
                      std::shared_ptr<const std::vector<skworld::RecordPtr>>& records) const;
    bool launch(Mesh& mesh, const skworld::TileKey& key, Tile& tile);
    void publish(const LLUUID& mesh_id, U32 generation, const skworld::TileKey& key, U64 signature,
                 const std::shared_ptr<skworld::TileAirResult>& result, U64 content_key);
    /// Keep the tiles' detail air under SKAirBudgetMB, least recently near first.
    void evictAir();
    void fillTerrain(const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad,
                     skworld::TerrainWindow& out) const;
    U64 terrainSignature(const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad) const;
    /// The region's land over all its patches: a tile's terrain signature stands until this moves.
    U64 landStamp(LLViewerRegion& region) const;
    /// Which regions there are, for the same purpose.
    U64 worldStamp() const;
    /// Sample the terrain window of the tiles whose land moved, under SKAirTerrainBudgetMS.
    void sampleTerrain();
    U64 paramsSignature() const;
    void fillParams(skworld::TileInput& input) const;
    /// Every tile whose footprint meets the box, with its mesh.
    void forEachTileIn(const LLVector3d& min, const LLVector3d& max, const std::function<void(const Mesh&, const Tile&)>& fn) const;
    void gatherAirOverlay(const LLVector3d& camera, F64 reach, U32 mode, bool opaque);
    void gatherSkyOverlay(const LLVector3d& camera, F64 reach);
    void refreshOverlay();

    std::map<LLUUID, std::unique_ptr<Mesh>> mMeshes;
    std::unique_ptr<LL::ThreadPool> mWorkers;
    U64 mCensusStamp = 0;
    F64 mLastReconcile = -1.0;
    U32 mNextGeneration = 1;
    S32 mInFlight = 0;
    U64 mNextAirId = 0;
    Stats mStats;
    F32 mLastRasterMs = 0.f;
    F64 mLastLog = 0.f;
    bool mDemanding = false;
    skworld::SkyMap mSkyMap;
    std::map<U64, skworld::TileTopsPtr> mSkyTiles;     ///< what each contributor last gave the sky map
    U64 mSkyRevision = 0;
    struct SkyResult
    {
        skworld::SkyMap mMap;
        std::map<U64, skworld::TileTopsPtr> mTiles;
        U64 mRevision = 0;
    };
    std::future<std::shared_ptr<SkyResult>> mSkyJob;
    /// Consumers that keep the air building whatever SKAirAlways says.
    std::set<std::string> mConsumers;
    /// What the census file's "air.params" section writes and reads.
    skworld::AirCensusParams mCensusParams;
    bool mSectionRegistered = false;

    U64 mRevision = 0;
    /// The overlay as gathered: rebuilt when its key moves or the camera moves or turns past a threshold.
    SKWorldOverlayGeom mOverlay;
    U64 mOverlayKey = 0;
    bool mOverlayFresh = false;
    LLVector3 mOverlayCamera;
    LLVector3 mOverlayForward;
};

#endif // SK_AIRCELLS_H
