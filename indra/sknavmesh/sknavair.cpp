/**
 * @file sknavair.cpp
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

#include "linden_common.h"

#include "sknavair.h"

#include "Recast.h"

#include <algorithm>

namespace sknav
{
    void extractAir(const rcHeightfield& hf, S32 border_cells, AirGrid& out)
    {
        // Recast (x, y, z) is SL (y, z, x): a heightfield row along Recast x is
        // a run along SL y, so SL ix is Recast z and SL iy is Recast x.
        const S32 x0 = border_cells, z0 = border_cells;
        out.mWidth = std::max(0, hf.height - 2 * border_cells);
        out.mHeight = std::max(0, hf.width - 2 * border_cells);
        out.mCell = hf.cs;
        out.mUnit = hf.ch;
        out.mOrigin.set(hf.bmin[2] + z0 * hf.cs, hf.bmin[0] + x0 * hf.cs, hf.bmin[1]);
        out.mStart.assign(static_cast<size_t>(out.mWidth) * out.mHeight + 1, 0);
        out.mGaps.clear();

        // The top of the tile's height range, in span units: a gap with
        // nothing above it runs to here.
        const S32 top = static_cast<S32>((hf.bmax[1] - hf.bmin[1]) / hf.ch + 0.5f);
        const U16 open = static_cast<U16>(std::clamp(top, 0, 0xffff));

        for (S32 iy = 0; iy < out.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < out.mWidth; ++ix)
            {
                const rcSpan* s = hf.spans[(x0 + iy) + (z0 + ix) * hf.width];
                for (; s; s = s->next)
                {
                    AirGap gap;
                    gap.mFloor = static_cast<U16>(s->smax);
                    gap.mFlags = s->area != RC_NULL_AREA ? AirGap::WALKABLE : 0;
                    if (s->area >= 1 && s->area <= 3)
                    {
                        gap.mFlags |= static_cast<U8>(s->area << AirGap::AREA_SHIFT);
                    }
                    if (s->next)
                    {
                        gap.mCeiling = static_cast<U16>(s->next->smin);
                    }
                    else
                    {
                        gap.mCeiling = std::max(open, gap.mFloor);
                        gap.mFlags |= AirGap::OPEN | AirGap::OWN_SKY;
                    }
                    if (gap.mCeiling > gap.mFloor || (gap.mFlags & AirGap::OPEN))
                    {
                        out.mGaps.push_back(gap);
                    }
                }
                out.mStart[ix + iy * out.mWidth + 1] = static_cast<U32>(out.mGaps.size());
            }
        }
    }

    void computeClearance(AirGrid& grid, S32 climb, S32 overlap)
    {
        const S32 w = grid.mWidth, h = grid.mHeight;
        const size_t n = grid.mGaps.size();
        if (n == 0)
        {
            return;
        }
        // Per gap, its link in each direction (-x, +y, +x, -y, as Recast
        // orders them): the adjacent gap with the nearest floor, or none.
        const S32 dx[4] = { -1, 0, 1, 0 }, dy[4] = { 0, 1, 0, -1 };
        constexpr S32 NONE = -1;
        std::vector<S32> link(n * 4, NONE);
        std::vector<U16> dist(n, 0xffff);
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const U32 base = grid.mStart[ix + iy * w], count = grid.count(ix, iy);
                for (S32 d = 0; d < 4; ++d)
                {
                    const S32 nx = ix + dx[d], ny = iy + dy[d];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                    {
                        continue;   // off the grid: unknown, not a wall
                    }
                    // Both columns run lowest floor first, so the neighbours
                    // within climb of each gap are a window that only moves up.
                    const U32 nbase = grid.mStart[nx + ny * w], ncount = grid.count(nx, ny);
                    U32 lo = 0;
                    for (U32 i = 0; i < count; ++i)
                    {
                        const AirGap& g = grid.mGaps[base + i];
                        while (lo < ncount && grid.mGaps[nbase + lo].mFloor + climb < g.mFloor)
                        {
                            ++lo;
                        }
                        S32 best = NONE, best_step = 0;
                        for (U32 j = lo; j < ncount && grid.mGaps[nbase + j].mFloor <= g.mFloor + climb; ++j)
                        {
                            const AirGap& o = grid.mGaps[nbase + j];
                            const S32 step = std::abs(static_cast<S32>(o.mFloor) - static_cast<S32>(g.mFloor));
                            if (airAdjacent(g, o, climb, overlap) && (best == NONE || step < best_step))
                            {
                                best = static_cast<S32>(nbase + j);
                                best_step = step;
                            }
                        }
                        link[(base + i) * 4 + d] = best;
                        if (best == NONE)
                        {
                            dist[base + i] = 1;     // half a cell to the side that stops
                        }
                    }
                }
            }
        }
        // Two chamfer passes in half cells: 2 along an axis, 3 diagonally,
        // the diagonal taken through an orthogonal link as Recast does.
        auto relax = [&](U32 g, S32 d, S32 dd) {
            const S32 a = link[g * 4 + d];
            if (a == NONE)
            {
                return;
            }
            dist[g] = std::min<U16>(dist[g], static_cast<U16>(std::min(0xffff, dist[a] + 2)));
            const S32 b = link[a * 4 + dd];
            if (b != NONE)
            {
                dist[g] = std::min<U16>(dist[g], static_cast<U16>(std::min(0xffff, dist[b] + 3)));
            }
        };
        for (S32 iy = 0; iy < h; ++iy)
        {
            for (S32 ix = 0; ix < w; ++ix)
            {
                const U32 base = grid.mStart[ix + iy * w];
                for (U32 i = 0; i < grid.count(ix, iy); ++i)
                {
                    relax(base + i, 0, 3);
                    relax(base + i, 3, 2);
                }
            }
        }
        for (S32 iy = h - 1; iy >= 0; --iy)
        {
            for (S32 ix = w - 1; ix >= 0; --ix)
            {
                const U32 base = grid.mStart[ix + iy * w];
                for (U32 i = 0; i < grid.count(ix, iy); ++i)
                {
                    relax(base + i, 2, 1);
                    relax(base + i, 1, 0);
                }
            }
        }
        const F32 half = grid.mCell * 0.5f;
        for (size_t i = 0; i < n; ++i)
        {
            const F32 metres = dist[i] * half;
            grid.mGaps[i].mClear = static_cast<U8>(std::min<F32>(CLEAR_MAX, std::floor(metres / CLEAR_UNIT_M + 1e-4f)));
        }
    }

    void cropAir(const AirGrid& in, S32 pad, AirGrid& out)
    {
        out.mCell = in.mCell;
        out.mUnit = in.mUnit;
        out.mWidth = std::max(0, in.mWidth - 2 * pad);
        out.mHeight = std::max(0, in.mHeight - 2 * pad);
        out.mOrigin.set(in.mOrigin.mV[VX] + pad * in.mCell, in.mOrigin.mV[VY] + pad * in.mCell, in.mOrigin.mV[VZ]);
        out.mStart.assign(static_cast<size_t>(out.mWidth) * out.mHeight + 1, 0);
        out.mGaps.clear();
        for (S32 iy = 0; iy < out.mHeight; ++iy)
        {
            for (S32 ix = 0; ix < out.mWidth; ++ix)
            {
                const AirGap* g = in.gaps(ix + pad, iy + pad);
                out.mGaps.insert(out.mGaps.end(), g, g + in.count(ix + pad, iy + pad));
                out.mStart[ix + iy * out.mWidth + 1] = static_cast<U32>(out.mGaps.size());
            }
        }
    }
}
