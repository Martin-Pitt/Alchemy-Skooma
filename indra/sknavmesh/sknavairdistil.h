/**
 * @file sknavairdistil.h
 * @brief A tile's air distilled into a restricted quadtree, its seam strips and its solid tops.
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

#ifndef SK_NAVAIRDISTIL_H
#define SK_NAVAIRDISTIL_H

#include "sknavair.h"
#include "sknavflights.h"

#include <memory>
#include <string>
#include <vector>

struct rcHeightfield;

namespace sknav
{
    struct AirBuildStats;

    constexpr S32 AIR_MAX_LEVEL = 4;            ///< a level-4 leaf is 16 cells, 2 m at 0.125 m
    constexpr U16 AIR_HEIGHT_MAX = 8191;        ///< heights are 13 bits, 1023.9 m at 0.125 m
    constexpr U16 AIR_CEILING_OPEN = 0xffff;    ///< a distilled ceiling with nothing above in the tile
    constexpr F32 AIR_PASS_M = 1.f;             ///< a closed, unwalkable gap lower than this is no way through

    /// A square block of 2^mLevel cells whose gaps agree, from (mX, mY) in
    /// cells of the tile. Its gaps are mGapCount records from mFirstGap.
    struct AirLeaf
    {
        U16 mX = 0;
        U16 mY = 0;
        U8 mLevel = 0;
        U8 mGapCount = 0;
        U32 mFirstGap = 0;
    };

    /// The gaps of the level-0 cells along one tile edge, in order along it,
    /// for stitching to the neighbour without its quadtree.
    struct SeamStrip
    {
        std::vector<U32> mStart;        ///< cells + 1 offsets into mGaps
        std::vector<AirGap> mGaps;

        U32 count(S32 i) const { return mStart[i + 1] - mStart[i]; }
    };

    /// A solid run in a column, in the tile's height units.
    struct TopInterval
    {
        U16 mBottom = 0;
        U16 mTop = 0;
    };

    /// A tile's air, immutable once built. Heights are units from mOrigin z,
    /// clipped to AIR_HEIGHT_MAX; an open ceiling reads AIR_CEILING_OPEN.
    struct TileAir
    {
        enum Side : U8 { SIDE_NEG_X = 0, SIDE_POS_X = 1, SIDE_NEG_Y = 2, SIDE_POS_Y = 3 };

        LLVector3 mOrigin;              ///< mesh-local SL; z is where height 0 is
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        S32 mWidth = 0;                 ///< cells along SL x
        S32 mHeight = 0;                ///< along SL y
        std::vector<AirLeaf> mLeaves;   ///< leaves with air only, row-major by origin
        std::vector<AirGap> mGaps;
        /// -x and +x run along y, -y and +y along x.
        SeamStrip mSeams[4];
        /// Solid tops over 1 m cells: mTopCell cells a side, row-major along y.
        S32 mTopCell = 0;
        S32 mTopWidth = 0;
        S32 mTopHeight = 0;
        std::vector<U32> mTopStart;
        std::vector<TopInterval> mTop;

        size_t bytes() const;
    };
    typedef std::shared_ptr<const TileAir> TileAirPtr;

    /// Merge the fine grid into leaves of up to AIR_MAX_LEVEL. A block
    /// merges when every column has the same gaps with equal flags, floors
    /// and ceilings each within climb, and no usable gap (walkable, open or AIR_PASS_M tall) touching a wall; leaves
    /// that touch differ by at most one level. A merged gap keeps the highest
    /// floor, the lowest ceiling and the least clearance of those it replaced.
    void distilAir(const AirGrid& grid, S32 climb, TileAir& out);

    /// The seam strips of a tile's fine grid.
    void airSeams(const AirGrid& grid, TileAir& out);

    /// The solid runs of the heightfield inside the border, merged over
    /// cells_per_top columns a side.
    void airTops(const rcHeightfield& hf, S32 border_cells, S32 cells_per_top, TileAir& out);

    /// Everything a tile's air is built from its WEATHER heightfield: gaps
    /// read pad_cells past the tile so clearance is exact on it, slivers
    /// under min_gap units folded away, the treads of any flights marked,
    /// then the distil, seams and tops. Null when the tile has no air.
    TileAirPtr buildTileAir(const rcHeightfield& hf, S32 border_cells, S32 pad_cells, S32 climb, S32 overlap, S32 min_gap,
                            const std::vector<Flight>* flights = nullptr, AirBuildStats* stats = nullptr);

    /// A compact little-endian form of the records, and a JSON object naming
    /// the sections in the order they appear and each record's layout.
    std::vector<U8> packTileAir(const TileAir& air);
    std::string tileAirLayout(const TileAir& air);
}

#endif // SK_NAVAIRDISTIL_H
