/**
 * @file sknavspaces.h
 * @brief A mesh's air cut into spaces at door, stair and opening portals.
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

#ifndef SK_NAVSPACES_H
#define SK_NAVSPACES_H

#include "sknavenclosure.h"
#include "sknavfeatures.h"
#include "sknavflights.h"

#include "v2math.h"
#include "v4color.h"

#include <functional>
#include <memory>
#include <vector>

namespace sknav
{
    enum class PortalKind : U8 { DOOR = 0, STAIR = 1, OPENING = 2 };

    /// What a portal was cut on.
    enum PortalCue : U8
    {
        CUE_LINTEL = 1 << 0,        ///< the ceiling drops over it
        CUE_JAMBS = 1 << 1,         ///< blocked past both ends: walled on both sides
        CUE_E_STEP = 1 << 2,        ///< indoorness differs across it
        CUE_DOOR = 1 << 3,          ///< a door's aperture lies on it
        CUE_SYNTHETIC = 1 << 4,     ///< a door leaf that splits the air on its own
        CUE_WEAK = 1 << 5,          ///< a stair contact with no clear end, kept so no flight is an island
    };

    struct SpaceParams
    {
        F32 mClimbM = 0.75f;        ///< floors of linked cells within this
        F32 mWalkHeadM = 1.f;       ///< a walkable gap this tall is a floor an avatar stands on
        F32 mWalkOverlapM = 1.f;    ///< and links to one sharing this much air
        F32 mStairMinRiseM = 0.5f;  ///< a flight's stair portals together must climb this: less is a step, not a staircase
        F32 mClearMaxM = 16.f;      ///< the feature transform reaches no further
        F32 mLintelDropM = 0.3f;    ///< a lintel is this far below the ceiling on both sides
        F32 mEStep = 0.3f;          ///< indoorness this different across a cut is a threshold
        F32 mRelaxed = 0.2f;        ///< both thresholds where a portal stood last time
        F32 mDoorReachM = 0.3f;     ///< a door this close to a cut is on it
        F32 mRunLinkM = 0.4f;       ///< boundary edges this close are one run
        F32 mSliverM2 = 4.f;        ///< a space smaller than this, or a tenth of its neighbour, is a sliver
        F32 mSliverShare = 0.1f;
        F32 mOutdoorS = 0.5f;       ///< spaces this open to the sky join across openings
        F32 mWalledL = 0.6f;        ///< unless one side is this walled
        F32 mOutdoorCover = 0.3f;   ///< or either is roofed over this share of its sky
        F32 mIndoorL = 0.4f;        ///< a space less walled than this is at most SHELTERED
        F32 mAvatarAreaM2 = 2.f;    ///< a floor smaller than this seeds no space, and a region smaller is a fragment
        F32 mAgentRadiusM = 0.3f;   ///< a cut narrower than two of these is no passage
        F32 mHysteresis = 0.05f;    ///< a space's bucket holds until its mean e is this far past its edge
    };

    /// A threshold between two spaces. Positions are mesh-local, at the sill.
    struct Portal
    {
        PortalKind mKind = PortalKind::OPENING;
        U32 mSpaces[2] = { 0, 0 };  ///< indices into the product's spaces
        LLVector3 mCentre;
        LLVector3 mNormal;          ///< horizontal, from the first space into the second
        LLVector3 mA;
        LLVector3 mB;
        F32 mWidth = 0.f;
        F32 mSill = 0.f;
        F32 mHead = 0.f;            ///< the lintel's underside; 0 without a lintel
        U8 mCues = 0;
        LLUUID mDoor;
        bool mCrossMesh = false;

        bool lintel() const { return (mCues & CUE_LINTEL) != 0; }
        bool walled() const { return (mCues & CUE_JAMBS) != 0; }
    };

    /// A space: an id and its measured shape, nothing it is for. Positions
    /// are mesh-local; heights and areas in metres.
    struct Space
    {
        U32 mId = 0;                ///< stable within the mesh while the space stays where it is
        EnclosureBucket mBucket = EnclosureBucket::UNKNOWN;
        LLVector3 mMin;             ///< floor plan and floor to highest ceiling, and every gap it holds
        LLVector3 mMax;
        LLVector3 mCentre;          ///< of the floor, at the lowest floor
        LLVector2 mAxis;            ///< the floor's long axis
        LLVector2 mHalf;            ///< half extents along and across it
        LLVector3 mAnchor;          ///< snapped to 1 m, what a new id is hashed from
        F32 mFloorArea = 0.f;
        F32 mVolume = 0.f;
        F32 mSurface = 0.f;
        F32 mMeanFreePath = 0.f;    ///< 4V/S
        F32 mCeilingMin = 0.f;      ///< over the closed ceilings; all zero when every one is open
        F32 mCeilingMean = 0.f;
        F32 mCeilingMax = 0.f;
        F32 mOpenShare = 0.f;       ///< of the floor, under open sky within the tile
        F32 mFloorMin = 0.f;
        F32 mFloorMax = 0.f;
        F32 mElongation = 1.f;
        F32 mSMean = 0.f;
        F32 mCoverMean = 0.f;       ///< share of the sky roofed over, walls aside, floor-weighted
        F32 mEMean = 0.f;
        F32 mEMin = 0.f;
        F32 mEMax = 0.f;
        F32 mL = 0.f;               ///< share of the boundary walled 0.75 to 2 m above the floor
        F32 mUnknown = 0.f;         ///< share of the floor with no enclosure answer
        bool mLift = false;
        bool mFlight = false;       ///< a stair flight's own space
        bool mFrozen = false;       ///< kept from an earlier pass for tiles whose detail was evicted
        std::vector<U32> mPortals;  ///< indices into the product's portals
        /// Floor area per 0.5 m x 0.5 m x 1 m cell, sorted by key: what ids are tracked by.
        std::vector<std::pair<U64, U32>> mRaster;
    };

    /// A floor cell of a plan: its minimum corner, in metres of the frame
    /// the plan lives in, at the floor z the cell holds.
    struct PlanCell
    {
        F32 mX = 0.f;
        F32 mY = 0.f;
        F32 mZ = 0.f;
    };

    /// One straight run of a plan's boundary, from one cell corner to
    /// another, the plan's cells on its left, at the floor z it ran along.
    struct PlanEdge
    {
        F32 mX0 = 0.f, mY0 = 0.f, mX1 = 0.f, mY1 = 0.f;
        F32 mZ = 0.f;
    };

    /// The outline of a set of floor cells `cell` metres wide: every side
    /// no cell of the set stands across, chained into closed loops, runs at
    /// one floor merged into one edge each. Cells repeated keep their lowest
    /// floor; where two cells of the set touch only at a corner the outline
    /// pinches and the loops part there.
    std::vector<PlanEdge> tracePlan(const std::vector<PlanCell>& cells, F32 cell);

    /// One space's draw: its pastel, the phase its outline lifts by, and the
    /// outline itself, mesh-local metres.
    struct SpaceDraw
    {
        LLColor4 mColor;            ///< alpha 1; what draws it sets its own
        F32 mLift = 0.f;            ///< the outline's lift above a fill
        std::vector<PlanEdge> mOutline;
    };

    /// What the spaces overlay draws, built once per spaces pass and
    /// immutable with the product it rides on: a fill per coarse cell and
    /// floor, and each space's traced outline, all mesh-local. The gathers
    /// cull and emit it as the camera asks, and trace nothing themselves.
    struct SpacesDraw
    {
        struct Fill
        {
            F32 mX = 0.f, mY = 0.f;     ///< the cell's min corner
            F32 mZ = 0.f;               ///< its floor
            F32 mSide = 0.f;            ///< the coarse cell's side
            U32 mSpace = 0;             ///< into mSpaces
        };
        std::vector<Fill> mFills;
        std::vector<SpaceDraw> mSpaces;     ///< with the product's spaces, and mSpaceIds
        size_t bytes() const;
    };
    typedef std::shared_ptr<const SpacesDraw> SpacesDrawPtr;

    /// The overlay's draw for a mesh's spaces, from its products: the fills
    /// the tiles' coarse gaps hold, deduplicated across the tiles' border
    /// overlap, and each space's outline traced once.
    struct RoomsProduct;
    SpacesDrawPtr buildSpacesDraw(const EnclosureProduct& enclosure, const RoomsProduct& rooms);

    /// A tile's seeds, kept for the next pass: a tile whose air did not move
    /// keeps them rather than cutting them again.
    struct SeedStats
    {
        U32 mCells = 0;             ///< level-0 floor cells
        U32 mBoundary = 0;          ///< of those, cells with a side no floor links across
        U32 mWalled = 0;            ///< of those, cells walled 0.75 to 2 m up beside them
        U32 mFlightCells = 0;
        S32 mFloorMin = 0;          ///< tile units
        S32 mFloorMax = 0;
        F64 mSumX = 0.0, mSumY = 0.0, mSumXX = 0.0, mSumXY = 0.0, mSumYY = 0.0;   ///< cell centres, mesh-local
        std::vector<std::pair<U64, U32>> mRaster;
    };

    struct RoomsTile
    {
        TileKey mKey;
        TileAirPtr mAir;            ///< the air the seeds were cut from
        std::vector<U16> mSeeds;    ///< per gap of the air, 1-based; 0 on no avatar floor
        std::vector<SeedStats> mStats;  ///< per seed
    };

    /// Two seeds joined for want of a cut rather than by cues, kept while
    /// both their tiles' air stands.
    struct SeedLink
    {
        TileKey mA;
        TileKey mB;
        U16 mSeedA = 0;             ///< 1-based, as in RoomsTile::mSeeds
        U16 mSeedB = 0;
    };

    /// The merge's runs and cut scores, each keyed by everything it read, for the next pass to reuse.
    struct MergeMemo;

    /// One mesh's spaces and portals. Positions are mesh-local.
    struct RoomsProduct
    {
        U64 mGeneration = 0;
        std::vector<Space> mSpaces;
        std::vector<Portal> mPortals;
        std::vector<RoomsTile> mTiles;      ///< sorted by key
        std::vector<SeedLink> mLinks;
        F32 mMillis = 0.f;
        U32 mSeeds = 0;
        U32 mDirtyTiles = 0;
        U32 mCells = 0;                     ///< level-0 cells the transform ran over
        U32 mReused = 0;                    ///< runs and scores taken from the last pass's memo
        SpacesDrawPtr mDraw;                ///< what the overlay draws, built once with the pass.
                                            ///< Landed products are replaced, never rewritten, and the
                                            ///< draw copies what it read, so it cannot go stale beside
                                            ///< them; a pass that would change it lands a new product.
        std::shared_ptr<const MergeMemo> mMemo;

        /// The space with this id, or null.
        const Space* byId(U32 id) const;
        const RoomsTile* tile(const TileKey& key) const;
        size_t bytes() const;
    };
    typedef std::shared_ptr<const RoomsProduct> RoomsProductPtr;

    constexpr F32 STAIR_PAIR_REACH_M = 14.f;    ///< staircase ends this far apart in plan can belong to one flight
    constexpr F32 STAIR_MAX_WIDTH_M = 12.f;     ///< a stair portal wider than this is a deck edge, not an entrance

    /// One drawn connection inside a flight: from its foot portal to an exit,
    /// both by index into the product's portals.
    struct StairLink
    {
        U32 mFrom;
        U32 mTo;
    };

    /// Pair stair portals into a flight's connections: taking the portals by
    /// ascending sill, each joins the nearest still-unpaired end within
    /// reach's plan distance, so a staircase's opposing ends meet and a
    /// multi-storey well's foot serves each of its exits. Ends with nothing
    /// in reach stay single.
    std::vector<StairLink> stairPairs(const std::vector<Portal>& portals, const std::vector<U32>& indices, F32 reach);

    /// A flight's stair portals as connections, paired by stairPairs over
    /// each flight space's own portals. A flight with a single stair portal
    /// makes no link, and neither does anything but a stair portal.
    std::vector<StairLink> stairLinks(const RoomsProduct& product, F32 reach = STAIR_PAIR_REACH_M);

    struct RoomsInput
    {
        U64 mGeneration = 0;
        U64 mMeshKey = 0;           ///< the mesh id hashed, what new space ids start from
        LLVector3d mMeshOrigin;
        std::vector<DoorEntity> mDoors;     ///< global, copied from the door table
        std::vector<std::pair<LLVector3d, LLVector3d>> mLifts;  ///< elevator shafts, global boxes
        RoomsProductPtr mPrevious;
        SpaceParams mParams;
    };

    /// The spaces pass over an enclosure product: the feature transform and
    /// watershed seeds on the level-0 floor cells of tiles whose air moved
    /// plus a ring of tiles, then over every seed of the mesh the boundary
    /// runs, their cues, merges, slivers, the outdoor rule, lifts and ids.
    /// It writes each gap's space into the enclosure product it is given.
    class SpacesPass
    {
    public:
        SpacesPass(std::shared_ptr<const RoomsInput> input, std::shared_ptr<EnclosureProduct> enclosure);
        ~SpacesPass();

        /// Do about budget units of work; true once the product is ready.
        bool step(U32 budget);
        bool done() const;
        F32 stageMillis(S32 stage) const;
        static const char* stageName(S32 stage);
        static constexpr S32 NUM_STAGES = 6;
        RoomsProductPtr product() const;

    private:
        struct State;
        std::unique_ptr<State> mState;
    };

    /// Everything at once, for tests and tools.
    RoomsProductPtr computeSpaces(std::shared_ptr<const RoomsInput> input, std::shared_ptr<EnclosureProduct> enclosure);

    const char* portalKindName(PortalKind kind);
}

#endif // SK_NAVSPACES_H
