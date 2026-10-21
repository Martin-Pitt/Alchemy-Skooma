/**
 * @file sknavair.h
 * @brief The open air of a tile: every gap above a solid, per column, off the navmesh heightfield.
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

#ifndef SK_NAVAIR_H
#define SK_NAVAIR_H

#include "stdtypes.h"
#include "v3math.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

struct rcHeightfield;

namespace sknav
{
    /// One open gap in a column: from the top of a solid to the bottom of the
    /// next one, or to the top of the tile's height range when nothing is above.
    /// Heights are in grid units from the grid's origin z. A floor may read up
    /// to one unit high: Recast rounds a span's top up, where its bottom (the
    /// ceiling here) is rounded down and exact.
    struct AirGap
    {
        enum : U8
        {
            TOP = 1 << 0,       ///< the solid under the gap is floor at the air's slope and headroom
            OPEN = 1 << 1,      ///< nothing above within the tile; mCeiling is the tile's top
            OWN_SKY = 1 << 2,   ///< nothing above within this tile's column: the column's top gap
            FLIGHT = 1 << 3,    ///< on a stair flight; reserved, filled by the flight records
            AREA_SHIFT = 4,     ///< two bits of the Recast area under the gap, 0 when it is not floor
            AREA_MASK = 3 << AREA_SHIFT,
            AVATAR_FLOOR = 1 << 6,  ///< AVATAR walks the solid under the gap, before its erosion
        };
        U16 mFloor = 0;
        U16 mCeiling = 0;
        U8 mFlags = 0;
        U8 mClear = 0;          ///< lateral clearance in CLEAR_UNIT_M, capped at CLEAR_MAX
        S8 mSlopeX = 0;         ///< a merged floor's rise per cell along x, in 1/AIR_SLOPE_Q units; 0 in a fine grid
        S8 mSlopeY = 0;

        U8 area() const { return (mFlags & AREA_MASK) >> AREA_SHIFT; }
    };

    constexpr S32 AIR_SLOPE_Q = 64;     ///< slope steps per height unit per cell

    /// Division rounding towards negative infinity, for positive b.
    inline S64 floorDiv(S64 a, S64 b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    constexpr F32 CLEAR_UNIT_M = 0.125f;
    constexpr U8 CLEAR_MAX = 16;        ///< 2 m: past that a gap is simply clear

    /// The gaps of every column of a tile proper (its border left out), at the
    /// heightfield's own cell size. Rows run along SL y, columns along SL x:
    /// column (ix, iy) covers mOrigin + (ix, iy) * mCell to one cell further.
    /// What rooms and enclosure are computed from: where a heightfield only
    /// says where solid is, this says where the air is and how tall it is.
    struct AirGrid
    {
        LLVector3 mOrigin;              ///< mesh-local SL; z is where height 0 is
        F32 mCell = 0.f;                ///< metres per column
        F32 mUnit = 0.f;                ///< metres per height unit
        S32 mWidth = 0;                 ///< columns along SL x
        S32 mHeight = 0;                ///< along SL y
        std::vector<U32> mStart;        ///< mWidth * mHeight + 1 offsets into mGaps, row-major along y
        std::vector<AirGap> mGaps;      ///< lowest first within a column

        bool empty() const { return mGaps.empty(); }
        U32 count(S32 ix, S32 iy) const { return mStart[ix + iy * mWidth + 1] - mStart[ix + iy * mWidth]; }
        const AirGap* gaps(S32 ix, S32 iy) const { return mGaps.data() + mStart[ix + iy * mWidth]; }
        F32 height(U16 units) const { return mOrigin.mV[VZ] + units * mUnit; }
    };

    /// Read the gaps off a heightfield after its filters have run, so the
    /// walkable flag is the class's own. Gaps below the lowest solid are not
    /// air anything stands in and are left out.
    /// Flags to add to the gap over each span of a heightfield, column by
    /// column in the heightfield's order, bottom up.
    struct SpanFlags
    {
        std::vector<U32> mStart;    ///< per column, where its spans' flags begin; columns + 1
        std::vector<U8> mFlags;
    };

    /// A raster whose spans carry codes passes the code -> area table as area_lut.
    void extractAir(const rcHeightfield& hf, S32 border_cells, AirGrid& out, const U8* area_lut = nullptr, const SpanFlags* extra = nullptr);

    /// Whether two gaps of lateral neighbours connect: floors within climb
    /// and at least overlap units of air shared above both.
    inline bool airAdjacent(const AirGap& a, const AirGap& b, S32 climb, S32 overlap)
    {
        const S32 lo = std::max<S32>(a.mFloor, b.mFloor), hi = std::min<S32>(a.mCeiling, b.mCeiling);
        return std::abs(static_cast<S32>(a.mFloor) - static_cast<S32>(b.mFloor)) <= climb && hi - lo >= overlap;
    }

    /// Chamfer distance (Recast's 2/3 kernel) from every gap to the nearest
    /// side where air stops: a lateral neighbour column inside the grid with
    /// no gap adjacent to it. Columns off the grid's edge are not walls, so a
    /// grid padded by CLEAR_MAX past a tile gives exact values on the tile.
    void computeClearance(AirGrid& grid, S32 climb, S32 overlap);

    /// Fold every closed gap shorter than min_height units into the solid
    /// around it: slivers between stacked or overlapping prims are no air
    /// anything moves through, and would read as walls to their neighbours.
    void foldSlivers(AirGrid& grid, S32 min_height);

    /// The grid with pad columns taken off every side.
    void cropAir(const AirGrid& in, S32 pad, AirGrid& out);
}

#endif // SK_NAVAIR_H
