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
#include <array>
#include "sknavbuild.h"
#include "sknavexport.h"
#include "sknavlayout.h"
#include "sknavoverlay.h"
#include "sknavpolys.h"
#include "sknavresidency.h"
#include "sknavskymap.h"
#include "sknavspacefilter.h"
#include "sknavspaceproducts.h"
#include "sknavtilecache.h"
#include "sknavtypes.h"
#include "sknavwallruns.h"
#include "skworldcensus.h"
#include "threadpool.h"

#include <functional>
#include <future>
#include <iosfwd>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
struct dtMeshTile;
class LLViewerRegion;
class SKNavRooms;
class SKNavWorldView;

/// Main thread only. Every mesh has its own F64 origin on the global 32 m
/// lattice; tile indices are relative to it and may be negative when a
/// cluster grows towards the origin. Tiles rebuild when their signature (member
/// records, terrain patches, class parameters) moves, and never otherwise.
class SKNavRoomProbes;

class SKNavMesh : public LLSingleton<SKNavMesh>
{
    LLSINGLETON(SKNavMesh);
    ~SKNavMesh() override;

public:
    /// Once per frame after the census.
    void update();

    /// First reflection-probe placement pass, before the residual grid pass.
    void placeReflectionProbes();

    /// The content keys of a mesh's tiles whose footprint meets the global
    /// box, 0 for a tile not published under one.
    void tileContentKeysIn(const LLUUID& mesh, const LLVector3d& min, const LLVector3d& max, std::vector<U64>& out) const;
    /// A hash of what a mesh's map products (polys, storeys, wall runs) meeting the box are built from.
    U64 mapInputsKey(const LLUUID& mesh, const LLVector3d& min, const LLVector3d& max) const;
    /// Whether a terrain mesh has a published AVATAR tile meeting the box.
    bool terrainTilesIn(const LLUUID& mesh, const LLVector3d& min, const LLVector3d& max) const;
    /// Whether anything meeting the global box is still on its way: a tile
    /// queued or building, or a rooms pass not yet landed over it.
    bool settlingIn(const LLVector3d& min, const LLVector3d& max) const;
    /// Whether every tile of a mesh holds its detail air, none of it waiting on disk.
    bool detailResident(const LLUUID& mesh) const;
    /// Bring a mesh's detail air back from disk, and keep it for SKNavAirDetailHoldSeconds after the last ask.
    void requestDetail(const LLUUID& mesh) const;

    /// Close the build pool before the singleton goes: a job in flight holds
    /// this pointer to publish its result.
    void cleanupSingleton() override;

    /// Polygon edges of the avatar class near the agent, from the debug pass.
    void renderDebug();

    /// Keep the navmesh building while a consumer is registered (the mini-map while it is shown).
    void addDemand(const std::string& who);
    void removeDemand(const std::string& who);
    /// Moves whenever the navmesh's published state changes: a tile publishes, fails or leaves, a
    /// mesh is torn down, air is evicted (it follows the camera), or feature links change.
    U64 revision() const { return mRevision; }

    /// A poly edge on a portal: the poly and edge in the tile's polys, the
    /// portal's key, kind and the space on each side of it.
    struct PortalEdge
    {
        U16 mPoly = 0;                  ///< into PolyTile::mPolys
        U8 mEdge = 0;                   ///< from the poly's vertex mEdge to the next
        sknav::PortalKind mKind = sknav::PortalKind::OPENING;
        U64 mPortal = 0;                ///< portalKey (sknavstamp.h): the same portal across tiles and passes
        U32 mInside = 0;                ///< the poly's space id
        U32 mAcross = 0;                ///< the space id on the portal's other side
    };

    /// One published tile as the mini-map reads it.
    struct PolyTile
    {
        LLUUID mMesh;
        bool mTerrain = false;          ///< the region's own mesh
        U64 mRegion = 0;                ///< region handle
        LLVector3d mOrigin;             ///< mesh origin, global, on the 32 m lattice
        sknav::TileKey mKey;
        U64 mContentKey = 0;
        LLVector3d mMin, mMax;          ///< tile footprint, global
        std::shared_ptr<const sknav::TilePolys> mPolys;
        std::shared_ptr<const std::vector<sknav::WallSegment>> mSegments;
        std::shared_ptr<const std::vector<sknav::DropLink>> mLinks;
        std::shared_ptr<const std::vector<sknav::Flight>> mFlights;
        std::shared_ptr<const std::vector<U32>> mPolySpace;            ///< per poly of mPolys, its space id; 0 for none
        std::shared_ptr<const std::vector<PortalEdge>> mPortalEdges;   ///< its polys' edges on portals
        U64 mPolyRefBase = 0;           ///< Detour ref of poly 0; poly i is mPolyRefBase | i
    };
    typedef std::function<void(const PolyTile&)> PolyTileFn;
    /// Every published tile of the class whose footprint meets the box; polygons extracted once per publish.
    void polysIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const PolyTileFn& fn) const;
    /// A poly edge's neighbour across its tile's border, from Detour's external links as they stand now: the edge,
    /// the neighbour's ref and the stretch of the edge they share, as fractions from the edge's first vertex.
    typedef std::function<void(const LLUUID& mesh, const sknav::TileKey& key, U16 poly, U8 edge, U64 neighbour, F32 t0, F32 t1)> BorderLinkFn;
    void borderLinksIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const BorderLinkFn& fn) const;

    /// Whether anything the debug pass draws is switched on: the navmesh
    /// surface from the pipeline's debug mask, or any layer that reads the
    /// built mesh on its own - the walls, the wall faces, the census boxes.
    /// The pipeline calls the debug passes only when this is true, and the
    /// build follows it too, so a layer draws whether or not the surface is.
    static bool overlayActive();

    /// The floor under a global position within the class's reach: the nearest
    /// walkable polygon's height. Cluster meshes at the position first, then
    /// the region's terrain.
    bool floorUnder(const LLVector3d& pos, sknav::AgentClass cls, F32& z_out, LLUUID* mesh_out = nullptr) const;

    /// The space under a global position: the nearest walkable poly's, within
    /// the class's reach; false over no stamped poly.
    bool spaceAt(const LLVector3d& pos, LLUUID& mesh_out, U32& space_out) const;
    /// Every stamped poly of a space, its outline global.
    typedef std::function<void(const std::vector<LLVector3d>& outline)> OutlineFn;
    void spacePolys(const LLUUID& mesh, U32 space, const OutlineFn& fn) const;
    /// A mesh's storeys over its published polys, rebuilt after a publish; null for no mesh.
    /// Positions in it are relative to the mesh's origin.
    sknav::StoreyProductPtr storeys(const LLUUID& mesh) const;
    /// The storey under a global position, by index into storeys(mesh_out); false on a flight or off the mesh.
    bool storeyAt(const LLVector3d& pos, LLUUID& mesh_out, S32& storey_out) const;
    /// AVATAR's wall runs (sknavwallruns.h) of every tile meeting the box, mesh-local, with
    /// the mesh origin they are relative to, the tile's key and its polys' storeys' mesh.
    /// Built on the first read after a publish, from the same polys, spaces and air.
    typedef std::function<void(const LLVector3d& origin, const LLUUID& mesh, const sknav::TileKey& key, const sknav::WallRun& run)> WallRunFn;
    void wallRunsIn(const LLVector3d& min, const LLVector3d& max, const WallRunFn& fn) const;
    /// The climb links touching a space: the spaces a climb joins it to, with no geometry of their own.
    std::vector<sknav::ClimbLink> climbLinks(const LLUUID& mesh, U32 space) const;

    /// A path within one mesh (the one under a). Points are global; partial
    /// when b was not reachable and the path ends at the closest point found.
    bool findPath(const LLVector3d& a, const LLVector3d& b, sknav::AgentClass cls, std::vector<LLVector3d>& out, bool& partial) const;

    /// Walls (sknavwalls.h). Positions handed to the callback are global.
    /// Wall faces are what Atmo paints and drains along; segments are the
    /// floor edges the map draws; links are where a floor can be left.
    typedef std::function<void(const LLVector3d& min, const LLVector3d& max, U8 side, U8 area)> WallFaceFn;
    typedef std::function<void(const LLVector3d& a, const LLVector3d& b, sknav::EdgeKind kind, F32 value)> SegmentFn;
    typedef std::function<void(const LLVector3d& from, const LLVector3d& to, F32 height, sknav::LinkKind kind)> LinkFn;
    void wallFacesIn(const LLVector3d& min, const LLVector3d& max, const WallFaceFn& fn) const;
    void segmentsIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const SegmentFn& fn) const;
    /// Drop links from the tiles, then the ladder and elevator links between
    /// the exits of each span. The span links are a table beside the tiles,
    /// not Detour off-mesh connections: paths do not use them yet.
    void linksIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const LinkFn& fn) const;
    /// The solid raster's ledges and the run-off down them.
    void runoffIn(const LLVector3d& min, const LLVector3d& max, const SegmentFn& ledge_fn, const LinkFn& link_fn) const;
    /// Stair flights whose footprint meets the box, with the mesh origin
    /// their mesh-local positions are relative to. A flight across a tile
    /// seam comes once, as the copy of the tile holding its footprint's
    /// centroid, with that tile's polys.
    typedef std::function<void(const LLVector3d& origin, const sknav::Flight& flight)> FlightFn;
    void flightsIn(const LLVector3d& min, const LLVector3d& max, sknav::AgentClass cls, const FlightFn& fn) const;

    /// A ladder's or a working elevator's span with the floors it can be
    /// boarded or left at, per class, lowest first. Global.
    struct FeatureSpan
    {
        sknav::LinkKind mKind = sknav::LinkKind::LADDER;
        LLVector3d mMin;                ///< the span's plan and height
        LLVector3d mMax;
        std::vector<LLVector3d> mExits[sknav::NUM_CLASSES];
        std::vector<std::pair<LLVector3d, LLVector3d>> mLinks[sknav::NUM_CLASSES];
        bool mDirty = true;             ///< a tile it touches published since the exits were found
    };
    /// The span a feature's links were built for, by the feature's id (the
    /// smallest of its roots, which for an elevator is its car), or null.
    const FeatureSpan* featureSpan(const LLUUID& id) const;
    static LLUUID featureId(const sknav::Feature& feature);

    /// The census's doors whose box meets this one, global. Main thread only:
    /// the rooms pass copies the doors overlapping its tiles into its job
    /// input and never reads the live table from a worker.
    typedef std::function<void(const sknav::DoorEntity& door)> DoorFn;
    void doorsIn(const LLVector3d& min, const LLVector3d& max, const DoorFn& fn) const;

    /// The cover a segment ahead of this position gives someone of that eye
    /// height, and how tall it is. False when nothing is within reach.
    bool coverAt(const LLVector3d& pos, const LLVector3& dir, sknav::AgentClass cls, F32 eye_height,
                 sknav::Cover& cover_out, F32& height_out, F32 reach = 1.f) const;

    /// No solid of any published tile's air starts above pos within reach:
    /// the region sky map, which forgets a roof as soon as its tile does.
    bool skyOpenAt(const LLVector3d& pos, F32 reach = 50.f) const { return mSkyMap.skyOpenAt(pos, reach); }
    const sknav::SkyMap& skyMap() const { return mSkyMap; }

    std::string statsLine() const;

    /// The enclosure answers over every mesh, as of the last pass to land
    /// (sknavrooms.h). Main thread; the view itself is readable anywhere.
    std::shared_ptr<const SKNavWorldView> view() const;
    /// The key a tile's air gives the sky map under.
    static U64 skyContributor(const LLUUID& mesh, const sknav::TileKey& key);

    /// Cast from the middle of the screen, mark what it hits, and write down
    /// everything the census and the navmesh know there. Returns a line for
    /// the console; the report itself goes to the log under SKNavDump.
    std::string markUnderCamera();

    /// Every built mesh, tile, polygon, segment, link and face to one JSON
    /// file in the logs folder, for reading offline: the mini-map's test
    /// fixtures, and evidence attached to a report. Written once no spaces
    /// are pending and every tile is stamped against the spaces landed, so
    /// one file holds one generation of them. Returns a line for the console
    /// with the counts, the size and the full path, or that it waits.
    std::string exportAll();
    /// The last line exportAll gave, updated when a waiting export is written.
    const std::string& exportStatus() const { return mExportStatus; }

    /// What every tile is built from, to logs/sknav-census-<UTC>.json:
    /// the census's records, clusters, doors, features and spans, each
    /// region's land as the terrain windows sample it, and the build
    /// parameters, so the build replays offline (sknavcensusio.h). Returns
    /// a line for the console with the counts, the size and the full path.
    std::string exportCensus() const;

    /// The selected linkset, or the one under the screen centre, with everyone
    /// seated on it and their attachments, as triangles in the root's own frame
    /// with each face's transparency: raw input for working out a vehicle's
    /// outline on the mini-map. Its surroundings go with it in world axes: the
    /// census records' collision triangles within reach and the visible faces
    /// of their linksets, the navmesh's own input for replaying a build
    /// offline. Written beside the navmesh export; returns a line for the
    /// console.
    std::string exportObject() const;


    struct Stats
    {
        U32 mMeshes = 0;
        U32 mTiles = 0;
        U32 mTilesBuilt = 0;
        U32 mQueued = 0;
        U32 mInFlight = 0;
        U32 mBuilds = 0;
        U32 mFailures = 0;
        U32 mRefused = 0;       ///< layouts over MAX_TILES_PER_MESH, left unbuilt
        U32 mFaces = 0;
        U32 mSegments = 0;
        U32 mLinks = 0;
        U32 mAirTiles = 0;
        size_t mAirBytes = 0;
        U32 mAirEvicted = 0;    ///< tiles whose detail went to disk for the budget
        U32 mSpilledMeshes = 0;     ///< settled meshes whose detail air waits on disk
        U32 mSpilledTiles = 0;
        U32 mSpills = 0;            ///< tiles spilled, ever
        U64 mSpillBytes = 0;        ///< bytes those spills wrote
        size_t mSpillFreed = 0;     ///< air and raster bytes the last spill let go
        U32 mReloads = 0;           ///< tiles brought back, ever
        U32 mReloadMisses = 0;      ///< of those, ones the disk had lost and that were built again
        F32 mLastReloadMs = 0.f;    ///< the last reload, per tile on the worker
        F32 mLastReloadWallMs = 0.f;    ///< the last reload, from asking to the detail landing
        U64 mDiskBytes = 0;
        U32 mDiskEntries = 0;
        U32 mDiskLoads = 0;     ///< builds answered from disk
        U32 mDiskRebuilds = 0;  ///< builds that looked on disk and found nothing usable
        U32 mDiskCorrupt = 0;   ///< of those, entries that were there but refused
        F32 mDiskLoadMs = 0.f;  ///< the last load, read and unpack
        F32 mLastBuildMs = 0.f;
        F32 mWorstBuildMs = 0.f;
        F32 mExcludeMs = 0.f;   ///< of the last build that left records out of a class, what that cost
        F32 mLastRasterMs = 0.f;    ///< of the last build, its raster stage
        F32 mLastPolyMs = 0.f;      ///< and its polygon stage
        U32 mRasterTiles = 0;       ///< tiles holding their raster
        size_t mRasterBytes = 0;
        U32 mRasterEvicted = 0;     ///< rasters dropped for the budget or past the radius
        U32 mReRasters = 0;         ///< builds of tiles whose raster had been dropped
        U32 mStamps = 0;            ///< stamped tiles published
        U32 mStampLoads = 0;        ///< of those, polys found on disk under the stamp's pattern
        U32 mStampReRasters = 0;    ///< of those, rasterised again for want of a held raster
        U32 mRebinds = 0;           ///< tiles whose spaces moved but whose pattern did not: flags only
        U32 mStampOverflow = 0;     ///< stamps whose palette went down the cascade (sknavstamp.h)
        U32 mPolyOverflowTiles = 0; ///< polys over a Detour tile's budget: not published
        U32 mUnstampedPublishes = 0;    ///< tiles with air published without a stamp; should stay 0
        U32 mAwaitingStamp = 0;     ///< tiles whose raster stage published and whose stamp has not
        F32 mLastStampMs = 0.f;     ///< the last stamp job, stamp input to Detour data
        F32 mLastStampLatencyMs = 0.f;  ///< from a tile's raster stage publishing to its stamped polys
        F32 mWorstStampLatencyMs = 0.f;
        U32 mAirOverlayVerts = 0;   ///< vertices the air overlay last gathered
        U32 mAirOverlayTiles = 0;   ///< tiles it drew from
        U32 mAirOverlayCut = 0;     ///< tiles the vertex budget cut short or left out
    };
    const Stats& stats() const { return mStats; }

private:
    friend class SKNavRooms;
    friend class SKNavRoomProbes;

    /// Why a tile needed building, for the update blip. A record beats the
    /// land and both beat the parameters, because a signature that moved with
    /// all three moved for the most specific reason.
    enum TileCause : U8
    {
        CAUSE_NONE = 0,     ///< nothing to say: no cause was recorded
        CAUSE_CREATED,      ///< the tile was never built
        CAUSE_RECORDS,      ///< a record appeared, left or changed
        CAUSE_TERRAIN,      ///< the land under the tile moved
        CAUSE_PARAMS,       ///< the build parameters moved
        CAUSE_FAILED,       ///< the last build of it failed
    };

    /// A tile's wall segments bucketed by the 2 m cells of its footprint their plan
    /// AABBs touch, so a query over a few metres reads a few cells rather than the
    /// whole tile. Built on first query, for the segment vector it holds.
    struct SegmentCells
    {
        static constexpr S32 ACROSS = 16;
        std::shared_ptr<const std::vector<sknav::WallSegment>> mSource;
        std::vector<U32> mCells[ACROSS * ACROSS];
    };

    struct Tile
    {
        U64 mSignature = 0;     ///< what the tile should be built from
        U64 mBuilt = 0;         ///< what it was built from; 0 = nothing published
        bool mInFlight = false;
        S32 mPolys[sknav::NUM_CLASSES] = { 0 };
        F32 mMillis = 0.f;
        LLVector3d mMin;        ///< global, for scheduling by distance and drawing
        LLVector3d mMax;
        /// Why mSignature last moved away from what was built, and where what
        /// caused it sits (global), when something did.
        U8 mCause = CAUSE_NONE;
        LLVector3d mCauseAt;
        bool mHasCauseAt = false;
        /// What the build rasterises here, gathered when the tile's signature
        /// is worked out rather than walked again on the way to the worker.
        std::vector<sknav::RecordPtr> mRecords;
        /// The terrain window the tile is built from, sampled on the main
        /// thread under a budget and reused until the land under the tile
        /// moves (mLandStamp). mTerrainStale means the window is not the
        /// current land yet, so the tile waits rather than build on old ground.
        std::shared_ptr<const sknav::TerrainWindow> mTerrain;
        U64 mTerrainSignature = 0;
        U64 mRecordSignature = 0;
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
        std::shared_ptr<const std::vector<sknav::WallSegment>> mLedges;
        std::shared_ptr<const std::vector<sknav::DropLink>> mRunoff;
        sknav::TileAirPtr mAir;         ///< within SKNavAirRadius, until evicted
        sknav::TileAirPtr mAirCoarse;   ///< the solid tops an evicted tile keeps for the sky map
        U64 mContentKey = 0;            ///< the session-stable key of what was published (sknavtilecache.h)
        bool mDiskRefused = false;      ///< Detour refused the product loaded from disk: build the next one
        F64 mNearness = 0.0;            ///< camera nearness over time, for the air budget
        U64 mAirBuilt = 0;              ///< the signature the air was built from; 0 = never asked
        U64 mAirId = 0;                 ///< names mAir across a spill and its reload, which change the pointer but not the air
        bool mAirSpilled = false;       ///< its detail air and raster wait on disk under spillKey(mContentKey)
        std::shared_ptr<const std::vector<sknav::WallSegment>> mSegments[sknav::NUM_CLASSES];
        std::shared_ptr<const std::vector<sknav::DropLink>> mLinks[sknav::NUM_CLASSES];
        std::shared_ptr<const std::vector<U32>> mPolySpace;            ///< per AVATAR poly, its space id, as bound
        std::shared_ptr<const std::vector<PortalEdge>> mPortalEdges;   ///< AVATAR's poly edges on portals, as bound
        std::shared_ptr<const std::vector<sknav::Flight>> mFlights[sknav::NUM_CLASSES];
        /// The Detour tile's polygons, extracted on first read and valid only for the publish
        /// that set it: every path that adds or removes the Detour tile resets it.
        mutable std::shared_ptr<const sknav::TilePolys> mPolyCache[sknav::NUM_CLASSES];
        /// AVATAR's wall runs, built on first read from the inputs named in mWallRunsKey.
        mutable std::shared_ptr<const std::vector<sknav::WallRun>> mWallRuns;
        mutable std::array<U64, 12> mWallRunsKey{};
        /// The Detour tile's bytes, which Detour reads but does not own.
        std::vector<U8> mDetour[sknav::NUM_CLASSES];
        /// What the published polygons were built from, held near the camera
        /// so they can be built again without rasterising (sknavbuild.h).
        sknav::TileRasterPtr mRaster;
        bool mRasterDropped = false;    ///< the raster went for the budget since the tile was built
        /// The stamp the published polygons were built with: what it was read
        /// from, the side tables, their binding to the spaces, and the spaces.
        std::shared_ptr<const sknav::TileStampInput> mStampInput;
        std::shared_ptr<const sknav::TileStampGeom> mStampGeom;
        std::shared_ptr<const sknav::TileBinding> mBinding;
        sknav::RoomsProductPtr mStampedBy;
        U32 mStampSlot = 0;             ///< the Detour header's user id: the space filter's key to its polys' spaces
        U64 mStampBuilt = 0;            ///< the signature of the raster the stamped polys were built from
        bool mStampDue = false;         ///< spaces landed over its air since it was stamped
        bool mStampInFlight = false;
        F64 mStaticAt = 0.0;            ///< when its raster stage published, while the stamp is awaited
        /// Checked against mSegments by identity, so a publish needs no reset here.
        mutable std::shared_ptr<const SegmentCells> mSegmentCells[sknav::NUM_CLASSES];
    };

    struct Layout
    {
        std::vector<sknav::LaidTile> mTiles;
        std::vector<U64> mSignatures;
        std::set<sknav::TileKey> mKeys;
        size_t mNext = 0;
        bool mRefused = false;      ///< more tiles than a mesh holds, so none
        U64 mCensus = 0, mWorld = 0, mLand = 0, mParams = 0;
        F32 mPad = 0.f, mVoidMargin = 0.f;
        F64 mMinZ = 0., mMaxZ = 0.;
    };

    struct SpillJob;
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
        dtNavMesh* mNav[sknav::NUM_CLASSES] = { nullptr };
        dtNavMeshQuery* mQuery[sknav::NUM_CLASSES] = { nullptr };
        std::map<sknav::TileKey, Tile> mTiles;
        F64 mLastSeen = 0.f;
        // What the region pass's layout and tile re-sign were last laid
        // against. While all of these stand, the pass would rebuild exactly
        // what already stands, so it is skipped whole.
        U64 mLayoutCensus = 0;
        U64 mLayoutWorld = 0;
        U64 mLayoutLand = 0;
        U64 mLayoutParams = 0;
        F32 mLayoutVoidMargin = 0.f;
        std::future<std::shared_ptr<Layout>> mLayoutJob;
        std::shared_ptr<Layout> mPendingLayout;
        std::map<sknav::TileKey, Tile>::iterator mDropCursor;
        bool mDropping = false;
        /// The spaces the tiles are being stamped with, as they landed.
        sknav::EnclosureProductPtr mStampEnc;
        sknav::RoomsProductPtr mStampRooms;
        bool mStampScan = false;            ///< some tile may be due a stamp
        mutable sknav::StoreyProductPtr mStoreys;
        mutable std::vector<U64> mStoreysKey;   ///< the per-tile inputs they were built from
        /// Where its detail air is: every tile's in memory, on its way to disk, there, or on its way back.
        enum class Residency : U8 { RESIDENT, SPILLING, SPILLED, RELOADING };
        Residency mResidency = Residency::RESIDENT;
        F64 mDetailWanted = 0.0;            ///< when something last needed its detail
        F64 mResidencyChecked = 0.0;
        F64 mReloadAsked = 0.0;
        std::shared_ptr<SpillJob> mRefusedReload;   ///< what a reload read whose swap was refused, to try again
        ~Mesh();
    };

    struct TileCursor
    {
        LLUUID mMesh;
        sknav::TileKey mKey;
        bool mStarted = false;
    };
    bool scanTiles(TileCursor& cursor, F32 seconds, const std::function<void(Mesh&, const sknav::TileKey&, Tile&)>& visit);
    TileCursor mTerrainCursor, mScheduleCursor, mStatsCursor, mEvictionCursor, mSkyCursor;
    struct AirCandidate
    {
        LLUUID mMesh;
        U32 mGeneration;
        sknav::TileKey mKey;
        sknav::TileAirPtr mAir, mCoarse;
        F64 mNearness, mDistance;
        U8 mPriority = 3;
    };
    std::shared_ptr<std::vector<AirCandidate>> mEvictionScan;
    struct RasterCandidate
    {
        LLUUID mMesh;
        U32 mGeneration;
        sknav::TileKey mKey;
        sknav::TileRasterPtr mRaster;
        size_t mBytes;
        F64 mDistance;
        U8 mPriority = 3;
    };
    std::vector<RasterCandidate> mRasterScan;
    /// Held rasters get a quarter of the air budget; past it the farthest go
    /// until they fit in seven eighths of that, and past one and a half
    /// radii any raster goes.
    void evictRasters(size_t budget, F64 radius);
    size_t mHeldRasterBytes = 0;
    std::future<std::shared_ptr<std::vector<AirCandidate>>> mEvictionJob;
    std::shared_ptr<std::vector<AirCandidate>> mPendingEvictions;
    size_t mNextEviction = 0;
    struct SkyContributor { LLVector3d mOrigin; sknav::TileAirPtr mAir; };
    std::shared_ptr<std::map<U64, SkyContributor>> mSkyScan;
    U64 mSkyScanRevision = 0;
    bool mSkyScanReady = false, mEvictionScanReady = false;
    struct BuildCandidate
    {
        LLUUID mMesh;
        U32 mGeneration;
        sknav::TileKey mKey;
        F64 mDistance;
        U8 mPriority = 3;
    };
    std::shared_ptr<std::vector<BuildCandidate>> mScheduleScan, mBuildCandidates;
    std::future<std::shared_ptr<std::vector<BuildCandidate>>> mScheduleJob;
    bool mScheduleScanReady = false;
    size_t mNextCandidate = 0;
    F64 mCandidateTime = 0.f;
    Stats mScanningStats;

    typedef sknav::ExportCounts ExportCounts;
    /// One tile as a JSON object, shared by every export so their formats
    /// cannot drift. Features and doors are written as members, leading
    /// comma included, and only those meeting the box when one is given.
    void writeTile(std::ostream& file, const Mesh& mesh, const sknav::TileKey& key, const Tile& tile, ExportCounts& counts) const;
    void writeFeatures(std::ostream& file, const LLVector3d* min, const LLVector3d* max, ExportCounts& counts) const;
    void writeDoors(std::ostream& file, const LLVector3d* min, const LLVector3d* max, ExportCounts& counts) const;

    bool wanted() const;
    /// Whether the reflection probes consume the rooms: room probes on,
    /// probes enabled, and the probe level that shows the octree's probes
    /// the rooms replace. Demands the navmesh runs for them, air and census
    /// with it.
    static bool probesDemanded();
    /// Whether a tile builds its air eagerly: near the camera, or under the cluster the agent is in.
    bool inAirRadius(const Tile& tile) const;
    /// Which mesh the work queues serve first: 0 the agent's region's own
    /// terrain, 1 the terrain of the regions touching it, 2 the clusters
    /// owned by either, 3 everything else. Within a tier the distance orders,
    /// so a surround in the void can never come before the ground it rings.
    U8 meshPriority(const Mesh& mesh) const;
    /// The plan boxes of the clusters around the agent, whose tiles all get detail air.
    std::vector<std::pair<LLVector3d, LLVector3d>> mHomeBoxes;
    void findHomeBoxes(const SKCensusSnapshotPtr& snap);
    /// Bring the sky map in line with the published tiles' air.
    void syncSkyMap();
    /// The pool the tile builds run on, created on first use. A pool of its
    /// own, so the builds neither queue behind nor starve the viewer's shared
    /// work: they are pure and independent, one per core.
    LL::WorkQueue::ptr_t workerQueue();
    void reconcile(const SKCensusSnapshotPtr& snap);
    void applyLayouts();
    /// Whether a cluster stands on the ground under it, within
    /// SKNavGroundedMargin. A grounded structure is meshed with the region so
    /// that one mesh holds the ground, the walls that block it and the floors
    /// that lead off it; anything clear of the ground gets a mesh to itself.
    bool grounded(const sknav::Cluster& cluster) const;
    static U64 recordSignature(const std::vector<sknav::RecordPtr>& records);
    /// Work out what a tile is built from, note why it moved away from what
    /// was built, and keep the old signature to tell the next move by.
    void reSign(Tile& tile, const std::vector<sknav::RecordPtr>& before, U64 records, U64 terrain, U64 params);
    /// Where the first record that was not here before sits, or the first that
    /// left: the part that moved or appeared is what a line should point at.
    static bool changedRecord(const std::vector<sknav::RecordPtr>& before, const std::vector<sknav::RecordPtr>& after, LLVector3d& out);
    /// A rising blip for a tile that was just built, coloured by why.
    void tileBlip(const Mesh& mesh, const Tile& tile) const;
    void schedule();
    /// What a tile is built from, gathered on the main thread; false while its land is not sampled.
    bool prepareInput(const Mesh& mesh, const sknav::TileKey& key, Tile& tile, std::shared_ptr<sknav::TileBuildInput>& input,
                      std::shared_ptr<const std::vector<sknav::RecordPtr>>& records) const;
    bool launch(Mesh& mesh, const sknav::TileKey& key, Tile& tile);
    /// Stamp the tiles whose spaces landed: the polygons wait for them, so a
    /// tile publishes once, stamped, and a rebuilt one keeps its old polygons
    /// until the new ones land.
    void stampTiles();
    bool launchStamp(Mesh& mesh, const sknav::TileKey& key, Tile& tile);
    struct StampOutcome;
    void publishStamped(const LLUUID& mesh_id, U32 generation, const sknav::TileKey& key, U64 signature, const sknav::TileAirPtr& air,
                        const sknav::RoomsProductPtr& rooms, const std::shared_ptr<StampOutcome>& out);
    /// A binding's flags onto the tile's Detour polys, and its polys' spaces where the filter finds them.
    void applyBinding(Mesh& mesh, const sknav::TileKey& key, Tile& tile);
    /// The space id of a poly, through its Detour tile's user id; 0 for none.
    U32 polySpace(const dtMeshTile* tile, U32 poly) const;
    /// A Detour tile's polys' space ids, for the space filter.
    sknav::SpaceQueryFilter::TileSpaces polySpaces(const dtMeshTile* tile) const;
    /// Tiles still to be stamped against their mesh's landed spaces, a mesh with spaces pending counting one.
    U32 stampsUnsettled() const;
    std::string writeExport() const;
    bool mExportPending = false;
    F64 mExportAsked = 0.0;
    std::string mExportStatus;
    std::unordered_map<U32, std::shared_ptr<const std::vector<U32>>> mSlotSpaces;
    /// Forget a retiring mesh's tiles' entries in mSlotSpaces.
    void dropSlots(const Mesh& mesh);
    U32 mNextSlot = 0;
    void publish(const LLUUID& mesh_id, U32 generation, const sknav::TileKey& key, U64 signature, std::shared_ptr<sknav::TileBuildResult> result,
                 U64 content_key = 0, bool loaded = false, sknav::TileRasterPtr raster = nullptr);
    /// The tile product cache on disk, or null when SKNavDiskCacheMB is 0.
    std::shared_ptr<sknav::TileDiskCache> diskCache();
    /// Keep the tiles' detailed air under SKNavAirBudgetMB, least recently
    /// near first, spilling to disk what is not there already.
    void evictAir();
    /// Settled meshes spill their detail air and rasters to disk, and spilled ones
    /// a product or a reader needs bring it back, both on the workers.
    void updateResidency();
    void spillMesh(Mesh& mesh);
    void reloadMesh(Mesh& mesh);
    void finishSpill(const std::shared_ptr<SpillJob>& job);
    void finishReload(const std::shared_ptr<SpillJob>& job);
    /// Point a mesh's landed products and its stamp at other air, keeping their contents.
    bool swapProductsAir(Mesh& mesh, const sknav::AirSwap& swap);
    sknav::SettleGate mSettleGate;
    mutable std::set<LLUUID> mDetailRequests;
    U64 mNextAirId = 0;
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
    /// Gather the debug overlay anew when the mesh, one of the switches or
    /// the camera has moved enough that the cached one is no longer right,
    /// and upload it. A quiet mesh and a parked camera cost one signature
    /// compare a frame; renderDebug() then only draws what is cached.
    void refreshOverlay();
    /// The air overlay (SKNavOverlayAir) into mOverlay: tiles nearest the camera first, under a vertex budget.
    void gatherAirOverlay(const LLVector3d& camera, F64 reach, U32 mode, bool opaque);
    void gatherClimbLinks(const LLVector3d& camera, F64 reach, bool opaque);
    /// Take the census's ladders and working elevators into mFeatureLinks,
    /// and find the exits of any span a published tile has touched.
    void syncFeatureLinks(const SKCensusSnapshotPtr& snap);
    void findExits(FeatureSpan& span) const;
    /// Every published tile whose footprint meets the box, with its mesh.
    void forEachTileIn(const LLVector3d& min, const LLVector3d& max, const std::function<void(const Mesh&, const Tile&)>& fn) const;
    /// A tile's wall runs as of this revision, null without a Detour tile, stamp or air.
    void storeyInputs(const Mesh& mesh, std::vector<U64>& out) const;
    std::shared_ptr<const std::vector<sknav::WallRun>> tileWallRuns(const Mesh& mesh, const sknav::TileKey& key, const Tile& tile) const;

    std::map<LLUUID, std::unique_ptr<Mesh>> mMeshes;
    /// Span links by region handle, then feature. A tile rebuild would wipe
    /// anything kept in the tile, so they live here and are found again
    /// whenever a tile they touch publishes.
    std::map<U64, std::map<LLUUID, FeatureSpan>> mFeatureLinks;
    U64 mFeatureStamp = 0;          ///< the census the spans were taken from
    /// Doors by region handle, swapped whole from each new census; a region
    /// that has gone drops out with the next one.
    std::map<U64, std::vector<sknav::DoorEntity>> mFeatureDoors;
    std::unique_ptr<LL::ThreadPool> mWorkers;
    std::shared_ptr<sknav::TileDiskCache> mDiskCache;
    U64 mCensusStamp = 0;
    U32 mNextGeneration = 1;
    S32 mInFlight = 0;
    Stats mStats;
    F64 mLastLog = 0.f;
    bool mDemanding = false;
    sknav::SkyMap mSkyMap;
    std::map<U64, sknav::TileAirPtr> mSkyTiles;     ///< what each contributor last gave the sky map
    U64 mSkyRevision = 0;
    struct SkyResult
    {
        sknav::SkyMap mMap;
        std::map<U64, sknav::TileAirPtr> mTiles;
        U64 mRevision = 0;
    };
    std::future<std::shared_ptr<SkyResult>> mSkyJob;
    LLVector3d mMark;
    bool mHasMark = false;
    std::unique_ptr<SKNavRooms> mRooms;
    std::unique_ptr<SKNavRoomProbes> mRoomProbes;
    /// Consumers that keep the navmesh building whatever the overlay and probes want.
    std::set<std::string> mConsumers;

    /// A counter over what the debug overlay draws: bumped whenever a tile
    /// publishes, fails or leaves, or a mesh is torn down. The overlay keeps
    /// its gathered geometry until this moves, so a published tile redraws
    /// the pass on the next frame and a quiet mesh costs nothing.
    U64 mRevision = 0;
    /// The overlay as gathered, and what it was gathered from: everything
    /// but the camera's position and facing, hashed into mOverlayKey, with
    /// those two kept exact so a rebuild is a threshold with hysteresis
    /// rather than a bucket boundary the camera can jitter across.
    SKNavOverlayGeom mOverlay;
    U64 mOverlayKey = 0;
    bool mOverlayFresh = false;
    LLVector3 mOverlayCamera;
    LLVector3 mOverlayForward;
};

#endif // SK_NAVMESH_H
