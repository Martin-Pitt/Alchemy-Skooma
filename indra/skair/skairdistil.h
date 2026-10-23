/**
 * @file skairdistil.h
 * @brief A tile's air distilled into per-gap quadtrees, its seam strips and its solid tops.
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

#ifndef SK_AIRDISTIL_H
#define SK_AIRDISTIL_H

#include "skair.h"
#include "skairfaceoffsets.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace skworld
{
    struct AirBuildStats;

    constexpr S32 AIR_MAX_LEVEL = 4;            ///< a level-4 leaf is 16 cells, 2 m at 0.125 m
    constexpr U16 AIR_HEIGHT_MAX = 8191;        ///< heights are 13 bits, 1023.9 m at 0.125 m
    constexpr U16 AIR_CEILING_OPEN = 0xffff;    ///< a distilled ceiling with nothing above in the tile
    constexpr F32 AIR_PASS_M = 1.f;             ///< a closed, unwalkable gap lower than this is no way through
    constexpr S32 AIR_PLANE_TOL = 1;            ///< units a walkable floor may lie under its merged plane

    /// A square block of 2^mLevel cells, from (mX, mY) in cells of the tile,
    /// over which mGapCount gaps from mFirstGap agree, lowest first. Leaves
    /// nest: a finer leaf inside a coarser one holds the gaps of its columns
    /// that the coarser one does not, so a column's gaps are those of every
    /// leaf over it. A merged gap's floor is a plane through its leaf's
    /// centre (airFloorAt).
    struct AirLeaf
    {
        U16 mX = 0;
        U16 mY = 0;
        U8 mLevel = 0;
        U8 mGapCount = 0;
        U16 mParent = 0;        ///< how many leaves back the nearest coarser leaf over this one is; 0 for none
        U32 mFirstGap = 0;
    };

    /// The Morton key of a cell: x in the even bits, y in the odd ones.
    inline U32 airKey(U32 x, U32 y)
    {
        auto spread = [](U32 v) {
            v &= 0xffff;
            v = (v | (v << 8)) & 0x00ff00ff;
            v = (v | (v << 4)) & 0x0f0f0f0f;
            v = (v | (v << 2)) & 0x33333333;
            return (v | (v << 1)) & 0x55555555;
        };
        return spread(x) | (spread(y) << 1);
    }

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

    /// The bytes of every finished TileAir, its own and not its tops', and of every TileTops, alive in the process.
    extern std::atomic<size_t> gAirLiveBytes;
    extern std::atomic<size_t> gAirTopsBytes;

    /// What a finished air or tops adds to one of those totals until it is destroyed; a copy starts uncounted.
    class LiveBytes
    {
    public:
        explicit LiveBytes(std::atomic<size_t>& total) : mTotal(&total) {}
        LiveBytes(const LiveBytes& other) : mTotal(other.mTotal) {}
        LiveBytes& operator=(const LiveBytes&) { return *this; }
        ~LiveBytes() { mTotal->fetch_sub(mBytes, std::memory_order_relaxed); }
        /// Counts it as this many bytes, in place of what it was counted as before.
        void count(size_t bytes)
        {
            mTotal->fetch_add(bytes, std::memory_order_relaxed);
            mTotal->fetch_sub(mBytes, std::memory_order_relaxed);
            mBytes = bytes;
        }
        size_t counted() const { return mBytes; }

    private:
        std::atomic<size_t>* mTotal;
        size_t mBytes = 0;
    };

    /// A tile's solid tops over 1 m cells, in its air's frame: shared by the detail air and by the coarse air
    /// an evicted tile keeps, and all the sky map holds of either.
    struct TileTops
    {
        LLVector3 mOrigin;              ///< the air's
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        /// mTopCell cells a side, row-major along y.
        S32 mTopCell = 0;
        S32 mTopWidth = 0;
        S32 mTopHeight = 0;
        std::vector<U32> mTopStart;
        std::vector<TopInterval> mTop;
        LiveBytes mLive{ gAirTopsBytes };

        size_t bytes() const;
        void countLive() { mLive.count(bytes()); }
    };
    typedef std::shared_ptr<const TileTops> TileTopsPtr;
    /// The tops of an air that has none, shared.
    const TileTopsPtr& noTops();

    /// A tile's air, immutable once built. Heights are units from mOrigin z,
    /// clipped to AIR_HEIGHT_MAX; an open ceiling reads AIR_CEILING_OPEN.
    /// A number no other air in the process has had, a copy included: what keys hash for an air, as an
    /// address can be freed and taken by another.
    struct AirSerial
    {
        U64 mValue = next();
        AirSerial() = default;
        AirSerial(const AirSerial&) : mValue(next()) {}
        AirSerial& operator=(const AirSerial&) { mValue = next(); return *this; }
        static U64 next()
        {
            static std::atomic<U64> counter{ 0 };
            return ++counter;
        }
    };

    struct TileAir
    {
        enum Side : U8 { SIDE_NEG_X = 0, SIDE_POS_X = 1, SIDE_NEG_Y = 2, SIDE_POS_Y = 3 };

        LLVector3 mOrigin;              ///< mesh-local SL; z is where height 0 is
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        S32 mWidth = 0;                 ///< cells along SL x
        S32 mHeight = 0;                ///< along SL y
        std::vector<AirLeaf> mLeaves;   ///< leaves with air only, in airKey order of their origins, coarsest first at a shared origin
        std::vector<AirGap> mGaps;
        AirSerial mSerial;
        /// -x and +x run along y, -y and +y along x.
        SeamStrip mSeams[4];
        /// Solid tops over 1 m cells, never null.
        TileTopsPtr mTops = noTops();
        /// Where walls' true faces sit inside their solid cells (skairfaceoffsets.h); empty for coarse air.
        std::vector<FaceOffset> mFaceOffsets;
        LiveBytes mLive{ gAirLiveBytes };

        /// Its own bytes, without the tops it shares.
        size_t bytes() const;
        /// Counts it in gAirLiveBytes, once it is built and again if anything is added before it is shared.
        void countLive() { mLive.count(bytes()); }
    };
    typedef std::shared_ptr<const TileAir> TileAirPtr;

    /// The finest leaf covering a fine cell, or -1, from the last leaf whose
    /// key is at most the cell's; the coarser ones over it follow parentLeaf.
    S32 findLeafByKey(const TileAir& air, S32 ix, S32 iy);

    /// Whether two airs hold the same cells, gaps, seams, tops and face offsets, whatever their serials: a tile
    /// built again to the same air can keep the one it has, and nothing that reads it need run again.
    bool sameAir(const TileAir& a, const TileAir& b);

    /// The nearest coarser leaf over a leaf, or -1.
    inline S32 parentLeaf(const TileAir& air, S32 leaf)
    {
        return air.mLeaves[leaf].mParent ? leaf - air.mLeaves[leaf].mParent : -1;
    }

    /// A gap's floor at a cell of the leaf holding it: its plane's height at the cell's centre.
    inline S32 airFloorAt(const AirGap& g, const AirLeaf& leaf, S32 ix, S32 iy)
    {
        if (!g.mSlopeX && !g.mSlopeY)
        {
            return g.mFloor;
        }
        const S32 s = 1 << leaf.mLevel;
        const S32 rise = g.mSlopeX * (2 * (ix - leaf.mX) + 1 - s) + g.mSlopeY * (2 * (iy - leaf.mY) + 1 - s);
        return g.mFloor + static_cast<S32>(floorDiv(rise + AIR_SLOPE_Q, 2 * AIR_SLOPE_Q));
    }

    /// A floor read off a plane as a stored height.
    inline U16 airFloorUnits(S32 units)
    {
        return static_cast<U16>(std::clamp<S32>(units, 0, AIR_HEIGHT_MAX));
    }

    /// A column's gaps gathered from every leaf over it, lowest first.
    struct AirColumn
    {
        U32 mCount = 0;
        U32 mGap[0xff];     ///< indices into TileAir::mGaps
        S32 mLeaf[0xff];    ///< the leaf holding each

        /// The gap's floor at the column's cell.
        S32 floorAt(const TileAir& air, U32 k, S32 ix, S32 iy) const;
    };

    /// The gaps over a fine cell, starting from its finest leaf; none when leaf is negative.
    void airColumn(const TileAir& air, S32 leaf, AirColumn& out);

    /// Sort leaves into key order, coarsest first at a shared origin, and link each to its parent.
    void linkLeaves(TileAir& air);

    /// Merge the fine grid's gaps into leaves of up to AIR_MAX_LEVEL, gap by
    /// gap: a gap merges over a block when every column has one gap matching
    /// it, with equal flags, floors and ceilings each within climb (walkable
    /// floors within half of one), and none is a usable gap (walkable, open
    /// or AIR_PASS_M tall) touching a wall. The other gaps of those columns stay
    /// in finer leaves, and a gap beside an overlapping one is at most one
    /// level coarser. A merged gap keeps the highest floor, the lowest ceiling
    /// and the least clearance of those it replaced.
    void distilAir(const AirGrid& grid, S32 climb, TileAir& out);

    /// The seam strips of a tile's fine grid.
    void airSeams(const AirGrid& grid, TileAir& out);

    /// The solid runs of the raster inside the border, merged over
    /// cells_per_top columns a side.
    void airTops(const ColumnRaster& raster, S32 border_cells, S32 cells_per_top, TileAir& out);

    /// Everything a tile's air is built from the solid raster: gaps
    /// read pad_cells past the tile so clearance is exact on it, slivers
    /// under min_gap units folded away, then the distil, seams and tops. Null when the tile has no air.
    /// fine, when given, gets the fine grid the air was distilled from;
    /// extracted, when not empty, is taken as the padded gaps already read and folded.
    TileAirPtr buildTileAir(const ColumnRaster& raster, S32 border_cells, S32 pad_cells, S32 climb, S32 overlap, S32 min_gap,
                            AirBuildStats* stats = nullptr, AirGrid* fine = nullptr, AirGrid* extracted = nullptr);

    /// A coarse tile's air in cells and height units factor times finer, so the
    /// passes read it beside a fine tile's: each leaf factor times as wide,
    /// split into leaves of AIR_MAX_LEVEL past it, heights times factor.
    /// Null unless factor is a power of two.
    TileAirPtr upsampleAir(const TileAir& coarse, S32 factor);

    /// A compact little-endian form of the records, and a JSON object naming
    /// the sections in the order they appear and each record's layout.
    std::vector<U8> packTileAir(const TileAir& air);
    std::string tileAirLayout(const TileAir& air);
}

#endif // SK_AIRDISTIL_H
