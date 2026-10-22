/**
 * @file sknavairfine.cpp
 * @brief A tile's air at half its cell over the blocks where its narrow openings are, and how it differs from the tile's own.
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

#include "sknavairfine.h"
#include "sknavairdistil.h"

#include <algorithm>
#include <numeric>

namespace sknav
{
    namespace
    {
        /// Open cells from a gap along one direction until a wall, or -1 past max or off the grid.
        S32 openRun(const AirGrid& grid, S32 ix, S32 iy, const AirGap& from, S32 dx, S32 dy, S32 climb, S32 overlap, S32 max)
        {
            const AirGap* gap = &from;
            for (S32 k = 1; k <= max + 1; ++k)
            {
                const S32 x = ix + dx * k, y = iy + dy * k;
                if (x < 0 || y < 0 || x >= grid.mWidth || y >= grid.mHeight)
                {
                    return -1;
                }
                const AirGap* next = nullptr;
                const AirGap* g = grid.gaps(x, y);
                for (U32 i = 0, n = grid.count(x, y); i < n && !next; ++i)
                {
                    next = airAdjacent(*gap, g[i], climb, overlap) ? &g[i] : nullptr;
                }
                if (!next)
                {
                    return k - 1;
                }
                gap = next;
            }
            return -1;
        }

        S32 findRoot(std::vector<S32>& parent, S32 i)
        {
            while (parent[i] != i)
            {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        }

        /// Union-find components of a grid's gaps over lateral links.
        std::vector<S32> components(const AirGrid& grid, S32 climb, S32 overlap)
        {
            std::vector<S32> parent(grid.mGaps.size());
            std::iota(parent.begin(), parent.end(), 0);
            for (S32 iy = 0; iy < grid.mHeight; ++iy)
            {
                for (S32 ix = 0; ix < grid.mWidth; ++ix)
                {
                    for (S32 d = 0; d < 2; ++d)
                    {
                        const S32 nx = ix + (d == 0 ? 1 : 0), ny = iy + (d == 0 ? 0 : 1);
                        if (nx >= grid.mWidth || ny >= grid.mHeight)
                        {
                            continue;
                        }
                        const U32 a0 = grid.mStart[ix + iy * grid.mWidth], b0 = grid.mStart[nx + ny * grid.mWidth];
                        for (U32 a = a0, an = a0 + grid.count(ix, iy); a < an; ++a)
                        {
                            for (U32 b = b0, bn = b0 + grid.count(nx, ny); b < bn; ++b)
                            {
                                if (airAdjacent(grid.mGaps[a], grid.mGaps[b], climb, overlap))
                                {
                                    parent[findRoot(parent, a)] = findRoot(parent, b);
                                }
                            }
                        }
                    }
                }
            }
            for (S32 i = 0; i < static_cast<S32>(parent.size()); ++i)
            {
                parent[i] = findRoot(parent, i);
            }
            return parent;
        }

        /// The gap of a coarse column, by index into the grid, that a fine gap lies in: the most shared height, or -1.
        S32 coarseGapOf(const AirGrid& coarse, S32 cx, S32 cy, const AirGap& f)
        {
            S32 best = -1, best_shared = 0;
            const U32 begin = coarse.mStart[cx + cy * coarse.mWidth];
            for (U32 i = begin, n = begin + coarse.count(cx, cy); i < n; ++i)
            {
                const AirGap& g = coarse.mGaps[i];
                // One coarse unit of slack: Recast rounds a span's top up at either cell.
                const S32 lo = std::max<S32>(f.mFloor, 2 * g.mFloor - 2), hi = std::min<S32>(f.mCeiling, 2 * g.mCeiling + 2);
                if (hi - lo > best_shared || (best < 0 && hi >= lo && f.mCeiling == f.mFloor))
                {
                    best = static_cast<S32>(i);
                    best_shared = hi - lo;
                }
            }
            return best;
        }
    }

    size_t FineAir::bytes() const
    {
        return sizeof(FineAir) + mWindows.capacity() * sizeof(FineWindow) + mLeaves.capacity() * sizeof(FineLeaf) +
               mGaps.capacity() * sizeof(FineGap);
    }

    std::vector<U8> fineCandidates(const AirGrid& grid, S32 climb, S32 overlap)
    {
        const S32 bw = (grid.mWidth + FINE_BLOCK_CELLS - 1) / FINE_BLOCK_CELLS, bh = (grid.mHeight + FINE_BLOCK_CELLS - 1) / FINE_BLOCK_CELLS;
        std::vector<U8> blocks(static_cast<size_t>(bw) * bh, 0);
        for (S32 iy = 0; iy < grid.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < grid.mWidth; ++ix)
            {
                U8& block = blocks[ix / FINE_BLOCK_CELLS + (iy / FINE_BLOCK_CELLS) * bw];
                const AirGap* g = grid.gaps(ix, iy);
                for (U32 i = 0, n = grid.count(ix, iy); i < n && !block; ++i)
                {
                    if (static_cast<S32>(g[i].mCeiling) - static_cast<S32>(g[i].mFloor) < overlap)
                    {
                        continue;
                    }
                    for (S32 axis = 0; axis < 2 && !block; ++axis)
                    {
                        const S32 dx = axis == 0 ? 1 : 0, dy = 1 - dx;
                        const S32 lo = openRun(grid, ix, iy, g[i], -dx, -dy, climb, overlap, FINE_OPENING_CELLS - 1);
                        const S32 hi = lo < 0 ? -1 : openRun(grid, ix, iy, g[i], dx, dy, climb, overlap, FINE_OPENING_CELLS - 1);
                        block = lo >= 0 && hi >= 0 && lo + hi + 1 <= FINE_OPENING_CELLS;
                    }
                }
            }
        }
        return blocks;
    }

    std::vector<FineWindow> fineWindows(const std::vector<U8>& blocks, S32 grid_width, S32 grid_height)
    {
        const S32 bw = (grid_width + FINE_BLOCK_CELLS - 1) / FINE_BLOCK_CELLS, bh = (grid_height + FINE_BLOCK_CELLS - 1) / FINE_BLOCK_CELLS;
        std::vector<S32> label(blocks.size(), -1);
        std::vector<FineWindow> rects;
        for (S32 start = 0; start < bw * bh; ++start)
        {
            if (!blocks[start] || label[start] >= 0)
            {
                continue;
            }
            FineWindow r{ bw, bh, 0, 0 };
            std::vector<S32> stack{ start };
            label[start] = static_cast<S32>(rects.size());
            while (!stack.empty())
            {
                const S32 b = stack.back();
                stack.pop_back();
                const S32 bx = b % bw, by = b / bw;
                r = { std::min(r.mX0, bx), std::min(r.mY0, by), std::max(r.mX1, bx + 1), std::max(r.mY1, by + 1) };
                for (S32 ny = std::max(0, by - 1); ny <= std::min(bh - 1, by + 1); ++ny)
                {
                    for (S32 nx = std::max(0, bx - 1); nx <= std::min(bw - 1, bx + 1); ++nx)
                    {
                        const S32 n = nx + ny * bw;
                        if (blocks[n] && label[n] < 0)
                        {
                            label[n] = label[start];
                            stack.push_back(n);
                        }
                    }
                }
            }
            rects.push_back(r);
        }
        // Bounds of different groups may overlap: merge until none do.
        for (bool merged = true; merged;)
        {
            merged = false;
            for (size_t i = 0; i < rects.size() && !merged; ++i)
            {
                for (size_t j = i + 1; j < rects.size() && !merged; ++j)
                {
                    const FineWindow &a = rects[i], &b = rects[j];
                    if (a.mX0 < b.mX1 && b.mX0 < a.mX1 && a.mY0 < b.mY1 && b.mY0 < a.mY1)
                    {
                        rects[i] = { std::min(a.mX0, b.mX0), std::min(a.mY0, b.mY0), std::max(a.mX1, b.mX1), std::max(a.mY1, b.mY1) };
                        rects.erase(rects.begin() + j);
                        merged = true;
                    }
                }
            }
        }
        for (FineWindow& r : rects)
        {
            r = { r.mX0 * FINE_BLOCK_CELLS, r.mY0 * FINE_BLOCK_CELLS, std::min(grid_width, r.mX1 * FINE_BLOCK_CELLS),
                  std::min(grid_height, r.mY1 * FINE_BLOCK_CELLS) };
        }
        return rects;
    }

    void appendFineLeaves(const AirGrid& fine, S32 fine_x0, S32 fine_y0, const std::vector<U8>& blocks, S32 grid_width, FineAir& out)
    {
        const S32 bw = (grid_width + FINE_BLOCK_CELLS - 1) / FINE_BLOCK_CELLS;
        for (S32 v = 0; v < fine.mHeight; ++v)
        {
            for (S32 u = 0; u < fine.mWidth; ++u)
            {
                const S32 fx = fine_x0 + u, fy = fine_y0 + v;
                const U32 n = fine.count(u, v);
                if (!n || !blocks[(fx / 2) / FINE_BLOCK_CELLS + ((fy / 2) / FINE_BLOCK_CELLS) * bw])
                {
                    continue;
                }
                FineLeaf leaf;
                leaf.mX = static_cast<U16>(fx);
                leaf.mY = static_cast<U16>(fy);
                leaf.mGapCount = static_cast<U8>(std::min<U32>(n, 255));
                leaf.mFirstGap = static_cast<U32>(out.mGaps.size());
                const AirGap* g = fine.gaps(u, v);
                for (U32 i = 0; i < leaf.mGapCount; ++i)
                {
                    out.mGaps.push_back({ g[i].mFloor, g[i].mCeiling, g[i].mFlags, g[i].mClear });
                }
                out.mLeaves.push_back(leaf);
            }
        }
    }

    void sortFineLeaves(FineAir& air)
    {
        std::sort(air.mLeaves.begin(), air.mLeaves.end(),
                  [](const FineLeaf& a, const FineLeaf& b) { return airKey(a.mX, a.mY) < airKey(b.mX, b.mY); });
        std::vector<FineGap> gaps;
        gaps.reserve(air.mGaps.size());
        for (FineLeaf& leaf : air.mLeaves)
        {
            const U32 first = static_cast<U32>(gaps.size());
            gaps.insert(gaps.end(), air.mGaps.begin() + leaf.mFirstGap, air.mGaps.begin() + leaf.mFirstGap + leaf.mGapCount);
            leaf.mFirstGap = first;
        }
        air.mGaps = std::move(gaps);
    }

    void compareFineAir(const AirGrid& coarse, const AirGrid& fine, const FineWindow& w, const std::vector<U8>& blocks, S32 climb,
                        S32 overlap, S32 fine_climb, S32 fine_overlap, FineCompare& out)
    {
        const S32 bw = (coarse.mWidth + FINE_BLOCK_CELLS - 1) / FINE_BLOCK_CELLS;
        const auto candidate = [&](S32 cx, S32 cy) { return blocks[cx / FINE_BLOCK_CELLS + (cy / FINE_BLOCK_CELLS) * bw] != 0; };

        // The coarse window as a grid of its own, so its components are the window's.
        AirGrid cw;
        cw.mWidth = w.mX1 - w.mX0;
        cw.mHeight = w.mY1 - w.mY0;
        cw.mStart.assign(static_cast<size_t>(cw.mWidth) * cw.mHeight + 1, 0);
        for (S32 y = 0; y < cw.mHeight; ++y)
        {
            for (S32 x = 0; x < cw.mWidth; ++x)
            {
                const AirGap* g = coarse.gaps(w.mX0 + x, w.mY0 + y);
                cw.mGaps.insert(cw.mGaps.end(), g, g + coarse.count(w.mX0 + x, w.mY0 + y));
                cw.mStart[x + y * cw.mWidth + 1] = static_cast<U32>(cw.mGaps.size());
            }
        }
        const std::vector<S32> coarse_comp = components(cw, climb, overlap);
        const std::vector<S32> fine_comp = components(fine, fine_climb, fine_overlap);

        // Each fine gap's coarse gap in the window grid.
        std::vector<S32> owner(fine.mGaps.size(), -1);
        for (S32 v = 0; v < fine.mHeight; ++v)
        {
            for (S32 u = 0; u < fine.mWidth; ++u)
            {
                const U32 begin = fine.mStart[u + v * fine.mWidth];
                for (U32 i = begin, n = begin + fine.count(u, v); i < n; ++i)
                {
                    owner[i] = coarseGapOf(cw, u / 2, v / 2, fine.mGaps[i]);
                    if (candidate(w.mX0 + u / 2, w.mY0 + v / 2))
                    {
                        ++out.mFineGaps;
                        if (owner[i] < 0)
                        {
                            ++out.mNewGaps;
                            out.mNewWalkable += (fine.mGaps[i].mFlags & AirGap::TOP) ? 1 : 0;
                        }
                    }
                }
            }
        }

        for (S32 y = 0; y < cw.mHeight; ++y)
        {
            for (S32 x = 0; x < cw.mWidth; ++x)
            {
                for (S32 d = 0; d < 2; ++d)
                {
                    const S32 nx = x + (d == 0 ? 1 : 0), ny = y + (d == 0 ? 0 : 1);
                    if (nx >= cw.mWidth || ny >= cw.mHeight || !(candidate(w.mX0 + x, w.mY0 + y) || candidate(w.mX0 + nx, w.mY0 + ny)))
                    {
                        continue;
                    }
                    // The two fine column pairs across this coarse edge.
                    S32 fa[2][2], fb[2][2];
                    for (S32 k = 0; k < 2; ++k)
                    {
                        fa[k][0] = d == 0 ? 2 * x + 1 : 2 * x + k;
                        fa[k][1] = d == 0 ? 2 * y + k : 2 * y + 1;
                        fb[k][0] = d == 0 ? 2 * nx : 2 * nx + k;
                        fb[k][1] = d == 0 ? 2 * ny + k : 2 * ny;
                    }
                    const U32 a0 = cw.mStart[x + y * cw.mWidth], b0 = cw.mStart[nx + ny * cw.mWidth];
                    for (U32 a = a0, an = a0 + cw.count(x, y); a < an; ++a)
                    {
                        for (U32 b = b0, bn = b0 + cw.count(nx, ny); b < bn; ++b)
                        {
                            if (!airAdjacent(cw.mGaps[a], cw.mGaps[b], climb, overlap))
                            {
                                continue;
                            }
                            ++out.mCoarseLinks;
                            bool joined = false, same = false;
                            for (S32 k = 0; k < 2; ++k)
                            {
                                const U32 p0 = fine.mStart[fa[k][0] + fa[k][1] * fine.mWidth], q0 = fine.mStart[fb[k][0] + fb[k][1] * fine.mWidth];
                                for (U32 p = p0, pn = p0 + fine.count(fa[k][0], fa[k][1]); p < pn; ++p)
                                {
                                    if (owner[p] != static_cast<S32>(a))
                                    {
                                        continue;
                                    }
                                    for (U32 q = q0, qn = q0 + fine.count(fb[k][0], fb[k][1]); q < qn; ++q)
                                    {
                                        if (owner[q] != static_cast<S32>(b))
                                        {
                                            continue;
                                        }
                                        joined = joined || airAdjacent(fine.mGaps[p], fine.mGaps[q], fine_climb, fine_overlap);
                                        same = same || fine_comp[p] == fine_comp[q];
                                    }
                                }
                            }
                            if (!joined)
                            {
                                ++out.mClosed;
                                out.mClosedSeparating += same ? 0 : 1;
                            }
                        }
                    }
                    // Fine links over the edge the coarse air lacks.
                    for (S32 k = 0; k < 2; ++k)
                    {
                        const U32 p0 = fine.mStart[fa[k][0] + fa[k][1] * fine.mWidth], q0 = fine.mStart[fb[k][0] + fb[k][1] * fine.mWidth];
                        for (U32 p = p0, pn = p0 + fine.count(fa[k][0], fa[k][1]); p < pn; ++p)
                        {
                            for (U32 q = q0, qn = q0 + fine.count(fb[k][0], fb[k][1]); q < qn; ++q)
                            {
                                if (!airAdjacent(fine.mGaps[p], fine.mGaps[q], fine_climb, fine_overlap))
                                {
                                    continue;
                                }
                                ++out.mFineLinks;
                                const S32 a = owner[p], b = owner[q];
                                if (a >= 0 && b >= 0 && airAdjacent(cw.mGaps[a], cw.mGaps[b], climb, overlap))
                                {
                                    continue;
                                }
                                ++out.mOpened;
                                out.mOpenedSeparating += (a < 0 || b < 0 || coarse_comp[a] != coarse_comp[b]) ? 1 : 0;
                            }
                        }
                    }
                }
            }
        }
    }
}
