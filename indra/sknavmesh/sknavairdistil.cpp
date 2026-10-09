/**
 * @file sknavairdistil.cpp
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

#include "linden_common.h"

#include "sknavairdistil.h"
#include "sknavbuildstats.h"

#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace
{
    using namespace sknav;

    /// A gap as a block of them sees it: the range its floors and ceilings span.
    struct GapAgg
    {
        U16 mFloorMin = 0;
        U16 mFloorMax = 0;
        U16 mCeilMin = 0;
        U16 mCeilMax = 0;
        U8 mFlags = 0;
        U8 mClear = 0;
    };

    struct Block
    {
        bool mLeaf = false;     ///< the block can stand as one leaf
        bool mMerge = false;    ///< and can be merged into its parent
        U8 mCount = 0;
        U32 mFirst = 0;
    };

    struct Level
    {
        S32 mWidth = 0;
        S32 mHeight = 0;
        std::vector<Block> mBlocks;
        std::vector<GapAgg> mGaps;
    };

    U16 clipHeight(U16 units)
    {
        return std::min(units, AIR_HEIGHT_MAX);
    }

    AirGap record(const GapAgg& a)
    {
        AirGap g;
        g.mFloor = a.mFloorMax;
        g.mCeiling = (a.mFlags & AirGap::OPEN) ? AIR_CEILING_OPEN : a.mCeilMin;
        g.mFlags = a.mFlags;
        g.mClear = a.mClear;
        return g;
    }

    AirGap record(const AirGap& fine)
    {
        AirGap g = fine;
        g.mFloor = clipHeight(fine.mFloor);
        g.mCeiling = (fine.mFlags & AirGap::OPEN) ? AIR_CEILING_OPEN : clipHeight(fine.mCeiling);
        return g;
    }

    template<typename T> void put(std::vector<U8>& out, T v)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            out.push_back(static_cast<U8>((static_cast<U64>(v) >> (8 * i)) & 0xff));
        }
    }

    void putGap(std::vector<U8>& out, const AirGap& g)
    {
        put<U16>(out, g.mFloor);
        put<U16>(out, g.mCeiling);
        put<U8>(out, g.mFlags);
        put<U8>(out, g.mClear);
        put<U16>(out, g.mSeed);
    }
}

namespace sknav
{
    size_t TileAir::bytes() const
    {
        size_t n = sizeof(TileAir) + mLeaves.capacity() * sizeof(AirLeaf) + mGaps.capacity() * sizeof(AirGap) +
                   mTopStart.capacity() * sizeof(U32) + mTop.capacity() * sizeof(TopInterval) +
                   mFaceOffsets.capacity() * sizeof(FaceOffset);
        for (const SeamStrip& s : mSeams)
        {
            n += s.mStart.capacity() * sizeof(U32) + s.mGaps.capacity() * sizeof(AirGap);
        }
        return n;
    }

    S32 findLeafByKey(const TileAir& air, S32 ix, S32 iy)
    {
        if (ix < 0 || iy < 0 || ix >= air.mWidth || iy >= air.mHeight)
        {
            return -1;
        }
        const U32 key = airKey(ix, iy);
        const auto it = std::upper_bound(air.mLeaves.begin(), air.mLeaves.end(), key,
                                         [](U32 k, const AirLeaf& l) { return k < airKey(l.mX, l.mY); });
        if (it == air.mLeaves.begin())
        {
            return -1;
        }
        // The predecessor lies inside the finest leaf over the cell, if any, so that leaf is it or one of its parents.
        S32 leaf = static_cast<S32>(it - 1 - air.mLeaves.begin());
        while (leaf >= 0)
        {
            const AirLeaf& l = air.mLeaves[leaf];
            const S32 s = 1 << l.mLevel;
            if (ix < l.mX + s && iy < l.mY + s)
            {
                return leaf;
            }
            leaf = parentLeaf(air, leaf);
        }
        return -1;
    }

    void airColumn(const TileAir& air, S32 leaf, AirColumn& out)
    {
        out.mCount = 0;
        for (; leaf >= 0; leaf = parentLeaf(air, leaf))
        {
            const AirLeaf& l = air.mLeaves[leaf];
            for (U32 i = 0; i < l.mGapCount && out.mCount < 0xff; ++i)
            {
                // Insert by floor: a column's gaps are disjoint, and few.
                const U32 g = l.mFirstGap + i;
                U32 at = out.mCount++;
                while (at > 0 && air.mGaps[out.mGap[at - 1]].mFloor > air.mGaps[g].mFloor)
                {
                    out.mGap[at] = out.mGap[at - 1];
                    --at;
                }
                out.mGap[at] = g;
            }
        }
    }

    void linkLeaves(TileAir& air)
    {
        // Keys alone tie a coarse leaf with the first finer one at its origin; gaps move with their leaves untouched.
        std::stable_sort(air.mLeaves.begin(), air.mLeaves.end(), [](const AirLeaf& a, const AirLeaf& b) {
            const U32 ka = airKey(a.mX, a.mY), kb = airKey(b.mX, b.mY);
            return ka < kb || (ka == kb && a.mLevel > b.mLevel);
        });
        std::vector<S32> open;
        for (S32 i = 0; i < static_cast<S32>(air.mLeaves.size()); ++i)
        {
            AirLeaf& l = air.mLeaves[i];
            while (!open.empty())
            {
                const AirLeaf& o = air.mLeaves[open.back()];
                const S32 s = 1 << o.mLevel;
                if (l.mX >= o.mX && l.mY >= o.mY && l.mX < o.mX + s && l.mY < o.mY + s)
                {
                    break;
                }
                open.pop_back();
            }
            l.mParent = static_cast<U16>(open.empty() ? 0 : i - open.back());
            open.push_back(i);
        }
    }

    void distilAir(const AirGrid& grid, S32 climb, TileAir& out)
    {
        out.mOrigin = grid.mOrigin;
        out.mCell = grid.mCell;
        out.mUnit = grid.mUnit;
        out.mWidth = grid.mWidth;
        out.mHeight = grid.mHeight;
        out.mLeaves.clear();
        out.mGaps.clear();
        const S32 w = grid.mWidth, h = grid.mHeight;
        if (w <= 0 || h <= 0)
        {
            return;
        }

        const S32 pass = static_cast<S32>(std::ceil(AIR_PASS_M / std::max(grid.mUnit, 1e-3f)));
        // Level 0 is the grid itself; each level above merges 2 x 2 blocks
        // of the one below when all four may merge and agree with each other.
        Level levels[AIR_MAX_LEVEL + 1];
        {
            Level& l0 = levels[0];
            l0.mWidth = w;
            l0.mHeight = h;
            l0.mBlocks.resize(static_cast<size_t>(w) * h);
            l0.mGaps.reserve(grid.mGaps.size());
            for (S32 iy = 0; iy < h; ++iy)
            {
                for (S32 ix = 0; ix < w; ++ix)
                {
                    Block& b = l0.mBlocks[ix + iy * w];
                    const U32 count = grid.count(ix, iy);
                    if (count > 0xff)
                    {
                        LL_WARNS_ONCE("SKNav") << "an air column has " << count << " gaps; keeping the lowest 255" << LL_ENDL;
                    }
                    b.mLeaf = true;
                    b.mMerge = count <= 0xff;
                    b.mCount = static_cast<U8>(std::min<U32>(count, 0xff));
                    b.mFirst = static_cast<U32>(l0.mGaps.size());
                    const AirGap* g = grid.gaps(ix, iy);
                    for (U32 i = 0; i < b.mCount; ++i)
                    {
                        // Only air something can use makes a wall matter.
                        const bool passable = (g[i].mFlags & (AirGap::TOP | AirGap::OPEN)) || g[i].mCeiling - g[i].mFloor >= pass;
                        GapAgg a;
                        a.mFloorMin = a.mFloorMax = clipHeight(g[i].mFloor);
                        a.mCeilMin = a.mCeilMax = clipHeight(g[i].mCeiling);
                        a.mFlags = g[i].mFlags;
                        a.mClear = g[i].mClear;
                        b.mMerge = b.mMerge && (g[i].mClear > 0 || !passable);  // a usable gap touching a wall stays fine
                        l0.mGaps.push_back(a);
                    }
                }
            }
        }
        for (S32 lv = 1; lv <= AIR_MAX_LEVEL; ++lv)
        {
            const Level& below = levels[lv - 1];
            Level& l = levels[lv];
            l.mWidth = (below.mWidth + 1) / 2;
            l.mHeight = (below.mHeight + 1) / 2;
            l.mBlocks.resize(static_cast<size_t>(l.mWidth) * l.mHeight);
            for (S32 by = 0; by < l.mHeight; ++by)
            {
                for (S32 bx = 0; bx < l.mWidth; ++bx)
                {
                    Block& b = l.mBlocks[bx + by * l.mWidth];
                    const Block* kids[4] = { nullptr, nullptr, nullptr, nullptr };
                    bool ok = 2 * bx + 1 < below.mWidth && 2 * by + 1 < below.mHeight;
                    for (S32 k = 0; ok && k < 4; ++k)
                    {
                        kids[k] = &below.mBlocks[(2 * bx + (k & 1)) + (2 * by + (k >> 1)) * below.mWidth];
                        ok = kids[k]->mMerge && kids[k]->mCount == kids[0]->mCount;
                    }
                    if (!ok)
                    {
                        continue;
                    }
                    const U32 first = static_cast<U32>(l.mGaps.size());
                    for (U32 i = 0; ok && i < kids[0]->mCount; ++i)
                    {
                        GapAgg a = below.mGaps[kids[0]->mFirst + i];
                        for (S32 k = 1; k < 4; ++k)
                        {
                            const GapAgg& o = below.mGaps[kids[k]->mFirst + i];
                            // AVATAR's floor is carried, not compared: the air keeps the leaves it always had.
                            ok = ok && (o.mFlags & ~AirGap::AVATAR_FLOOR) == (a.mFlags & ~AirGap::AVATAR_FLOOR);
                            a.mFlags |= o.mFlags & AirGap::AVATAR_FLOOR;
                            a.mFloorMin = std::min(a.mFloorMin, o.mFloorMin);
                            a.mFloorMax = std::max(a.mFloorMax, o.mFloorMax);
                            a.mCeilMin = std::min(a.mCeilMin, o.mCeilMin);
                            a.mCeilMax = std::max(a.mCeilMax, o.mCeilMax);
                            a.mClear = std::min(a.mClear, o.mClear);
                        }
                        // A leaf's record is the highest floor under the lowest ceiling, so those must not cross.
                        ok = ok && a.mFloorMax - a.mFloorMin <= climb && a.mCeilMax - a.mCeilMin <= climb &&
                             ((a.mFlags & AirGap::OPEN) || a.mFloorMax < a.mCeilMin);
                        l.mGaps.push_back(a);
                    }
                    if (!ok)
                    {
                        l.mGaps.resize(first);
                        continue;
                    }
                    b.mLeaf = b.mMerge = true;
                    b.mCount = kids[0]->mCount;
                    b.mFirst = first;
                }
            }
        }

        // Each cell's leaf level: the largest block over it that may stand
        // as a leaf, found top down.
        std::vector<S8> lvl(static_cast<size_t>(w) * h, 0);
        auto fill = [&](S32 lv, S32 bx, S32 by) {
            const S32 s = 1 << lv;
            for (S32 y = by * s; y < std::min(h, (by + 1) * s); ++y)
            {
                for (S32 x = bx * s; x < std::min(w, (bx + 1) * s); ++x)
                {
                    lvl[x + y * w] = static_cast<S8>(lv);
                }
            }
        };
        std::function<void(S32, S32, S32)> assign = [&](S32 lv, S32 bx, S32 by) {
            const Level& l = levels[lv];
            if (bx >= l.mWidth || by >= l.mHeight)
            {
                return;
            }
            if (lv == 0 || l.mBlocks[bx + by * l.mWidth].mLeaf)
            {
                fill(lv, bx, by);
                return;
            }
            for (S32 k = 0; k < 4; ++k)
            {
                assign(lv - 1, 2 * bx + (k & 1), 2 * by + (k >> 1));
            }
        };
        for (S32 by = 0; by < levels[AIR_MAX_LEVEL].mHeight; ++by)
        {
            for (S32 bx = 0; bx < levels[AIR_MAX_LEVEL].mWidth; ++bx)
            {
                assign(AIR_MAX_LEVEL, bx, by);
            }
        }

        // Restrict: a leaf beside one more than a level finer splits, until
        // none does. A block's quarters may always stand as leaves.
        for (bool changed = true; changed;)
        {
            changed = false;
            for (S32 iy = 0; iy < h; ++iy)
            {
                for (S32 ix = 0; ix < w; ++ix)
                {
                    const S32 lv = lvl[ix + iy * w];
                    const S32 s = 1 << lv;
                    if (lv < 2 || ix % s || iy % s)
                    {
                        continue;
                    }
                    bool split = false;
                    for (S32 t = 0; t < s && !split; ++t)
                    {
                        const S32 around[4][2] = { { ix - 1, iy + t }, { ix + s, iy + t }, { ix + t, iy - 1 }, { ix + t, iy + s } };
                        for (const auto& n : around)
                        {
                            if (n[0] >= 0 && n[1] >= 0 && n[0] < w && n[1] < h && lvl[n[0] + n[1] * w] < lv - 1)
                            {
                                split = true;
                            }
                        }
                    }
                    if (split)
                    {
                        fill(lv - 1, ix >> (lv - 1), iy >> (lv - 1));
                        fill(lv - 1, (ix >> (lv - 1)) + 1, iy >> (lv - 1));
                        fill(lv - 1, ix >> (lv - 1), (iy >> (lv - 1)) + 1);
                        fill(lv - 1, (ix >> (lv - 1)) + 1, (iy >> (lv - 1)) + 1);
                        changed = true;
                    }
                }
            }
        }

        // Leaves in key order: the level-4 blocks by their own key, each walked in Z order.
        std::function<void(S32, S32, S32)> emit = [&](S32 ix, S32 iy, S32 lv) {
            if (ix >= w || iy >= h)
            {
                return;
            }
            if (lvl[ix + iy * w] < lv)
            {
                const S32 half = 1 << (lv - 1);
                for (S32 k = 0; k < 4; ++k)
                {
                    emit(ix + (k & 1) * half, iy + (k >> 1) * half, lv - 1);
                }
                return;
            }
            AirLeaf leaf;
            leaf.mX = static_cast<U16>(ix);
            leaf.mY = static_cast<U16>(iy);
            leaf.mLevel = static_cast<U8>(lv);
            leaf.mFirstGap = static_cast<U32>(out.mGaps.size());
            if (lv == 0)
            {
                const U32 count = std::min<U32>(grid.count(ix, iy), 0xff);
                const AirGap* g = grid.gaps(ix, iy);
                for (U32 i = 0; i < count; ++i)
                {
                    out.mGaps.push_back(record(g[i]));
                }
                leaf.mGapCount = static_cast<U8>(count);
            }
            else
            {
                const Level& l = levels[lv];
                const Block& b = l.mBlocks[(ix >> lv) + (iy >> lv) * l.mWidth];
                for (U32 i = 0; i < b.mCount; ++i)
                {
                    out.mGaps.push_back(record(l.mGaps[b.mFirst + i]));
                }
                leaf.mGapCount = b.mCount;
            }
            if (leaf.mGapCount > 0)
            {
                out.mLeaves.push_back(leaf);
            }
        };
        const S32 bw = levels[AIR_MAX_LEVEL].mWidth, bh = levels[AIR_MAX_LEVEL].mHeight;
        S32 side = 1;
        while (side < std::max(bw, bh))
        {
            side *= 2;
        }
        for (U32 m = 0; m < static_cast<U32>(side * side); ++m)
        {
            U32 bx = 0, by = 0;
            for (S32 b = 0; b < 16; ++b)
            {
                bx |= ((m >> (2 * b)) & 1) << b;
                by |= ((m >> (2 * b + 1)) & 1) << b;
            }
            emit(static_cast<S32>(bx) << AIR_MAX_LEVEL, static_cast<S32>(by) << AIR_MAX_LEVEL, AIR_MAX_LEVEL);
        }
        out.mLeaves.shrink_to_fit();
        out.mGaps.shrink_to_fit();
    }

    void airSeams(const AirGrid& grid, TileAir& out)
    {
        const S32 w = grid.mWidth, h = grid.mHeight;
        for (S32 side = 0; side < 4; ++side)
        {
            SeamStrip& strip = out.mSeams[side];
            const bool along_y = side == TileAir::SIDE_NEG_X || side == TileAir::SIDE_POS_X;
            const S32 n = along_y ? h : w;
            strip.mStart.assign(static_cast<size_t>(std::max(0, n)) + 1, 0);
            strip.mGaps.clear();
            if (w <= 0 || h <= 0)
            {
                continue;
            }
            for (S32 i = 0; i < n; ++i)
            {
                const S32 ix = along_y ? (side == TileAir::SIDE_NEG_X ? 0 : w - 1) : i;
                const S32 iy = along_y ? i : (side == TileAir::SIDE_NEG_Y ? 0 : h - 1);
                const AirGap* g = grid.gaps(ix, iy);
                for (U32 k = 0; k < std::min<U32>(grid.count(ix, iy), 0xff); ++k)
                {
                    strip.mGaps.push_back(record(g[k]));
                }
                strip.mStart[i + 1] = static_cast<U32>(strip.mGaps.size());
            }
        }
    }

    void airTops(const rcHeightfield& hf, S32 border_cells, S32 cells_per_top, TileAir& out)
    {
        // Recast (x, y, z) is SL (y, z, x), as in extractAir.
        const S32 w = std::max(0, hf.height - 2 * border_cells), h = std::max(0, hf.width - 2 * border_cells);
        const S32 c = std::max(1, cells_per_top);
        out.mTopCell = c;
        out.mTopWidth = (w + c - 1) / c;
        out.mTopHeight = (h + c - 1) / c;
        out.mTopStart.assign(static_cast<size_t>(out.mTopWidth) * out.mTopHeight + 1, 0);
        out.mTop.clear();
        std::vector<TopInterval> runs;
        for (S32 ty = 0; ty < out.mTopHeight; ++ty)
        {
            for (S32 tx = 0; tx < out.mTopWidth; ++tx)
            {
                runs.clear();
                for (S32 iy = ty * c; iy < std::min(h, (ty + 1) * c); ++iy)
                {
                    for (S32 ix = tx * c; ix < std::min(w, (tx + 1) * c); ++ix)
                    {
                        for (const rcSpan* s = hf.spans[(border_cells + iy) + (border_cells + ix) * hf.width]; s; s = s->next)
                        {
                            runs.push_back({ clipHeight(static_cast<U16>(s->smin)), clipHeight(static_cast<U16>(s->smax)) });
                        }
                    }
                }
                std::sort(runs.begin(), runs.end(), [](const TopInterval& a, const TopInterval& b) { return a.mBottom < b.mBottom; });
                const size_t first = out.mTop.size();
                for (const TopInterval& r : runs)
                {
                    if (out.mTop.size() > first && r.mBottom <= out.mTop.back().mTop)
                    {
                        out.mTop.back().mTop = std::max(out.mTop.back().mTop, r.mTop);
                    }
                    else
                    {
                        out.mTop.push_back(r);
                    }
                }
                out.mTopStart[tx + ty * out.mTopWidth + 1] = static_cast<U32>(out.mTop.size());
            }
        }
    }

    TileAirPtr buildTileAir(const rcHeightfield& hf, S32 border_cells, S32 pad_cells, S32 climb, S32 overlap, S32 min_gap,
                            const std::vector<Flight>* flights, AirBuildStats* stats, const U8* area_lut, const SpanFlags* extra, AirGrid* fine,
                            AirGrid* extracted)
    {
        StageClock clock(stats ? stats->mMs : nullptr);
        const S32 pad = std::clamp(pad_cells, 0, border_cells);
        AirGrid padded, grid;
        const bool given = extracted && !extracted->empty();
        if (given)
        {
            padded = std::move(*extracted);
        }
        else
        {
            extractAir(hf, border_cells - pad, padded, area_lut, extra);
        }
        clock.lap(AirBuildStats::EXTRACT);
        if (padded.empty())
        {
            return nullptr;
        }
        if (!given)
        {
            foldSlivers(padded, min_gap);
        }
        clock.lap(AirBuildStats::FOLD);
        if (flights)
        {
            markFlights(padded, *flights);
        }
        clock.lap(AirBuildStats::FLIGHTS);
        computeClearance(padded, climb, overlap);
        clock.lap(AirBuildStats::CLEARANCE);
        cropAir(padded, pad, grid);
        clock.lap(AirBuildStats::CROP);
        if (grid.empty())
        {
            return nullptr;
        }
        auto air = std::make_shared<TileAir>();
        distilAir(grid, climb, *air);
        clock.lap(AirBuildStats::DISTIL);
        airSeams(grid, *air);
        clock.lap(AirBuildStats::SEAMS);
        airTops(hf, border_cells, std::max(1, static_cast<S32>(1.f / hf.cs + 0.5f)), *air);
        clock.lap(AirBuildStats::TOPS);
        if (stats)
        {
            stats->mGridGaps = static_cast<U32>(grid.mGaps.size());
            stats->mGridBytes = grid.mGaps.size() * sizeof(AirGap) + grid.mStart.size() * sizeof(U32);
            for (const SeamStrip& strip : air->mSeams)
            {
                stats->mStripBytes += strip.mGaps.size() * sizeof(AirGap) + strip.mStart.size() * sizeof(U32);
            }
        }
        if (fine)
        {
            *fine = std::move(grid);
        }
        return air;
    }

    void spanGaps(const rcCompactHeightfield& chf, S32 border, const AirGrid& fine, const TileAir& air, std::vector<U32>& out,
                  const std::vector<U16>* tops)
    {
        out.assign(static_cast<size_t>(chf.spanCount), SPAN_NO_GAP);
        AirColumn column;
        // Recast (x, y, z) is SL (y, z, x): the grid's ix runs along chf z, iy along chf x.
        for (S32 iy = 0; iy < fine.mHeight && border + iy < chf.width; ++iy)
        {
            for (S32 ix = 0; ix < fine.mWidth && border + ix < chf.height; ++ix)
            {
                const rcCompactCell& cell = chf.cells[(border + iy) + (border + ix) * chf.width];
                const S32 leaf = cell.count ? findLeafByKey(air, ix, iy) : -1;
                if (leaf < 0)
                {
                    continue;
                }
                // The column's distilled gaps run lowest first, one for each fine gap.
                airColumn(air, leaf, column);
                const AirGap* g = fine.gaps(ix, iy);
                const U32 count = std::min<U32>(fine.count(ix, iy), column.mCount);
                for (U32 i = cell.index, n = cell.index + cell.count; i < n; ++i)
                {
                    const S32 top = tops ? (*tops)[i] : chf.spans[i].y;
                    for (U32 k = 0; k < count; ++k)
                    {
                        if (top >= g[k].mFloor - 1 && top < g[k].mCeiling)
                        {
                            out[i] = column.mGap[k];
                            break;
                        }
                    }
                }
            }
        }
    }

    TileAirPtr upsampleAir(const TileAir& coarse, S32 factor)
    {
        const S32 f = std::max(1, factor);
        S32 shift = 0;
        while ((1 << shift) < f)
        {
            ++shift;
        }
        const auto scale = [f](U16 h) -> U16 {
            return h == AIR_CEILING_OPEN ? h : static_cast<U16>(std::min<S32>(static_cast<S32>(h) * f, AIR_HEIGHT_MAX));
        };
        const auto scaled = [&scale](AirGap g) {
            g.mFloor = scale(g.mFloor);
            g.mCeiling = scale(g.mCeiling);
            return g;
        };
        auto out = std::make_shared<TileAir>();
        out->mOrigin = coarse.mOrigin;
        out->mCell = coarse.mCell / f;
        out->mUnit = coarse.mUnit / f;
        out->mWidth = coarse.mWidth * f;
        out->mHeight = coarse.mHeight * f;

        // A coarse leaf is f times as wide in fine cells; past AIR_MAX_LEVEL it is split into leaves of that level.
        struct Placed
        {
            AirLeaf mLeaf;
            U32 mFrom;
        };
        std::vector<Placed> placed;
        for (const AirLeaf& l : coarse.mLeaves)
        {
            const S32 level = l.mLevel + shift;
            const S32 top = std::min(level, AIR_MAX_LEVEL);
            const S32 n = 1 << (level - top), side = 1 << top;
            for (S32 by = 0; by < n; ++by)
            {
                for (S32 bx = 0; bx < n; ++bx)
                {
                    Placed p;
                    p.mLeaf.mX = static_cast<U16>(l.mX * f + bx * side);
                    p.mLeaf.mY = static_cast<U16>(l.mY * f + by * side);
                    p.mLeaf.mLevel = static_cast<U8>(top);
                    p.mLeaf.mGapCount = l.mGapCount;
                    p.mFrom = l.mFirstGap;
                    placed.push_back(p);
                }
            }
        }
        std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
            const U32 ka = airKey(a.mLeaf.mX, a.mLeaf.mY), kb = airKey(b.mLeaf.mX, b.mLeaf.mY);
            return ka < kb || (ka == kb && a.mLeaf.mLevel > b.mLeaf.mLevel);
        });
        out->mLeaves.reserve(placed.size());
        for (Placed& p : placed)
        {
            p.mLeaf.mFirstGap = static_cast<U32>(out->mGaps.size());
            for (U32 k = 0; k < p.mLeaf.mGapCount; ++k)
            {
                out->mGaps.push_back(scaled(coarse.mGaps[p.mFrom + k]));
            }
            out->mLeaves.push_back(p.mLeaf);
        }
        linkLeaves(*out);
        for (S32 s = 0; s < 4; ++s)
        {
            const SeamStrip& in = coarse.mSeams[s];
            SeamStrip& strip = out->mSeams[s];
            const S32 cells = static_cast<S32>(in.mStart.size()) - 1;
            strip.mStart.assign(1, 0);
            for (S32 i = 0; i < cells * f; ++i)
            {
                const S32 c = i / f;
                for (U32 k = in.mStart[c]; k < in.mStart[c + 1]; ++k)
                {
                    strip.mGaps.push_back(scaled(in.mGaps[k]));
                }
                strip.mStart.push_back(static_cast<U32>(strip.mGaps.size()));
            }
        }
        out->mTopCell = coarse.mTopCell * f;
        out->mTopWidth = coarse.mTopWidth;
        out->mTopHeight = coarse.mTopHeight;
        out->mTopStart = coarse.mTopStart;
        out->mTop.reserve(coarse.mTop.size());
        for (const TopInterval& t : coarse.mTop)
        {
            out->mTop.push_back({ scale(t.mBottom), scale(t.mTop) });
        }
        return out;
    }

    void leafSpanGaps(const rcCompactHeightfield& chf, S32 border, const TileAir& air, S32 z_shift, S32 slack, std::vector<U32>& out)
    {
        out.assign(static_cast<size_t>(chf.spanCount), SPAN_NO_GAP);
        AirColumn column;
        for (S32 iy = 0; iy < air.mHeight && border + iy < chf.width; ++iy)
        {
            for (S32 ix = 0; ix < air.mWidth && border + ix < chf.height; ++ix)
            {
                const rcCompactCell& cell = chf.cells[(border + iy) + (border + ix) * chf.width];
                const S32 leaf = cell.count ? findLeafByKey(air, ix, iy) : -1;
                if (leaf < 0)
                {
                    continue;
                }
                airColumn(air, leaf, column);
                for (U32 i = cell.index, n = cell.index + cell.count; i < n; ++i)
                {
                    const S32 top = chf.spans[i].y + z_shift;
                    for (U32 k = 0; k < column.mCount; ++k)
                    {
                        const AirGap& g = air.mGaps[column.mGap[k]];
                        if (top >= g.mFloor - 1 - slack && top < g.mCeiling)
                        {
                            out[i] = column.mGap[k];
                            break;
                        }
                    }
                }
            }
        }
    }

    std::vector<U8> packTileAir(const TileAir& air)
    {
        std::vector<U8> out;
        out.reserve(air.mLeaves.size() * 12 + air.mGaps.size() * 8 + air.mTop.size() * 4);
        for (const AirLeaf& l : air.mLeaves)
        {
            put<U16>(out, l.mX);
            put<U16>(out, l.mY);
            put<U8>(out, l.mLevel);
            put<U8>(out, l.mGapCount);
            put<U16>(out, l.mParent);
            put<U32>(out, l.mFirstGap);
        }
        for (const AirGap& g : air.mGaps)
        {
            putGap(out, g);
        }
        for (const SeamStrip& s : air.mSeams)
        {
            for (U32 v : s.mStart)
            {
                put<U32>(out, v);
            }
            for (const AirGap& g : s.mGaps)
            {
                putGap(out, g);
            }
        }
        for (U32 v : air.mTopStart)
        {
            put<U32>(out, v);
        }
        for (const TopInterval& t : air.mTop)
        {
            put<U16>(out, t.mBottom);
            put<U16>(out, t.mTop);
        }
        for (const FaceOffset& f : air.mFaceOffsets)
        {
            put<U32>(out, f.mBits);
        }
        return out;
    }

    std::string tileAirLayout(const TileAir& air)
    {
        // Struct formats as Python's struct module reads them.
        std::ostringstream o;
        o << "{\"leaf_order\":\"morton\",\"leaf_nesting\":\"parent\",\"leaf\":\"<HHBBHI\",\"gap\":\"<HHBBH\",\"start\":\"<I\",\"top\":\"<HH\",\"face\":\"<I\",\"flags\":{\"top\":1,\"open\":2,\"own_sky\":4,"
             "\"flight\":8,\"area_shift\":4,\"area_mask\":48,\"avatar_floor\":64},\"ceiling_open\":"
          << AIR_CEILING_OPEN
          << ",\"face_bits\":{\"key_shift\":5,\"offset_mask\":31,\"steps\":" << FACE_OFFSET_STEPS
          << ",\"key\":\"column*4+side\",\"column\":\"ix+iy*width\",\"sides\":[\"neg_x\",\"pos_x\",\"neg_y\",\"pos_y\"]},\"sections\":[[\"leaves\",\"leaf\"," << air.mLeaves.size() << "],[\"gaps\",\"gap\"," << air.mGaps.size() << "]";
        const char* names[4] = { "seam_neg_x", "seam_pos_x", "seam_neg_y", "seam_pos_y" };
        for (S32 s = 0; s < 4; ++s)
        {
            o << ",[\"" << names[s] << "_start\",\"start\"," << air.mSeams[s].mStart.size() << "],[\"" << names[s] << "\",\"gap\","
              << air.mSeams[s].mGaps.size() << "]";
        }
        o << ",[\"top_start\",\"start\"," << air.mTopStart.size() << "],[\"top\",\"top\"," << air.mTop.size() << "],[\"face_offsets\",\"face\","
          << air.mFaceOffsets.size() << "]]}";
        return o.str();
    }
}
