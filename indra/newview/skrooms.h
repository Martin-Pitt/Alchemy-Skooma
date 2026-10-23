/**
 * @file skrooms.h
 * @brief The rooms layer in the viewer: the per-mesh enclosure, spaces and wind passes over the air cells, scheduled off
 *        the main thread, and the view their answers are read through.
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

#ifndef SK_ROOMS_H
#define SK_ROOMS_H

#include "llsingleton.h"
#include "lluuid.h"
#include "skairskymap.h"
#include "skroomsenclosure.h"
#include "skroomsgraph.h"
#include "skroomsschedule.h"
#include "skroomsspaceproducts.h"
#include "skroomsspaces.h"
#include "skroomswind.h"
#include "skworldoverlay.h"
#include "threadpool.h"
#include "v3dmath.h"

#include <atomic>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

/// What the enclosure pass knows, over every mesh at once. Immutable: the
/// main thread swaps in a new one when a pass lands, and any thread may read
/// one it holds. Positions and heights are global.
class SKWorldView
{
public:
    enum class Channel : U8 { S, E, D, FLOOR_Z, TOP };

    struct Mesh
    {
        LLUUID mId;
        LLVector3d mOrigin;
        LLVector3d mMin;        ///< the footprint of its tiles, global
        LLVector3d mMax;
        skworld::EnclosureProductPtr mProduct;
        skworld::RoomsProductPtr mRooms;
        skworld::RoomGraphPtr mRoomGraph; ///< its spaces grouped into rooms, with mRooms
        skworld::WindProductPtr mWind;    ///< null until its slabs are built
    };

    /// A space: its mesh and its id there.
    struct SpaceId
    {
        LLUUID mMesh;
        U32 mId = 0;
        bool operator==(const SpaceId& o) const { return mMesh == o.mMesh && mId == o.mId; }
    };
    /// A room: its mesh and its id there.
    struct RoomId
    {
        LLUUID mMesh;
        U32 mId = 0;
        bool operator==(const RoomId& o) const { return mMesh == o.mMesh && mId == o.mId; }
    };
    typedef std::function<void(const Mesh& mesh, const skworld::Space& space)> SpaceFn;
    typedef std::function<void(const Mesh& mesh, const skworld::Portal& portal)> PortalFn;
    typedef std::function<void(const Mesh& mesh, const skworld::AirFlight& flight)> FlightFn;

    /// Every mesh's gaps over one point, read once so many heights can be asked of it.
    struct AirColumn
    {
        struct MeshColumn
        {
            LLUUID mId;
            LLVector3d mOrigin;
            skworld::GapColumn mGaps;
        };
        LLVector3d mXY;
        std::vector<MeshColumn> mMeshes;

        /// SKWorldView::enclosureAt at height z over the column's point, with the answering mesh.
        bool enclosureAt(F64 z, skworld::Enclosure& out, LLUUID* mesh_out = nullptr) const;
        /// SKWorldView::airGapAt at height z over the column's point.
        bool airGapAt(F64 z, SpaceId& space, skworld::Enclosure& gap) const;
    };

    /// Moves whenever a mesh's air answers change: enclosure, spaces or rooms. The air revision.
    U64 revision() const { return mAirRevision; }
    U64 airRevision() const { return mAirRevision; }
    /// Moves whenever a mesh's wind slabs change, and only then.
    U64 windRevision() const { return mWindRevision; }

    /// The gap holding pos, over every mesh: an answer beats UNKNOWN, and
    /// the higher floor wins among answers. False where no mesh has air.
    bool enclosureAt(const LLVector3d& pos, skworld::Enclosure& out) const;

    /// The space holding pos, as enclosureAt finds its gap; false in none.
    bool spaceAt(const LLVector3d& pos, SpaceId& out) const;
    /// Where a point is: the air gap holding it, over every mesh as enclosureAt chooses. Inside solid, or in an
    /// indoor gap in no space, the nearest gap in a space below it in the column. The gap's floor and ceiling are
    /// global; space.mId is 0 when the gap is in no space. False where no mesh has a gap there.
    bool airGapAt(const LLVector3d& pos, SpaceId& space, skworld::Enclosure& gap) const;
    /// A space by id, or null. Its positions are relative to its mesh's origin.
    const skworld::Space* space(const SpaceId& id, const Mesh** mesh_out = nullptr) const;
    /// Spaces whose box, and portals whose centre, meet the global box.
    void spacesIn(const LLVector3d& min, const LLVector3d& max, const SpaceFn& fn) const;
    void portalsIn(const LLVector3d& min, const LLVector3d& max, const PortalFn& fn) const;
    void portalsOf(const SpaceId& id, const PortalFn& fn) const;
    /// Flights (stairs, ramps, climbs) whose footprint and height range meet the global box. Positions mesh-local.
    void flightsIn(const LLVector3d& min, const LLVector3d& max, const FlightFn& fn) const;
    /// A flight by its mesh and id, or null.
    const skworld::AirFlight* flight(const LLUUID& mesh, U32 id, const Mesh** mesh_out = nullptr) const;

    /// The room holding a space; null for a space in no room (outdoors, roof, flight) or an unknown id.
    /// Its positions and cells are relative to its mesh's origin.
    const skworld::Room* roomOf(const SpaceId& space, const Mesh** mesh_out = nullptr) const;
    /// The room holding pos, through spaceAt; null where its space is in none.
    const skworld::Room* roomAt(const LLVector3d& pos, RoomId* id_out = nullptr, const Mesh** mesh_out = nullptr) const;
    /// A room by id, or null.
    const skworld::Room* room(const RoomId& id, const Mesh** mesh_out = nullptr) const;
    /// Where the room's mask cell (0, 0) has its minimum corner, global; z is the mesh origin's.
    LLVector3d roomCellOrigin(const Mesh& mesh, const skworld::Room& room) const;

    /// The top of the highest solid over a point, from the sky map; false
    /// when nothing stands there.
    bool topOfWorld(const LLVector3d& xy, F64& top) const;

    /// The column over a point's plan, which enclosureAt, spaceAt and airGapAt read.
    void columnAt(const LLVector3d& xy, AirColumn& out) const;

    /// Every gap over a point, per mesh and lowest first.
    void gapsAt(const LLVector3d& xy, const std::function<void(const skworld::Enclosure&)>& fn) const;

    /// A grid of res-metre samples over the box's plan, row-major along y:
    /// per sample, the highest gap whose floor lies within the box's height,
    /// read on the channel. NaN where there is no answer.
    void raster(const LLVector3d& min, const LLVector3d& max, F32 res, Channel channel, std::vector<F32>& out) const;
    /// The sheet by space on the same grid: per sample, the space of the
    /// highest gap whose floor lies within the box's height, over every mesh;
    /// an id of 0 where that gap is in no space or there is none.
    void sheet(const LLVector3d& min, const LLVector3d& max, F32 res, std::vector<SpaceId>& out) const;

    const std::vector<Mesh>& meshes() const { return mMeshes; }

private:
    friend class SKRooms;
    U64 mAirRevision = 0;
    U64 mWindRevision = 0;
    std::vector<Mesh> mMeshes;
    std::shared_ptr<const skworld::SkyMap> mSky;
};
typedef std::shared_ptr<const SKWorldView> SKWorldViewPtr;

/// Wind near the camera, from each mesh's slabs: 16 headings by height
/// band, each a speed factor and a deflection per 0.5 m cell. Immutable,
/// like the view it reads, so any thread may hold one. A heading is where
/// the wind blows toward, in degrees counter-clockwise from east. Beyond
/// the slabs there is no answer, and the caller uses its own wind.
class SKWind
{
public:
    struct Sample
    {
        F32 mFactor = 1.f;          ///< of the free stream's speed
        LLVector2 mDeflect;         ///< the local wind blows along the heading plus this, normalised
        U8 mFlags = 0;              ///< skworld::WindCell's
    };

    explicit SKWind(SKWorldViewPtr view);

    /// Moves whenever a mesh's slabs change: the view's wind revision.
    U64 revision() const;

    /// The slab of one heading bucket over the box: from the mesh standing
    /// under the box's centre, in the band nearest 1.5 m above the box's
    /// bottom. Its positions are relative to the mesh's origin, given back
    /// in origin. Null where no mesh has slabs.
    skworld::WindSlabPtr slab(S32 heading, const LLVector3d& min, const LLVector3d& max, LLVector3d* origin = nullptr) const;

    /// The wind at a point for a heading, between the two nearest buckets
    /// and the two bands around the point. False where no slab covers it.
    bool sampleAt(const LLVector3d& pos, F32 heading_deg, Sample& out) const;

private:
    const SKWorldView::Mesh* meshAt(const LLVector3d& pos, S32* cell = nullptr) const;

    SKWorldViewPtr mView;
};
typedef std::shared_ptr<const SKWind> SKWindPtr;

/// Schedules the enclosure pass over the air cells (SKAirCells). A mesh whose air changed is marked dirty,
/// and once it has been quiet for SKRoomsSettleMs its tiles' air and the
/// sky map are snapshotted and handed to a queue of its own, one pass per
/// mesh at a time, each in 20 ms chunks so a better tier can preempt it. A
/// pass mends the last answers where their air stands, and air that moves
/// while one runs is mended next, on top of what it lands; its spaces are
/// kept when no air, door or answer the spaces pass reads moved. Meshes
/// that touch read each other's answers one generation late, and a mesh whose answers moved re-runs its neighbours, at most three times.
/// Main thread only, besides the view it hands out.
class SKRooms : public LLSingleton<SKRooms>
{
    LLSINGLETON(SKRooms);
    ~SKRooms() override;

public:
    /// Once per frame after the air cells.
    void update();
    /// Join the queue and leave the census; nothing lands after this.
    void cleanupSingleton() override;

    /// SKRoomsEnabled.
    static bool enabled();
    /// Whether a rooms overlay is drawn: the world debug mask and one of the rooms' own switches.
    static bool overlayActive();
    /// The rooms overlays: spaces, wind, and the air overlay's space and indoor modes.
    void renderDebug();

    /// Main thread only, as mView and mWind are swapped there. What they hand
    /// out is immutable, so a worker may be given it and keep it.
    SKWorldViewPtr view() const;
    SKWindPtr wind() const;
    /// The view's air revision; the wind's moves on its own.
    U64 revision() const { return airRevision(); }
    U64 airRevision() const { return mView ? mView->airRevision() : 0; }
    U64 windRevision() const { return mView ? mView->windRevision() : 0; }
    /// Whether a mesh meeting the global box has a pass pending or running.
    bool settlingIn(const LLVector3d& min, const LLVector3d& max) const;
    std::string statsLine() const;

    /// Fill each space's floor near the camera in a colour of its own, from
    /// the coarse answers, and draw its portals: doors red, stairs lime,
    /// openings cyan, dashed without a lintel. Agent coordinates.
    void gatherSpaces(SKWorldOverlayGeom& geom, const LLVector3d& camera, F64 reach, bool opaque) const;

    /// Arrows over the lowest wind band near the camera for a heading,
    /// coloured by speed factor: blue calm, cyan the free stream, yellow
    /// twice it. Agent coordinates.
    void gatherWind(SKWorldOverlayGeom& geom, const LLVector3d& camera, F64 reach, F32 heading_deg, bool opaque) const;

    /// A tile's per-gap answers, when the view holds answers for exactly
    /// this air, with the generation of the pass that gave them; else null.
    const skworld::TileEnclosure* enclosureTile(const LLUUID& mesh, const skworld::TileKey& key, const skworld::TileAirPtr& air, U64& generation) const;
    /// A mesh's spaces, portals and wind slabs as JSON members, leading comma included, global.
    void writeSpaces(std::ostream& file, const LLUUID& mesh) const;
    /// The products of a mesh's last landed pass; false when none has landed.
    bool landed(const LLUUID& mesh, skworld::EnclosureProductPtr& enclosure, skworld::RoomsProductPtr& rooms) const;
    /// Whether a mesh's spaces, or its wind slabs, are still to land over its air.
    bool passPending(const LLUUID& mesh) const;
    bool windPending(const LLUUID& mesh) const;
private:
    struct Job;
    struct WindJob;
    /// Where a mesh's answers may still move: the plan of the tiles that changed, or all of it.
    struct SettleBox
    {
        LLVector3d mMin = LLVector3d(1e300, 1e300, 0.0);
        LLVector3d mMax = LLVector3d(-1e300, -1e300, 0.0);
        bool mAll = false;

        void add(const LLVector3d& min, const LLVector3d& max);
        void add(const SettleBox& other);
        /// Whether it meets a global box, within the reach a repair's floods can move answers.
        bool meets(const LLVector3d& min, const LLVector3d& max) const;
    };
    struct MeshState
    {
        LLUUID mId;
        LLVector3d mOrigin;
        LLVector3d mMin;
        LLVector3d mMax;
        U64 mAirSignature = 0;
        U64 mDoorSignature = 0;             ///< the doors and lifts over it
        U64 mRoomsDoorSignature = 0;        ///< the doors and lifts its landed spaces were cut with
        std::map<skworld::TileKey, std::pair<U64, std::pair<LLVector3d, LLVector3d>>> mTileAir;   ///< per tile its air id and plan, as last seen
        SettleBox mPending;                 ///< what moved since the last post
        SettleBox mPosted;                  ///< what the pass in flight answers again
        bool mDirty = false;
        bool mInFlight = false;
        F64 mChanged = 0.0;                 ///< when its air last moved
        F64 mDirtySince = 0.0;              ///< when it last went dirty: churn delays a refresh this long at most
        F64 mLastPosted = 0.0;              ///< ready meshes take turns: the longest unserved goes first
        U64 mGeneration = 0;
        U32 mReruns = 0;                    ///< neighbour-driven re-runs since its air last moved
        std::shared_ptr<std::atomic<U64>> mLatest;
        skworld::EnclosureProductPtr mProduct;
        skworld::RoomsProductPtr mRooms;
        skworld::RoomGraphPtr mRoomGraph;     ///< built from mRooms on the rooms thread
        skworld::WindProductPtr mWind;
        bool mWindDirty = false;            ///< its rooms landed since its slabs were built
        bool mWindInFlight = false;
        F64 mWindChanged = 0.0;             ///< when its answers last moved
        F64 mWindDirtySince = 0.0;          ///< when its slabs last went stale: churn delays a refresh this long at most
        U64 mWindGeneration = 0;
        std::shared_ptr<std::atomic<U64>> mWindLatest;  ///< what cancels a wind pass: only a preemption, never the air moving
        U32 mEvicted = 0;                   ///< tiles whose detail the air budget let go
        U8 mPriority = 3;                   ///< the air's order for it: the agent's region first

        skworld::RoomsSchedule schedule() const
        {
            return { mProduct != nullptr, mDirty, mInFlight, mChanged, mDirtySince, mLastPosted, mPriority };
        }
        /// Wind is scheduled as the rooms are: a mesh with no slabs builds them on the rooms it has.
        skworld::RoomsSchedule windSchedule() const
        {
            return { mWind != nullptr, mWindDirty, mWindInFlight, mWindChanged, mWindDirtySince, 0.0, mPriority };
        }
        void windStale(F64 now)
        {
            if (!mWindDirty)
            {
                mWindDirtySince = now;
            }
            mWindDirty = true;
            mWindChanged = now;
        }
    };

    void post(MeshState& state);
    void postWind(MeshState& state);
    void runWind(const std::shared_ptr<WindJob>& job, const std::weak_ptr<bool>& alive);
    void finishWind(const std::shared_ptr<WindJob>& job);
    void runChunk(const std::shared_ptr<Job>& job, const std::weak_ptr<bool>& alive);
    void finish(const std::shared_ptr<Job>& job);
    void rebuildView();
    bool touches(const MeshState& a, const MeshState& b) const;
    static std::vector<std::pair<LLVector3d, LLVector3d>> lifts(const LLVector3d& min, const LLVector3d& max);

    std::map<LLUUID, MeshState> mMeshes;
    std::unique_ptr<LL::ThreadPool> mThread;
    std::shared_ptr<bool> mAlive;
    std::shared_ptr<const skworld::SkyMap> mSky;
    U64 mSkyVersion = ~0ull;
    U64 mSeenRevision = ~0ull;
    U64 mSeenFeatures = ~0ull;
    F64 mLastFullWalk = -1.0;               ///< when every mesh was last walked, the change list's backstop
    SKWorldViewPtr mView;
    SKWindPtr mWind;
    U64 mViewAirRevision = 0;
    U64 mViewWindRevision = 0;
    bool mViewAirDirty = false;             ///< a mesh's enclosure, spaces or rooms changed since the view was built
    bool mViewWindDirty = false;            ///< a mesh's wind slabs changed since

    // stats
    F32 mLastMs = 0.f;
    F32 mLastWallMs = 0.f;
    F32 mLastStageMs[skworld::EnclosurePass::NUM_STAGES] = {};
    F32 mLastSpacesMs = 0.f;
    F32 mLastSpaceStageMs[skworld::SpacesPass::NUM_STAGES] = {};
    U32 mPasses = 0;
    U32 mCancelled = 0;
    U32 mRepairs = 0;                       ///< passes that mended the last answers
    U32 mSpacesKept = 0;                    ///< of which kept the last spaces
    U32 mLastAnswered = 0;                  ///< tiles the last pass answered again
    F32 mLastRoomGraphMs = 0.f;
    F32 mLastFlightMs = 0.f;                ///< the last spaces pass's flight detection
    F32 mLastWindMs = 0.f;
    size_t mLastWindBytes = 0;
    U32 mWindPasses = 0;

    /// What the census file's rooms section writes: the parameters of the last pass posted.
    skworld::EnclosureParams mCensusEnclosure;

    // overlay
    void gatherAirModes(const LLVector3d& camera, F64 reach, U32 mode);
    void refreshOverlay();
    SKWorldOverlayGeom mOverlay;
    bool mOverlayFresh = false;
    U64 mOverlayKey = 0;
    LLVector3 mOverlayCamera;
    LLVector3 mOverlayForward;
};

#endif // SK_ROOMS_H
