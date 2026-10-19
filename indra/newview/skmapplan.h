/**
 * @file skmapplan.h
 * @brief The mini-map's plan: which navmesh spaces are drawn and at what storey rank, and the geometry that draws them.
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

#ifndef SK_MAPPLAN_H
#define SK_MAPPLAN_H

#include "llmath.h"
#include "v3dmath.h"
#include "v2math.h"
#include "v3math.h"
#include "lluuid.h"
#include "sknavrooms.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

/// Everything is plan-local metres: global minus the plan's origin, a region corner.
namespace skmap
{
namespace plan
{
    enum class PortalKind : U8 { DOOR, STAIR, OPENING, ADJACENT };
    enum class Bucket : U8 { OUTDOORS, SHELTERED, INDOORS, UNKNOWN };

    /// A navmesh poly edge's neighbour across a tile border and the stretch of the edge they share.
    struct BorderLink
    {
        U32 mPoly = 0;
        F32 mT0 = 0.f;
        F32 mT1 = 1.f;
    };

    struct Poly
    {
        U32 mFirst = 0;             ///< into Data::mVerts and Data::mNbr
        U8 mCount = 0;
        S32 mSpace = -1;            ///< into Data::mSpaces, -1 for none
        F32 mWater = 0.f;           ///< its region's water height
    };

    /// Neighbour codes in Data::mNbr besides a poly index.
    constexpr S32 NBR_NONE = -1;
    constexpr S32 NBR_BORDER = -2;  ///< see Data::mBorder

    struct Space
    {
        Bucket mBucket = Bucket::UNKNOWN;
        bool mRoof = false;
        bool mFlight = false;
        U64 mBuilding = 0;          ///< mesh-qualified building id, 0 for none
        S32 mStorey = -1;           ///< into Data::mStoreyFloor, -1 for none
        F32 mArea = 0.f;
        U32 mMesh = 0;
        std::vector<U32> mPortals;  ///< its cut portals, into Data::mPortals
    };

    struct Portal
    {
        PortalKind mKind = PortalKind::OPENING;
        S32 mSpaces[2] = { -1, -1 };
        LLVector3 mA;
        LLVector3 mB;
    };

    struct Flight
    {
        bool mStair = true;         ///< else a ramp
        LLVector3 mBottom[2];       ///< the flight record's end edges
        LLVector3 mTop[2];
        std::vector<U32> mPolys;
        std::vector<U32> mEnds[2];  ///< the floor polys each end meets; empty when unknown
        std::vector<std::pair<LLVector2, LLVector2>> mOutline;     ///< its polys' edges no other of its polys shares
    };

    enum class WallClass : U8 { NONE, HATCHED, SOLID };
    struct Wall
    {
        std::vector<LLVector3> mLine;   ///< floor on the left
        WallClass mClass = WallClass::NONE;
        F32 mFace = -1.f;
        F32 mFar = -1.f;
        F32 mFloor = 0.f;
        std::vector<U32> mPolys;
    };

    struct Contour
    {
        U32 mPoly = 0;
        LLVector2 mA;
        LLVector2 mB;
    };

    struct Region
    {
        LLVector2 mMin;             ///< plan-local corner
        S32 mSide = 256;            ///< metres, one height sample per metre
        F32 mWater = 0.f;
        std::vector<F32> mHeights;  ///< (side + 1)^2, row-major along y
    };

    /// One snapshot of the region meshes the map shows, with the topology the selection reads.
    struct Data
    {
        LLVector3d mOrigin;
        std::vector<LLVector3> mVerts;
        std::vector<S32> mNbr;      ///< per poly edge
        std::unordered_map<U64, std::vector<BorderLink>> mBorder;  ///< (poly << 3 | edge) -> its neighbours across the border
        std::vector<Poly> mPolys;
        std::vector<Space> mSpaces;
        std::vector<Portal> mPortals;
        std::vector<F32> mStoreyFloor;
        std::vector<std::pair<U32, U32>> mClimbs;
        std::vector<Flight> mFlights;
        std::vector<Wall> mWalls;
        std::vector<Contour> mContours;
        std::vector<Region> mRegions;
        std::vector<std::pair<LLUUID, std::unordered_map<U32, U32>>> mSpaceIndex;   ///< per mesh, space id -> index

        // Derived by finish().
        std::vector<std::vector<U32>> mSpacePolys;
        std::vector<S32> mFlightOf;                 ///< per poly
        std::vector<std::pair<U64, std::vector<U32>>> mRoofsOf;    ///< building -> its roof spaces, first-seen order
        std::vector<std::vector<U32>> mWallsOfPoly;
        std::vector<std::vector<U32>> mTouchOf;     ///< spaces sharing a poly edge with no portal between
        std::vector<std::vector<U32>> mNoSpaceNbr;  ///< space -> polys with no space sharing an edge with it
        std::vector<std::vector<U32>> mAdj;
        std::vector<std::vector<U32>> mStairAdj;
        std::vector<U8> mGround;                    ///< per space
        std::vector<U32> mGroundOrder;              ///< the ground's spaces in flood order
        std::vector<U8> mStreetRoom;
        std::vector<U64> mNoEntry;                  ///< buildings with no door or opening to anything not indoors
        std::unordered_map<U64, S32> mGroundStorey; ///< building -> its ground storey

        void finish();
        bool indoor(S32 s) const { return s >= 0 && mSpaces[s].mBucket == Bucket::INDOORS; }
        bool outdoorish(S32 s) const { return s >= 0 && mSpaces[s].mBucket != Bucket::INDOORS; }
        const LLVector3& vert(U32 poly, U32 k) const { return mVerts[mPolys[poly].mFirst + k % mPolys[poly].mCount]; }
        /// Calls fn(a, b) for each stretch of a poly edge not shared with a poly in the set.
        template <typename In, typename Fn> void edgeOutline(U32 poly, U32 edge, const In& in, const Fn& fn) const;
    };
    typedef std::shared_ptr<const Data> DataPtr;

    enum class Overlap : U8 { STOREY = 0, GROUND_ONLY = 1, ROOFS = 2 };
    enum class Outside : U8 { FULL = 0, DIM = 1, NONE = 2 };

    struct Options
    {
        Overlap mOverlap = Overlap::STOREY;
        Outside mOutside = Outside::FULL;
        bool mGaps = true;
        bool mLedges = true;
        bool mOther = true;
        bool mContours = true;
        bool mRoofOutline = true;
        bool mDoors = true;
        bool mShade = true;
        bool mTerrain = true;
        bool mRegionEdges = true;
        bool mWalls = true;
        bool mStairs = true;
        bool operator==(const Options& o) const;
    };

    /// What is drawn: spaces by storey rank, higher covering lower.
    struct Selection
    {
        std::vector<std::pair<U32, F32>> mRank;     ///< insertion order
        std::vector<S32> mRankOf;                   ///< per space, into mRank, -1 for none
        std::vector<U8> mDim;
        std::vector<std::pair<U32, F32>> mFlights;
        std::vector<U32> mOther;
        std::vector<std::pair<U32, F32>> mGapSpaces;
        std::vector<std::pair<U32, F32>> mGapPolys;
        std::vector<std::pair<LLVector3, LLVector3>> mRoofLines;
        std::vector<U32> mRoofPolys;
        bool mOnGround = false;
        bool mInside = false;
        U64 mHash = 0;

        bool has(S32 s) const { return s >= 0 && mRankOf[s] >= 0; }
        F32 rank(U32 s) const { return mRank[mRankOf[s]].second; }
        void set(U32 s, F32 r);
    };

    /// The viewer's space (-1 off every space) and which mesh it stands in. Standing on an outdoor space off the ground
    /// level (a deck or balcony), the plan adds that space's outdoor set and the storeys its DOOR and OPENING portals
    /// lead into, as the ground does with its street entrances.
    Selection select(const Data& data, S32 viewer_space, const Options& opts, bool standing = false);

    /// One vertex of every composite pass: position, colour, then per-pass parameters.
    struct Vertex
    {
        F32 mP[3];
        F32 mC[4];
        F32 mK[4];
    };

    struct Geometry
    {
        struct Rank
        {
            F32 mRank = 0.f;
            std::vector<Vertex> mFloors;    ///< k: kind, outdoor, water height
            std::vector<Vertex> mWalls;     ///< poché, k: 0 full, 1 half
            std::vector<Vertex> mWallLines; ///< depth-tested at the run's floor
            std::vector<Vertex> mLines;     ///< over the rank, in draw order
        };
        std::vector<Rank> mRanks;           ///< low to high
        std::vector<Vertex> mShadowClip;
        std::vector<Vertex> mShadowBand;
        std::vector<Vertex> mTop;           ///< region edges, over everything
        U32 mWallRuns = 0;
        size_t triangles() const;
    };

    /// How much of a line layer shows at a zoom, by metres per pixel: 1 up to full_mpp, 0 from gone_mpp, and a smoothstep
    /// in log2(mpp) between, so a zoom moves it smoothly and never pops.
    inline F32 zoomFade(F32 mpp, F32 full_mpp, F32 gone_mpp)
    {
        const F32 t = llclamp((std::log2(llmax(mpp, 1e-6f)) - std::log2(full_mpp)) / (std::log2(gone_mpp) - std::log2(full_mpp)), 0.f, 1.f);
        return 1.f - t * t * (3.f - 2.f * t);
    }
    constexpr F32 ZOOM_DETAIL_MPP = 0.125f;     ///< closer than this (32 m across 256 px) every line is as drawn
    constexpr F32 ZOOM_LEDGE_GONE_MPP = 0.5f;   ///< ledges and drops are gone from here (128 m across 256 px)
    constexpr F32 ZOOM_WALL_GONE_MPP = 1.f;     ///< wall outlines, contours and the half-cover hatch from here; walls are fill
    /// The composite's geometry for a selection, line widths in texels of mpp metres.
    void buildGeometry(const Data& data, const Selection& sel, const Options& opts, F32 mpp, Geometry& out);

    /// What a gather over the box would read: moves when any of it does. Main thread.
    U64 gatherKey(const LLVector3d& min, const LLVector3d& max);
    /// What gatherKey mixes, part by part, so the map recorder can say which part moved. Main thread.
    struct GatherParts
    {
        struct Mesh
        {
            LLUUID mId;
            LLVector3d mMin, mMax;
            U64 mRoomsGeneration = 0;
            bool mResident = false;
            U64 mInputs = 0;
        };
        std::vector<Mesh> mMeshes;
        std::vector<std::pair<U64, F32>> mRegions;  ///< handle, water height
    };
    void gatherKeyParts(const LLVector3d& min, const LLVector3d& max, GatherParts& out);
    /// What a plan is built from: the region meshes meeting a box, whole, and the regions' land.
    struct Snapshot;
    typedef std::shared_ptr<const Snapshot> SnapshotPtr;

    /// Reads snapshots on the main thread without building anything there: the navmesh's products by shared pointer
    /// (storeys and wall runs as last built; stale ones build on the navmesh's workers), and per tile what has to be
    /// copied, read again only for tiles whose inputs moved.
    class Gatherer
    {
    public:
        Gatherer();
        ~Gatherer();
        SnapshotPtr snapshot(const LLVector3d& origin, const LLVector3d& min, const LLVector3d& max);

    private:
        struct Cache;
        std::unique_ptr<Cache> mCache;
    };

    struct SnapshotStats
    {
        U32 mTilesRead = 0;     ///< tiles whose border links were read again
        U32 mRegions = 0;
        U32 mMeshes = 0;
    };
    SnapshotStats stats(const Snapshot& snap);

    /// The plan's data from a snapshot, plan-local to its origin. Any thread.
    DataPtr assemble(const Snapshot& snap);

    /// A space by its mesh and id; mId 0 for none.
    struct SpaceRef
    {
        LLUUID mMesh;
        U32 mId = 0;
        bool operator==(const SpaceRef& o) const { return mMesh == o.mMesh && mId == o.mId; }
        bool operator!=(const SpaceRef& o) const { return !(*this == o); }
    };
    /// Where a global position is: the fine air gap holding it (SKNavWorldView::airGapAt) with its space, bucket,
    /// floor and building. Main thread.
    struct Whereabouts
    {
        bool mFound = false;
        SpaceRef mSpace;            ///< mId 0 for a gap in no space
        bool mIndoor = false;       ///< mBucket is INDOORS
        Bucket mBucket = Bucket::UNKNOWN; ///< the gap's bucket; never UNKNOWN when mFound
        F32 mFloor = 0.f;           ///< global
        F32 mCeiling = 1e30f;       ///< global; 1e30 under open sky
        bool mFine = false;         ///< read from detail air; always true when mFound
        U32 mBuilding = 0;          ///< the space's building, 0 in none
        bool mRoof = false;         ///< the space is a building's roof
    };
    /// Where someone standing with their feet at feet is: the air just over the feet, which must be fine, known
    /// and floored within band_below under to band_above over the feet, since a foot sunk into a roof reads the
    /// room under it. Not found otherwise: coarse air, an unloaded region or unsettled rooms give no guess.
    Whereabouts whereabouts(const LLVector3d& feet, F32 band_below = 1.0f, F32 band_above = 1.0f);
    /// whereabouts judged from a column already read at the feet's plan, in world.
    Whereabouts whereabouts(const SKNavWorldView& world, const SKNavWorldView::AirColumn& column, const LLVector3d& feet,
                            F32 band_below = 1.0f, F32 band_above = 1.0f);
    /// Whether the body standing at feet is in outdoor air: the first known gap 0.3, 0.8 or 1.3 m over the feet,
    /// as enclosureAt reads them, so a sunk foot does not read the room under it. UNKNOWN is never outdoors.
    bool outdoorsIn(const SKNavWorldView::AirColumn& column, const LLVector3d& feet);
    /// The floor under the agent standing at at with its feet at feet: the navmesh's when polygons are built
    /// and it lies near the feet, else a known fine air gap's floored at or just under the feet, else, off the
    /// floor in a jump or a fall, an indoor fine gap's up to 4 m under them. False with no answer. Main thread.
    bool agentFloor(const LLVector3d& at, F64 feet, F32& floor_out);
    /// A space as a Data index, -1 for none. Any thread.
    S32 spaceIndex(const Data& data, const SpaceRef& ref);
}
}

template <typename In, typename Fn> void skmap::plan::Data::edgeOutline(U32 poly, U32 edge, const In& in, const Fn& fn) const
{
    const LLVector3& a = vert(poly, edge);
    const LLVector3& b = vert(poly, edge + 1);
    const S32 n = mNbr[mPolys[poly].mFirst + edge];
    if (n >= 0)
    {
        if (!in(static_cast<U32>(n)))
        {
            fn(a, b);
        }
        return;
    }
    if (n == NBR_NONE)
    {
        fn(a, b);
        return;
    }
    // Across a tile border: the stretches no neighbour of the set covers.
    std::vector<std::pair<F32, F32>> cover;
    const auto it = mBorder.find((static_cast<U64>(poly) << 3) | edge);
    if (it != mBorder.end())
    {
        for (const BorderLink& l : it->second)
        {
            if (in(l.mPoly))
            {
                cover.emplace_back(l.mT0, l.mT1);
            }
        }
    }
    std::sort(cover.begin(), cover.end());
    F32 t = 0.f;
    for (const auto& c : cover)
    {
        if (c.first > t)
        {
            fn(a + (b - a) * t, a + (b - a) * c.first);
        }
        t = llmax(t, c.second);
    }
    if (t < 1.f)
    {
        fn(a + (b - a) * t, b);
    }
}

#endif // SK_MAPPLAN_H
