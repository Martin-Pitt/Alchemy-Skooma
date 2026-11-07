/**
 * @file skroadmodel.h
 * @brief Spike: the road autopilot's road model, the road read from its pieces: parts, segments, joins, the walk
 * and the lane line.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef SK_ROADMODEL_H
#define SK_ROADMODEL_H

#include "llfile.h"
#include "llmath.h"
#include "llquaternion.h"
#include "lltimer.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "skairdistil.h"
#include "skcensustypes.h"

#include <boost/iterator/indirect_iterator.hpp>

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The road model as skroaddriver.cpp had it, moved whole: what it knows of the viewer it asks the host below,
// which the driver is in the viewer and a frozen case is in the road bench, so the same road reading runs on both.
namespace skroad
{
    // The model's clock: real time, unless the bench holds it at a case's time.
    F64 clockSeconds();
    void setClock(F64 seconds);                 // negative: real time again

    class Timer
    {
    public:
        Timer() { reset(); }
        void reset() { mStart = clockSeconds(); }
        F64 getElapsedTimeF64() const { return clockSeconds() - mStart; }

    private:
        F64 mStart = 0;
    };

    // A census scan as the model reads it. A CensusPtr shares its scan's ownership, so one scan is one owner.
    struct CensusView
    {
        U64 mStamp = 0;
        const std::vector<skworld::RecordPtr>& mRecords;
        const std::vector<skworld::RecordPtr>& mAdded;
        const std::vector<skworld::RecordPtr>& mRemoved;
        const std::vector<skworld::RecordPtr>& mChanged;
    };
    typedef std::shared_ptr<const CensusView> CensusPtr;

    struct PieceShapes;
    struct LanePoint;

    // What the model reads of the world past the census: skroaddriver.cpp's in the viewer, the bench's from a case.
    namespace host
    {
        struct Props                            // what the simulator said of a linkset
        {
            std::string mName;
            LLUUID mOwnerId;
            LLUUID mGroupId;
        };
        struct MeshPlacement                    // a mesh or sculpt part as it lies, with its asset
        {
            LLUUID sculpt_id;
            U8 sculpt_type = 0;
            U8 physics_shape = 0;
            LLVector3 pos;                      // region frame
            LLQuaternion rot;
            LLVector3 scale;
        };
        CensusPtr census();
        const Props* properties(const LLUUID& root);    // null until they come
        bool hasProperties(const LLUUID& root);
        bool watchProperties(std::function<void(const LLUUID&)> changed);
        bool lindenOwner(const LLUUID& owner);           // a Linden, a mole or an LDPW member
        void learnLdpw(const LLUUID& owner);             // an object's owner set it to LDPW: a member
        S32 parcelOwnership(F64 x, F64 y);               // the parcel overlay's ownership at a global point
                                                         // (PARCEL_PUBLIC, PARCEL_OWNED, ...), -1 unknown
        // The parcel at a point: 1 Linden road land, 0 not Linden's, 2 a destination by its name (a station, an
        // airport: never road, whoever owns it), -1 not known yet; its name where asked
        S32 roadLandAt(F64 x, F64 y, std::string* name = nullptr);
        bool privateRoadsAllowed();                      // SKRoadAutopilotPrivateRoads
        void learnTextures(PieceShapes& piece);          // its faces' textures and its top's, once its object is live
        bool meshPlacement(const skworld::Record& r, MeshPlacement& out);
        bool rootRotation(const LLUUID& root, LLQuaternion& rot);
        // The linkset's kit hash (skroadkits.h kitGeoSignature) and its root's global pose; false until its
        // members are all known
        bool kitOf(const LLUUID& root, U64& hash, LLVector3d& pos, LLQuaternion& rot);
        // Measures the linkset's kit (skroadkits.h measureKit) from its live geometry, off the main thread where it
        // can, and hands the result to kitMeasured(key) on the main thread; false where its shape is not all in yet
        bool measureKit(const LLUUID& root, U64 key);
        std::string signLibraryPath();                   // SKRoadSignLibrary: empty, no library
        // Every face's texture and PBR base colour of the linkset; false until it is live
        bool signTextures(const LLUUID& root, std::vector<LLUUID>& out);
        void sortAsks(const LLVector3d& car);            // the linksets to ask about, nearest first
        F32 lookAheadSetting();                          // SKRoadAutopilotLookAhead
        std::string goal();                              // SKRoadAutopilotGoal
        bool surveyOn();                                 // SKRoadSurvey
        std::string logPath(const std::string& file);
        std::vector<LanePoint> testCourse(const std::string& name, F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v);
    }

    constexpr F64 PI = 3.14159265358979323846;

    // ---------------------------------------------------------------- tuning (predictive.py)

    constexpr F64 HORIZON = 2.5;                // s rolled forward
    constexpr F64 PATH_EVERY = 0.2;             // s between walks of the lane path
    constexpr F64 PATH_STEP = 2.0;              // m between cross-sections
    constexpr F64 OPEN_W = 27.5;                // m: a section this wide reached its limit both ways (open area)
    constexpr F64 V_PLAN_MIN = 4.0;             // plan as if moving at least this fast (a stopped car still turns)
    const LLUUID LDPW("4a18e287-db5b-0551-2500-087da6e89efb");
    const LLUUID GOVERNOR_LINDEN("3d6181b0-6a4b-97ef-18d8-722652995cf1");   // owns most of the older Linden roads

    // ---------------------------------------------------------------- the road as its pieces

    struct PieceShapes;

    struct Shape
    {
        F64 x[4], y[4];
        F64 zv[4] = { 0, 0, 0, 0 };             // each corner's height: a road on a hill slopes
        S32 n = 0;
        F64 z = 0;                              // its mean height
        F64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // its plan's bounds
        F64 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0; // the bounds of where inside() holds for it (see bound)
        const PieceShapes* piece = nullptr;
    };

    // An edge of the outline of one of a part's sides (see buildSides), in its loop's order.
    struct SideEdge
    {
        F64 x0, y0, z0, x1, y1, z1;
        U64 key;                                // its corners rounded, either way round (cornerKey)
        bool loop_start;                        // the first edge of its loop
        F64 nx = 0, ny = 0, nz = 1;             // its side's normal
    };

    // An edge of a part's top as a road piece: a straight run of its upward sides' outline, with the way out of
    // the part across it (see boxWalk).
    struct TopEdge
    {
        F64 x0, y0, z0, x1, y1, z1;
        F64 ox, oy;                             // outward, in plan
    };

    struct PieceShapes
    {
        U64 id = 0;                     // its key in sShapeCache: root, part, signature
        U64 signature = 0;
        LLUUID root;
        LLUUID part;
        std::vector<Shape> shapes;
        std::vector<SideEdge> outline;  // its sides' outlines, where it meets other pieces
        U64 faces = 0;                  // its faces, order-free: a part duplicating another has the same
        std::vector<LLUUID> textures;   // its faces', sorted, once its object is live
        bool textures_known = false;
        F64 used = 0;                   // when it was last in reach (sCacheClock)
        // its top as a road piece: a plain box's top face, or its upward sides standing in for one (boxWalk)
        std::vector<TopEdge> top;
        bool box = false;               // a plain box prim (the census's exact box)
        bool thick = false;             // a box slab too thick to be road on its own (a deck's structure): see carryOn
        F64 cx = 0, cy = 0, zt = 0;     // the top's middle and mean height
        F64 ux = 1, uy = 0;             // its long axis in plan (vx, vy = -uy, ux its short one)
        F64 lu = 0, lv = 0;             // its length along each
        F64 bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;     // its plan's bounds
        LLUUID top_tex;                 // the texture of the face on top, once its object is live
        bool top_known = false;
        U32 match_gen = 0;              // sDriveTexGen when `matches` was worked out
        bool matches = true;            // in the driving surface's textures (drivableStrict)
    };

    // ---------------------------------------------------------------- the air (foundation-world/air)

    // The air's walkable floors are the road's surface where the census has a road: the top a car stands on, a
    // plane with its slope, never a base under the visible road nor a post on it; and each floor's clearance,
    // the distance to the nearest wall or obstacle (a fence, a bollard), to 2 m.
    struct AirTile
    {
        skworld::TileAirPtr air;
        F64 ox = 0, oy = 0, oz = 0;             // global position of the air's origin
        F64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // its footprint
    };
    struct LanePoint
    {
        F64 x, y, ax, ay;       // the kept line's point and the road's direction there
        F64 lane_w, road_w;
        S32 lanes;
        F64 z;
        F64 left_x, left_y, right_x, right_y;   // the road's edges at this section, for the overlay
        S8 access = -1;                         // whose road it is under the point (ROAD_PUBLIC...), see accessAt
        S8 lane_source = 0;                     // what placed its edges and lane: LANE_RULE or LANE_KIT (applyKit)
    };

    // Whose road a linkset is: public (a Linden's or a mole's, or rooted on the Linden road parcel) or private (a
    // resident's, rooted on their own land). Decided by the parcel under the linkset's root, not where its pieces
    // lie: a private road reaching over the public road parcel to meet it keeps its root on its owner's land.
    constexpr S8 ROAD_UNKNOWN = -1, ROAD_PRIVATE = 0, ROAD_PUBLIC = 1;
    S8 linksetAccess(const LLUUID& root);
    // The road under a point: public where any piece there is public (a private road's piece overhanging the public
    // road is that road's junction mouth, not a private stretch of it), else private where any is, else unknown.
    S8 accessAt(F64 x, F64 y, F64 z);
    const char* accessName(S8 access);

    // The right lane at a point across a road (centre, direction, widths), and the lane line's offset right of the
    // road's centre line on a two-lane road (the car's half width and the margin, `keep` further): for placing a
    // car on the road from outside the walk (the drive mission).
    bool laneAt(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 keep, LanePoint& out);
    // Where the road under a lane point is a measured road kit (skroadkits.h), its deck edges and lane centre in
    // place of the generic rule's (laneShift); the point is left as it was elsewhere.
    constexpr S8 LANE_RULE = 0, LANE_KIT = 1;
    bool applyKit(LanePoint& lp, F64 keep);
    // Two linksets that are copies of one kit (same geometry signature), so the joint between them is no change
    // of surface; false where either is not known yet.
    bool sameKit(const LLUUID& a, const LLUUID& b);
    // The lane line walked along the road's pieces from (x, y, z) toward (hx, hy), at speed v (its length).
    std::vector<LanePoint> boxWalk(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v, F64 keep);
    F64 laneShift(F64 keep);

    // A junction on the road ahead: where more than one way on leaves a segment's end (or a junction slab).
    // Every way on is a branch; which one the car takes is a choice made once and kept (chooseBranch), so a
    // later policy can turn on purpose: down a road not yet driven, or by whose road it is (Linden, mole,
    // private), from each branch's linkset.
    struct Branch
    {
        LLUUID root;                            // the branch's first linkset (its owner, group and name are
                                                // ALObjectPropertiesCache's)
        F64 dx = 1, dy = 0;                     // its direction leaving the junction
        F64 turn_deg = 0;                       // signed against the way in: + left, - right
        F64 x = 0, y = 0;                       // where it starts (its end at the junction)
        bool same_surface = true;
        bool on_route = false;                  // a linkset of the remembered route
        bool definite = false;                  // its linkset is road outright
        S32 seg = -1;
        LLUUID part;                            // its first part (the box walk's), where a branch is a part
        S8 access = ROAD_UNKNOWN;               // its linkset's (linksetAccess)
        bool mouth = false;                     // a private road's mouth on a public road (its pieces overhanging it)
        bool side = false;                      // met level with the way in, to the side (a T's cross road)
        bool land_unknown = false;              // its land's owner not known yet (no junction choice onto it)
        const LLUUID& id() const { return part.notNull() ? part : root; }
        const char* kind() const
        {
            return std::fabs(turn_deg) < 30 ? "straight" : turn_deg > 0 ? (turn_deg < 60 ? "bear left" : "left")
                                                                         : (turn_deg > -60 ? "bear right" : "right");
        }
    };
    struct Junction
    {
        LLUUID from;                            // the linkset the car arrives on
        F64 x = 0, y = 0;
        std::vector<Branch> branches;
        S32 chosen = -1;
    };

    // walk_end's first word where the walk reached the road's end (View::road_end)
    constexpr const char* WALK_END_ROAD_END = "end: ";

    struct View
    {
        bool valid = false;
        LLVector3d car;
        F64 yaw = 0;
        std::vector<LanePoint> path;
        std::vector<std::pair<F64, F64>> rollout;    // the predicted car with no new tap
        S32 tapping = 0;
        bool waiting = false;
        bool chained = false;                       // the path from the road's segments, not cross-sections
        S32 other_surface = 0;                      // path steps past the surface's textures (bridge, junction)
        S32 width_fallbacks = 0;
        S32 edge_hooks = 0;     // lines whose hook at a region edge was run on straight (straightenEdgeHook)
        S32 lane_kinks = 0;     // sections the lane continuity guard moved (keepLaneContinuous)
        std::string route;                          // how the walk used the committed route: followed, repaired, or seeded afresh and why
        std::string walk_end;                      // why the box walk stopped where it did; WALK_END_ROAD_END first at the road's end
        bool road_end = false;                      // the walk reached the road's end: nothing but ground past its last piece
        F64 road_end_x = 0, road_end_y = 0;         // the last piece's far edge, where the line stops
        F64 road_end_dx = 1, road_end_dy = 0;       // the way into the end
        // signs beside the walked road read by their texture (skroadsigns.h): the nearest posted limit and warning
        // ahead, as far along the walk (m); 0 where none
        F32 sign_speed_limit = 0.f;
        F64 sign_speed_at = 0;
        S32 sign_warning = 0;                       // a SignClass: junction, curve, danger, stop, give way
        F64 sign_warning_at = 0;
        bool jump = false;                          // the walk crossed a gap: a ramp's lip with the road going on beyond
        F64 jump_x = 0, jump_y = 0, jump_z = 0;     // the lip
        F64 land_x = 0, land_y = 0, land_z = 0;     // where the road picks up again
        F64 jump_dx = 1, jump_dy = 0;               // the way across
        std::string source;                         // what laid the path: "walk" (through the chain), "chain", "free walk", "kept"
        std::vector<LanePoint> chain_line;          // the chain's own line, beside the walk's
        std::vector<Junction> junctions;            // on the chain ahead, with the branch taken
        struct SurfaceChange
        {
            F64 x, y, z, dx, dy, w;
            bool same_road;                         // the road's shape goes on: re-surfaced, not another road
        };
        std::vector<SurfaceChange> surface_changes; // where the road's surface ends and another starts
        F64 walk_ms = 0;                            // the last refresh's walk and laying (boxWalk), ms
    };

    // The road once walked: the lane laid along the committed route, as far back as the history the route keeps and
    // ahead to where the walk got, kept from refresh to refresh. A refresh moves the car along it and walks on only
    // from its far end; what changes under it (a piece added or gone, a piece's verdict, surface or shape, the air, a
    // sign) re-lays it from the first route step whose footprint it touches, the steps before it standing. It holds
    // ids and values, never pointers into the parts, so it can be handed to another thread whole.
    struct StoredRoad
    {
        struct Step
        {
            LLUUID part, root;
            F64 s = 0;                          // where the line enters it, along the stored road
            U32 serial = 0;                     // its number, kept as steps behind the car are dropped
        };
        struct Point
        {
            F64 x = 0, y = 0, s = 0;            // a point of the walked line
            LLUUID a, b;                        // the parts it lies on (one, or the two it is between)
            U32 step = 0;                       // the serial of the step it was walked on
        };
        struct SignHit
        {
            U32 step = 0;
            F64 s = 0;                          // along the stored road (as the walk measured it)
            S32 what = 0;                       // a SignClass
            F32 value = 0.f;
        };
        std::vector<Step> steps;
        std::vector<Point> line;
        std::vector<LanePoint> lanes;           // one per line point, as laid
        std::vector<LanePoint> dense;           // the lane line every PATH_STEP, on the road's surface, smoothed
        std::vector<LanePoint> dense_raw;       // ... before smoothing: what a point next to a joint is smoothed from again
        std::vector<F64> dense_s;
        std::vector<std::pair<U32, Junction>> junctions;                    // by the step they were met on
        std::vector<std::pair<U32, View::SurfaceChange>> surface_changes;
        std::vector<SignHit> signs;
        bool jump = false;                      // a gap crossed, by the step it was crossed from
        U32 jump_step = 0;
        F64 jump_x = 0, jump_y = 0, jump_z = 0, land_x = 0, land_y = 0, land_z = 0, jump_dx = 1, jump_dy = 0;
        bool ended = false;                     // the walk stopped at its far end short of its length ...
        bool road_end = false;                  // ... at the road's end
        bool stuck = false;                     // ... with the road not ending there (its next part not met)
        F64 road_end_x = 0, road_end_y = 0, road_end_dx = 1, road_end_dy = 0;
        std::string walk_end;
        F64 keep = 0;                           // the lane's keep it was laid for
        U32 next_serial = 1;
        F64 car_s = 0;                          // the car's place along it at the last refresh
        size_t car_i = 0;                       // ... the dense point before it
        // what it was laid over, to tell what changed: the road parts, the air tiles and the sign candidates
        // whose footprints lie within STORE_MARGIN_M of its sections
        struct PartSeen
        {
            U64 state = 0;                      // its key (root, part, signature), verdict and surface
            F64 bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        };
        std::unordered_map<LLUUID, PartSeen> parts_seen;
        std::vector<std::pair<U64, std::array<F64, 4>>> air_seen;
        std::vector<std::pair<U64, std::array<F64, 4>>> signs_seen;
        U64 parts_gen = 0, verdict_gen = 0, air_rev = ~0ull, tex_gen = 0, sign_key = 0;
        // a stuck end is walked again only once something could have changed: the parts or the car's step
        U64 stuck_parts_gen = 0, stuck_verdict_gen = 0;
        U32 stuck_car_step = 0;

        bool empty() const { return dense.empty(); }
        void clear() { *this = StoredRoad(); }
    };
    extern StoredRoad sStored;
    // the path came from the stored road (readRoad), and the whole of it, behind the car and ahead, for the overlay
    extern bool sPathFromStore;
    extern std::vector<LanePoint> sRoadPath;
    // What the overlay draws as the car's lane: the stored road whole where the path is laid from it, else the path.
    const std::vector<LanePoint>& roadForView();

    // ---------------------------------------------------------------- the road as segments

    // A road object (linkset) is a segment: its drivable parts' upward faces give its centre line, ends
    // and width exactly. The lane line follows segments chained end to end, choosing at each end the one
    // that carries on straightest, rather than slicing across whatever lies flat beside the road (a slice
    // ran into a car park beside a side road). A squarish segment (a junction, a plaza) has no direction of
    // its own: the line passes straight through it.
    struct Segment
    {
        LLUUID root;
        F64 cx = 0, cy = 0, ax = 1, ay = 0;     // centre (on the centre line) and axis
        F64 lo = 0, hi = 0;                     // its ends along the axis from the centre
        F64 width = 0, z = 0;
        F64 zc = 0, zslope = 0;                 // its surface's height at the centre, and rise per m along the
                                                // axis: a road on a hill drops metres along one linkset
        bool square = false;
        bool straight = false;
        bool boxed = false;                     // its frame is its root's box, not a line fitted to its faces                  // its faces fill most of its rectangle: its axis is the road's
                                                // direction all along it (a curved one's is not)
        bool matches = true;                    // in the driving surface's textures
        bool pending = false;                   // its properties have not come yet: maybe road, not yet known
        bool definite = false;                  // road outright (a Linden group, a road name, a Linden's unnamed
                                                // piece), not only road-shaped (an unnamed private pavement)
        std::unordered_set<LLUUID> textures;    // of the parts it is made of (where known)
        std::unordered_set<LLUUID> surface;     // its main surface: the textures of the parts that cover most of
                                                // it (a part's are all its faces': its kerb's and sides' too)
        std::vector<std::pair<F64, F64>> pts;   // its drivable faces' corners, for a square one's extent
        std::vector<const Shape*> shapes;       // its top faces
        std::vector<std::pair<F64, F64>> part_mids; // each of its parts' faces' corners' middle (a curved one's line)
    };
    // The segments in reach, by index; each one is kept in sRoots (see settleSegments).
    struct ActiveSegments
    {
        std::vector<Segment*> list;
        Segment& operator[](size_t i) const { return *list[i]; }
        size_t size() const { return list.size(); }
        bool empty() const { return list.empty(); }
        boost::indirect_iterator<std::vector<Segment*>::const_iterator> begin() const { return list.begin(); }
        boost::indirect_iterator<std::vector<Segment*>::const_iterator> end() const { return list.end(); }
    };

    // Where two road pieces meet: their top faces' outer edges lying on each other, in line, level and
    // overlapping. Bases go to any depth; the tops of a road's pieces make one strip, and these are its seams.
    struct Join
    {
        S32 other = -1;
        F64 x = 0, y = 0, z = 0;                // the middle of the shared edge
        F64 ex = 1, ey = 0;                     // the edge's direction
        F64 length = 0;                         // how much edge they share
        F64 ax = 0, ay = 0, bx = 0, by = 0;     // the ends of what they share, along the edge
        F64 lo = 1e9, hi = -1e9;                // (its extent from the first shared edge's start)
    };
    struct Continuation
    {
        S32 other;                              // the part it goes on onto
        S32 edge, other_edge;                   // across this part's top edge, onto the other's
        F64 x, y, z, ax, ay, bx, by;            // where (see Meet)
    };

    constexpr F64 SIGHT_MIN_M = 20.0, SIGHT_LIFT_S = 3.0, SIGHT_BRAKE_S = 1.5;   // the road it can see (below)

    // ---------------------------------------------------------------- the line's way

    constexpr F64 LINE_DIR_KEEP_S = 10.0, LINE_BACK_DEG = 100.0, STEER_MIN_LINE_M = 10.0;

    // Who picks a junction's branch under SKRoadAutopilotGoal "route": the junction's point and its branches'
    // bearings (degrees clockwise from north) to the branch's index, -1 for no opinion. Unset, explore decides.
    using BranchPicker = std::function<int(double x, double y, const std::vector<double>& bearings)>;
    void setBranchPicker(BranchPicker picker);

    F64 wrap(F64 a);
    F64 lookAhead(F64 v);
    F32 shapeReach();
    extern std::unordered_set<LLUUID> sDriveTex;
    void driveTexChanged();
    extern std::vector<LLUUID> sChainRoots;
    extern std::unordered_set<LLUUID> sRoute;
    extern std::vector<const Shape*> sShapes;
    extern bool sShapesRead;
    extern std::deque<LLUUID> sToAsk;
    extern std::deque<LLUUID> sAskFirst;
    extern std::unordered_set<LLUUID> sAskFirstSet;
    extern std::unordered_map<LLUUID, F64> sAsked;
    extern Timer sAskClock;
    extern Timer sSinceAsk;
    extern bool sGatherDirty;
    void refreshShapes(const LLVector3d& car);
    // Gathers afresh at the next refresh and forgets the line, route and surface: a new drive somewhere else (the
    // bench's next case in the same process).
    void forgetRoad();
    extern std::vector<AirTile> sAirTiles;
    extern U64 sAirRevision;
    extern Timer sSinceAir;
    bool airFloor(F64 x, F64 y, F64 z, F64 up, F64 down, F64& floor, F64& clear);
    F64 topAt(F64 x, F64 y, F64 z, F64 up);
    // The highest road shape's top at a point, no more than `up` above z nor `down` below it; false where none is.
    bool roadTopAt(F64 x, F64 y, F64 z, F64 up, F64 down, F64& top);
    void noteDriveSurface(F64 x, F64 y, F64 z, bool starting);
    extern F64 sCarHalfW;
    extern View sView;
    extern Timer sClock;
    extern std::vector<LanePoint> sPath;
    extern F64 sPathTime;
    extern std::deque<std::pair<F64, F64>> sDriven;
    extern bool sHaveLastLine;
    extern F64 sLastSeenRoad;
    extern ActiveSegments sSegments;
    extern std::vector<std::vector<Join>> sJoins;
    F64 zAt(const Segment& sg, F64 x, F64 y);
    const std::vector<S32>& siblings(S32 idx);     // the halves alongside a segment: one road with it
    F64 reach(const Segment& sg, F64 x, F64 y, F64 dx, F64 dy);
    void writeDriven();
    void noteDriven(const LLVector3d& pos, F64 now);
    extern std::vector<PieceShapes*> sParts;
    extern std::vector<std::vector<Continuation>> sConts;
    extern std::vector<S32> sWalkParts;
    F64 lineSeen();
    bool readRoad(const LLVector3d& pos, F64 hx, F64 hy, F64 speed, F64 keep, F64 now, const std::string& course);
    void noteLineWay(F64 now);
}

#endif // SK_ROADMODEL_H
