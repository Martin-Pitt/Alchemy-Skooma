/**
 * @file skrooms.h
 * @brief The rooms layer in the viewer: the per-mesh enclosure and spaces passes over the air cells, scheduled off
 *        the main thread, and the view their answers are read through.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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
#include "skworldoverlay.h"
#include "threadpool.h"
#include "v3dmath.h"

#include <atomic>
#include <functional>
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
    std::vector<Mesh> mMeshes;
    std::shared_ptr<const skworld::SkyMap> mSky;
};
typedef std::shared_ptr<const SKWorldView> SKWorldViewPtr;

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
    /// The rooms overlays: spaces, and the air overlay's space and indoor modes.
    void renderDebug();

    /// Main thread only, as mView is swapped there. What it hands out is
    /// immutable, so a worker may be given it and keep it.
    SKWorldViewPtr view() const;
    /// The view's air revision.
    U64 revision() const { return airRevision(); }
    U64 airRevision() const { return mView ? mView->airRevision() : 0; }
    /// Whether a mesh meeting the global box has a pass pending or running.
    bool settlingIn(const LLVector3d& min, const LLVector3d& max) const;
    std::string statsLine() const;

    /// Fill each space's floor near the camera in a colour of its own, from
    /// the coarse answers, and draw its portals: doors red, stairs lime,
    /// openings cyan, dashed without a lintel. Agent coordinates.
    void gatherSpaces(SKWorldOverlayGeom& geom, const LLVector3d& camera, F64 reach, bool opaque) const;
private:
    struct Job;
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
        U32 mEvicted = 0;                   ///< tiles whose detail the air budget let go
        U8 mPriority = 3;                   ///< the air's order for it: the agent's region first

        skworld::RoomsSchedule schedule() const
        {
            return { mProduct != nullptr, mDirty, mInFlight, mChanged, mDirtySince, mLastPosted, mPriority };
        }
    };

    void post(MeshState& state);
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
    U64 mViewAirRevision = 0;
    bool mViewAirDirty = false;             ///< a mesh's enclosure, spaces or rooms changed since the view was built

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
