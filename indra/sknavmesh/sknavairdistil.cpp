/**
 * @file sknavairdistil.cpp
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
        // A walkable floor merges only where it is flat within a third of a climb, so neighbouring leaves'
        // floors stay within a climb wherever the steps under them are no more than two thirds of one.
        const S32 flat = climb / 3;
        constexpr U8 MERGE = 1, LIVE = 2;
        constexpr S8 NO_LIVE = AIR_MAX_LEVEL + 1;

        // Level 0 is the grid's own gaps, each with a state; levels above hold merged gaps, each over a 2 x 2 block.
        struct Agg
        {
            GapAgg mG;
            U8 mState = 0;
            U32 mKid = 0;           ///< the first of its four kids in mKids, one per kid block
        };
        struct Grade
        {
            S32 mWidth = 0;
            S32 mHeight = 0;
            std::vector<U32> mStart;    ///< blocks + 1 offsets into mAggs, each block's lowest first
            std::vector<Agg> mAggs;
            std::vector<U32> mKids;
            std::vector<U8> mFiner;     ///< per block, whether a block under it holds a live gap
            std::vector<S8> mMinLive;   ///< per block, the finest level of a live gap in or under it
        };
        Grade lv[AIR_MAX_LEVEL + 1];
        std::vector<U8> state(grid.mGaps.size(), 0);
        // The gaps of a level's blocks, lowest first: level 0's are the grid's, at most 255 a cell.
        struct FineGaps
        {
            const AirGrid* mGrid;
            U8* mState;
            U32 start(size_t c) const { return mGrid->mStart[c]; }
            U32 end(size_t c) const { return std::min(mGrid->mStart[c + 1], mGrid->mStart[c] + 0xff); }
            U8& state(U32 i) const { return mState[i]; }
            GapAgg agg(U32 i) const
            {
                const AirGap& g = mGrid->mGaps[i];
                GapAgg a;
                a.mFloorMin = a.mFloorMax = clipHeight(g.mFloor);
                a.mCeilMin = a.mCeilMax = clipHeight(g.mCeiling);
                a.mFlags = g.mFlags;
                a.mClear = g.mClear;
                return a;
            }
        };
        struct MergedGaps
        {
            Grade* mGrade;
            U32 start(size_t b) const { return mGrade->mStart[b]; }
            U32 end(size_t b) const { return mGrade->mStart[b + 1]; }
            U8& state(U32 i) const { return mGrade->mAggs[i].mState; }
            const GapAgg& agg(U32 i) const { return mGrade->mAggs[i].mG; }
        };
        const FineGaps fine{ &grid, state.data() };
        lv[0].mWidth = w;
        lv[0].mHeight = h;
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const size_t c = ix + iy * w;
                const U32 count = grid.mStart[c + 1] - grid.mStart[c];
                if (count > 0xff)
                {
                    LL_WARNS_ONCE("SKNav") << "an air column has " << count << " gaps; keeping the lowest 255" << LL_ENDL;
                }
                for (U32 i = fine.start(c); i < fine.end(c); ++i)
                {
                    // Only air something can use makes a wall matter: a usable gap touching a wall stays fine.
                    const AirGap& g = grid.mGaps[i];
                    const bool passable = (g.mFlags & (AirGap::TOP | AirGap::OPEN)) || g.mCeiling - g.mFloor >= pass;
                    state[i] = LIVE | (count <= 0xff && (g.mClear > 0 || !passable) ? MERGE : 0);
                }
            }
        }

        auto join = [climb, flat](GapAgg& a, const GapAgg& o) {
            // AVATAR's floor is carried, not compared: the air keeps the leaves it always had.
            if ((o.mFlags & ~AirGap::AVATAR_FLOOR) != (a.mFlags & ~AirGap::AVATAR_FLOOR))
            {
                return false;
            }
            const U16 floor_min = std::min(a.mFloorMin, o.mFloorMin), floor_max = std::max(a.mFloorMax, o.mFloorMax);
            const U16 ceil_min = std::min(a.mCeilMin, o.mCeilMin), ceil_max = std::max(a.mCeilMax, o.mCeilMax);
            // A leaf's record is the highest floor under the lowest ceiling, so those must not cross.
            if (floor_max - floor_min > ((a.mFlags & AirGap::TOP) ? flat : climb) || ceil_max - ceil_min > climb ||
                (!(a.mFlags & AirGap::OPEN) && floor_max >= ceil_min))
            {
                return false;
            }
            a.mFlags |= o.mFlags & AirGap::AVATAR_FLOOR;
            a.mFloorMin = floor_min;
            a.mFloorMax = floor_max;
            a.mCeilMin = ceil_min;
            a.mCeilMax = ceil_max;
            a.mClear = std::min(a.mClear, o.mClear);
            return true;
        };
        // Any level's gaps, for the passes that move between levels.
        auto belowStart = [&](S32 L, size_t b) { return L == 0 ? fine.start(b) : lv[L].mStart[b]; };
        auto belowEnd = [&](S32 L, size_t b) { return L == 0 ? fine.end(b) : lv[L].mStart[b + 1]; };
        auto stateOf = [&](S32 L, U32 i) -> U8& { return L == 0 ? state[i] : lv[L].mAggs[i].mState; };
        auto aggOf = [&](S32 L, U32 i) { return L == 0 ? fine.agg(i) : lv[L].mAggs[i].mG; };

        // Each level merges a gap over a 2 x 2 block when every kid has one gap that agrees with it; the
        // kids' finest live level is known once their block is done.
        auto mergeLevel = [&](S32 L, const auto& below_gaps) {
            const S32 B = L - 1;
            Grade& below = lv[B];
            Grade& l = lv[L];
            l.mWidth = (below.mWidth + 1) / 2;
            l.mHeight = (below.mHeight + 1) / 2;
            l.mStart.assign(static_cast<size_t>(l.mWidth) * l.mHeight + 1, 0);
            below.mMinLive.assign(static_cast<size_t>(below.mWidth) * below.mHeight, NO_LIVE);
            for (S32 by = 0; by < l.mHeight; ++by)
            {
                for (S32 bx = 0; bx < l.mWidth; ++bx)
                {
                    const size_t b = bx + by * l.mWidth;
                    l.mStart[b + 1] = static_cast<U32>(l.mAggs.size());
                    size_t kid[4];
                    S32 kids = 0;
                    for (S32 k = 0; k < 4; ++k)
                    {
                        const S32 kx = 2 * bx + (k & 1), ky = 2 * by + (k >> 1);
                        if (kx < below.mWidth && ky < below.mHeight)
                        {
                            kid[kids++] = kx + ky * below.mWidth;
                        }
                    }
                    if (kids == 4)
                    {
                        U32 cursor[3] = { below_gaps.start(kid[1]), below_gaps.start(kid[2]), below_gaps.start(kid[3]) };
                        const U32 end[3] = { below_gaps.end(kid[1]), below_gaps.end(kid[2]), below_gaps.end(kid[3]) };
                        for (U32 i = below_gaps.start(kid[0]), n = below_gaps.end(kid[0]); i < n; ++i)
                        {
                            if (!(below_gaps.state(i) & MERGE))
                            {
                                continue;
                            }
                            GapAgg a = below_gaps.agg(i);
                            U32 pick[3];
                            bool ok = true;
                            for (S32 k = 0; k < 3 && ok; ++k)
                            {
                                // Every kid runs lowest first: a's match lies past the last one taken.
                                ok = false;
                                for (U32 j = cursor[k]; j < end[k]; ++j)
                                {
                                    const GapAgg o = below_gaps.agg(j);
                                    if (o.mFloorMin > a.mFloorMax + climb)
                                    {
                                        break;
                                    }
                                    if ((below_gaps.state(j) & MERGE) && join(a, o))
                                    {
                                        pick[k] = j;
                                        ok = true;
                                        break;
                                    }
                                }
                            }
                            if (!ok)
                            {
                                continue;
                            }
                            Agg merged;
                            merged.mG = a;
                            merged.mState = MERGE | LIVE;
                            merged.mKid = static_cast<U32>(l.mKids.size());
                            l.mKids.push_back(i);
                            below_gaps.state(i) &= ~LIVE;
                            for (S32 k = 0; k < 3; ++k)
                            {
                                l.mKids.push_back(pick[k]);
                                below_gaps.state(pick[k]) &= ~LIVE;
                                cursor[k] = pick[k] + 1;
                            }
                            l.mAggs.push_back(merged);
                        }
                        l.mStart[b + 1] = static_cast<U32>(l.mAggs.size());
                    }
                    for (S32 k = 0; k < kids; ++k)
                    {
                        S8& m = below.mMinLive[kid[k]];
                        for (U32 i = below_gaps.start(kid[k]), n = below_gaps.end(kid[k]); i < n && m > B; ++i)
                        {
                            m = (below_gaps.state(i) & LIVE) ? static_cast<S8>(B) : m;
                        }
                        if (B > 0)
                        {
                            const Grade& g = lv[B - 1];
                            const S32 kx = static_cast<S32>(kid[k] % below.mWidth), ky = static_cast<S32>(kid[k] / below.mWidth);
                            for (S32 t = 0; t < 4; ++t)
                            {
                                const S32 gx = 2 * kx + (t & 1), gy = 2 * ky + (t >> 1);
                                if (gx < g.mWidth && gy < g.mHeight)
                                {
                                    m = std::min(m, g.mMinLive[gx + gy * g.mWidth]);
                                }
                            }
                        }
                    }
                }
            }
        };
        lv[1].mAggs.reserve(grid.mGaps.size() / 4);
        mergeLevel(1, fine);
        for (S32 L = 2; L <= AIR_MAX_LEVEL; ++L)
        {
            lv[L].mAggs.reserve(lv[L - 1].mAggs.size() / 3);
            mergeLevel(L, MergedGaps{ &lv[L - 1] });
        }
        {
            Grade& top = lv[AIR_MAX_LEVEL];
            const Grade& g = lv[AIR_MAX_LEVEL - 1];
            top.mMinLive.assign(static_cast<size_t>(top.mWidth) * top.mHeight, NO_LIVE);
            for (S32 by = 0; by < top.mHeight; ++by)
            {
                for (S32 bx = 0; bx < top.mWidth; ++bx)
                {
                    const size_t b = bx + by * top.mWidth;
                    S8& m = top.mMinLive[b];
                    m = top.mStart[b + 1] > top.mStart[b] ? static_cast<S8>(AIR_MAX_LEVEL) : m;
                    for (S32 t = 0; t < 4; ++t)
                    {
                        const S32 gx = 2 * bx + (t & 1), gy = 2 * by + (t >> 1);
                        if (gx < g.mWidth && gy < g.mHeight)
                        {
                            m = std::min(m, g.mMinLive[gx + gy * g.mWidth]);
                        }
                    }
                }
            }
        }

        // Restrict, gap by gap: a live gap beside an overlapping live one more than a level finer splits into its kids.
        auto overlaps = [](const GapAgg& a, const GapAgg& b) {
            const S32 ca = (a.mFlags & AirGap::OPEN) ? 0xffff : a.mCeilMin, cb = (b.mFlags & AirGap::OPEN) ? 0xffff : b.mCeilMin;
            return std::min(ca, cb) - std::max<S32>(a.mFloorMax, b.mFloorMax) >= 1;
        };
        // Whether a live gap of level hi or finer overlapping a lies beside a block's side, looking down the
        // neighbour's kids along the shared edge only where something that fine lives.
        auto finerBeside = [&](S32 L, S32 nx, S32 ny, S32 side, S32 hi, const GapAgg& a) {
            struct Node
            {
                S32 mLevel, mX, mY;
            };
            Node stack[2 * (AIR_MAX_LEVEL + 1)];
            S32 top = 0;
            stack[top++] = { L, nx, ny };
            // Side 0 looks back from +x, 1 from -x, 2 from +y, 3 from -y.
            const S32 fx = side == 0 ? 1 : 0, fy = side == 2 ? 1 : 0;
            while (top > 0)
            {
                const Node n = stack[--top];
                const Grade& m = lv[n.mLevel];
                if (n.mX >= m.mWidth || n.mY >= m.mHeight)
                {
                    continue;
                }
                const size_t mb = n.mX + n.mY * m.mWidth;
                if (m.mMinLive[mb] > hi)
                {
                    continue;
                }
                if (n.mLevel <= hi)
                {
                    for (U32 j = belowStart(n.mLevel, mb), e = belowEnd(n.mLevel, mb); j < e; ++j)
                    {
                        if ((stateOf(n.mLevel, j) & LIVE) && overlaps(a, aggOf(n.mLevel, j)))
                        {
                            return true;
                        }
                    }
                }
                if (n.mLevel > 0)
                {
                    for (S32 t = 0; t < 2; ++t)
                    {
                        stack[top++] = { n.mLevel - 1, 2 * n.mX + (side < 2 ? fx : t), 2 * n.mY + (side < 2 ? t : fy) };
                    }
                }
            }
            return false;
        };
        auto neighbour = [&](S32 L, S32 bx, S32 by, S32 side, S32& nx, S32& ny) {
            nx = bx + (side == 0 ? -1 : side == 1 ? 1 : 0);
            ny = by + (side == 2 ? -1 : side == 3 ? 1 : 0);
            return nx >= 0 && ny >= 0 && nx < lv[L].mWidth && ny < lv[L].mHeight;
        };
        struct Ref
        {
            S32 mLevel;
            U32 mAgg;
            S32 mX;     ///< its block
            S32 mY;
        };
        std::vector<Ref> work;
        for (S32 L = 2; L <= AIR_MAX_LEVEL; ++L)
        {
            const Grade& l = lv[L];
            for (S32 by = 0; by < l.mHeight; ++by)
            {
                for (S32 bx = 0; bx < l.mWidth; ++bx)
                {
                    const size_t b = bx + by * l.mWidth;
                    // Only a block beside finer live air can need to split.
                    bool near_fine = false;
                    for (S32 side = 0, nx, ny; side < 4 && !near_fine; ++side)
                    {
                        near_fine = neighbour(L, bx, by, side, nx, ny) && l.mMinLive[nx + ny * l.mWidth] <= L - 2;
                    }
                    for (U32 i = l.mStart[b]; near_fine && i < l.mStart[b + 1]; ++i)
                    {
                        if (l.mAggs[i].mState & LIVE)
                        {
                            work.push_back({ L, i, bx, by });
                        }
                    }
                }
            }
        }
        while (!work.empty())
        {
            const Ref r = work.back();
            work.pop_back();
            Grade& l = lv[r.mLevel];
            Agg& a = l.mAggs[r.mAgg];
            if (!(a.mState & LIVE))
            {
                continue;
            }
            bool split = false;
            for (S32 side = 0, nx, ny; side < 4 && !split; ++side)
            {
                split = neighbour(r.mLevel, r.mX, r.mY, side, nx, ny) && finerBeside(r.mLevel, nx, ny, side, r.mLevel - 2, a.mG);
            }
            if (!split)
            {
                continue;
            }
            a.mState &= ~LIVE;
            const S32 B = r.mLevel - 1;
            for (S32 M = r.mLevel, x = r.mX, y = r.mY; M <= AIR_MAX_LEVEL; ++M, x >>= 1, y >>= 1)
            {
                S8& m = lv[M].mMinLive[x + y * lv[M].mWidth];
                m = std::min(m, static_cast<S8>(B));
            }
            for (S32 k = 0; k < 4; ++k)
            {
                const U32 j = l.mKids[a.mKid + k];
                const S32 kx = 2 * r.mX + (k & 1), ky = 2 * r.mY + (k >> 1);
                stateOf(B, j) |= LIVE;
                S8& m = lv[B].mMinLive[kx + ky * lv[B].mWidth];
                m = std::min(m, static_cast<S8>(B));
                if (B >= 2)
                {
                    work.push_back({ B, j, kx, ky });
                }
            }
            // Coarser live air beside the kids may now be too coarse; a block over this one too is not beside it.
            for (S32 side = 0, nx, ny; side < 4; ++side)
            {
                if (!neighbour(r.mLevel, r.mX, r.mY, side, nx, ny))
                {
                    continue;
                }
                for (S32 M = r.mLevel + 1, d = 1; M <= AIR_MAX_LEVEL; ++M, ++d)
                {
                    const S32 mx = nx >> d, my = ny >> d;
                    if (mx == (r.mX >> d) && my == (r.mY >> d))
                    {
                        break;
                    }
                    const Grade& m = lv[M];
                    const size_t mb = mx + my * m.mWidth;
                    for (U32 j = m.mStart[mb]; j < m.mStart[mb + 1]; ++j)
                    {
                        if (m.mAggs[j].mState & LIVE)
                        {
                            work.push_back({ M, j, mx, my });
                        }
                    }
                }
            }
        }

        // Which blocks have live air under them, bottom up, so the walk skips the rest.
        for (S32 L = 1; L <= AIR_MAX_LEVEL; ++L)
        {
            Grade& l = lv[L];
            l.mFiner.assign(static_cast<size_t>(l.mWidth) * l.mHeight, 0);
            for (S32 by = 0; by < l.mHeight; ++by)
            {
                for (S32 bx = 0; bx < l.mWidth; ++bx)
                {
                    const size_t b = bx + by * l.mWidth;
                    l.mFiner[b] = l.mMinLive[b] < L;
                }
            }
        }

        // A leaf per block holding its live gaps, preorder: key order, coarsest first at a shared origin.
        struct Visit
        {
            S32 mLevel;
            S32 mX;
            S32 mY;
            S32 mParent;    ///< the nearest coarser leaf over the block, or -1
        };
        std::vector<Visit> stack;
        out.mGaps.reserve(grid.mGaps.size() / 2);
        out.mLeaves.reserve(grid.mGaps.size() / 3);
        auto emit = [&](S32 root_x, S32 root_y) {
            stack.push_back({ AIR_MAX_LEVEL, root_x, root_y, -1 });
            while (!stack.empty())
            {
                const Visit v = stack.back();
                stack.pop_back();
                const Grade& l = lv[v.mLevel];
                if (v.mX >= l.mWidth || v.mY >= l.mHeight)
                {
                    continue;
                }
                const size_t b = v.mX + v.mY * l.mWidth;
                AirLeaf leaf;
                leaf.mX = static_cast<U16>(v.mX << v.mLevel);
                leaf.mY = static_cast<U16>(v.mY << v.mLevel);
                leaf.mLevel = static_cast<U8>(v.mLevel);
                leaf.mFirstGap = static_cast<U32>(out.mGaps.size());
                for (U32 i = belowStart(v.mLevel, b), n = belowEnd(v.mLevel, b); i < n; ++i)
                {
                    if (stateOf(v.mLevel, i) & LIVE)
                    {
                        out.mGaps.push_back(v.mLevel == 0 ? record(grid.mGaps[i]) : record(l.mAggs[i].mG));
                    }
                }
                leaf.mGapCount = static_cast<U8>(out.mGaps.size() - leaf.mFirstGap);
                S32 parent = v.mParent;
                if (leaf.mGapCount > 0)
                {
                    const S32 at = static_cast<S32>(out.mLeaves.size());
                    leaf.mParent = static_cast<U16>(parent < 0 ? 0 : at - parent);
                    out.mLeaves.push_back(leaf);
                    parent = at;
                }
                if (v.mLevel > 0 && l.mFiner[b])
                {
                    for (S32 k = 3; k >= 0; --k)
                    {
                        stack.push_back({ v.mLevel - 1, 2 * v.mX + (k & 1), 2 * v.mY + (k >> 1), parent });
                    }
                }
            }
        };
        const S32 bw = lv[AIR_MAX_LEVEL].mWidth, bh = lv[AIR_MAX_LEVEL].mHeight;
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
            emit(static_cast<S32>(bx), static_cast<S32>(by));
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
