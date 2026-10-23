/**
 * @file skairwalls.cpp
 * @brief Walls off the solid raster: exposed faces, the ledges where floors end and the run-off down them.
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

#include "skairwalls.h"

#include "skaircolumns.h"
#include "skairsoup.h"
#include "skcensusprofile.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace
{
    using namespace skworld;

    // Sides in SL: 0 +x, 1 -x, 2 +y, 3 -y.
    const S32 DX[4] = { 1, -1, 0, 0 };
    const S32 DY[4] = { 0, 0, 1, -1 };

    /// A column's side exposed over some height, before runs along the wall are merged.
    struct FaceRun
    {
        S32 mAcross;            ///< the column's index across the wall
        S32 mBottom;            ///< units
        S32 mAlong0;            ///< inclusive range along the wall
        S32 mAlong1;
        S32 mTop;
        U8 mArea;
    };

    /// A floor's side over a drop: what the ledges and the run-off are made of.
    struct EdgeCell
    {
        S32 mX;
        S32 mY;
        S32 mTop;
        S32 mLanding;           ///< the landing's top in units, -1 with nothing below
        S32 mSide;
    };
}

namespace skworld
{
    Cover coverClass(F32 wall_height, F32 eye_height, F32 climb, F32 peek)
    {
        if (wall_height >= eye_height - peek)
        {
            return Cover::FULL;
        }
        if (wall_height >= climb)
        {
            return Cover::HALF;
        }
        return Cover::NONE;
    }

    void extractWallFaces(const ColumnRaster& raster, S32 border_cells, F32 min_height, std::vector<WallFace>& out)
    {
        SK_WORLD_ZONE("air.walls.faces");
        const ColumnFrame& f = raster.mFrame;
        const S32 W = f.mWidth;
        const F32 ox = f.mOrigin.mV[VX], oy = f.mOrigin.mV[VY], oz = f.mOrigin.mV[VZ], cs = f.mCell, unit = f.mUnit;
        std::vector<FaceRun> runs;
        for (S32 side = 0; side < 4; ++side)
        {
            // Faces of one side, one across-column and one bottom merge along the wall: ±x faces run along y.
            const bool along_y = side < 2;
            runs.clear();
            for (S32 iy = border_cells; iy < W - border_cells; ++iy)
            {
                for (S32 ix = border_cells; ix < W - border_cells; ++ix)
                {
                    const S32 c = ix + iy * W;
                    const S32 nx = ix + DX[side], ny = iy + DY[side];
                    const bool inside = nx >= 0 && ny >= 0 && nx < W && ny < W;
                    const S32 n = nx + ny * W;
                    const U32 na = inside ? raster.mStart[n] : 0, nb = inside ? raster.mStart[n + 1] : 0;
                    const S32 along = along_y ? iy : ix, across = along_y ? ix : iy;
                    for (U32 k = raster.mStart[c]; k < raster.mStart[c + 1]; ++k)
                    {
                        const U8 area = (raster.mWalk[k] & TOP_WALK) ? areaOfCode(raster.mCode[k]) : 0;
                        // The span's height less the neighbour's solids.
                        S32 lo = raster.mBottom[k];
                        const S32 hi = raster.mTop[k];
                        for (U32 m = na; m < nb; ++m)
                        {
                            const S32 tb = raster.mBottom[m], tt = raster.mTop[m];
                            if (tt <= lo || tb >= hi)
                            {
                                continue;
                            }
                            if (tb > lo)
                            {
                                runs.push_back({ across, lo, along, along, tb, area });
                            }
                            lo = std::max(lo, tt);
                        }
                        if (lo < hi)
                        {
                            runs.push_back({ across, lo, along, along, hi, area });
                        }
                    }
                }
            }
            std::stable_sort(runs.begin(), runs.end(), [](const FaceRun& a, const FaceRun& b) {
                return std::tie(a.mAcross, a.mBottom, a.mAlong0) < std::tie(b.mAcross, b.mBottom, b.mAlong0);
            });
            size_t i = 0;
            while (i < runs.size())
            {
                FaceRun run = runs[i];
                size_t j = i + 1;
                while (j < runs.size() && runs[j].mAcross == run.mAcross && runs[j].mBottom == run.mBottom && runs[j].mAlong0 == run.mAlong1 + 1 &&
                       runs[j].mTop == run.mTop && runs[j].mArea == run.mArea)
                {
                    run.mAlong1 = runs[j].mAlong0;
                    ++j;
                }
                i = j;
                if ((run.mTop - run.mBottom) * unit < min_height)
                {
                    continue;
                }
                // The face sits on the side's boundary of the column.
                const F32 a0 = (along_y ? oy : ox) + run.mAlong0 * cs, a1 = (along_y ? oy : ox) + (run.mAlong1 + 1) * cs;
                const F32 at = (along_y ? ox : oy) + (run.mAcross + ((side & 1) ? 0 : 1)) * cs;
                WallFace face;
                face.mMin.set(along_y ? at : a0, along_y ? a0 : at, oz + run.mBottom * unit);
                face.mMax.set(along_y ? at : a1, along_y ? a1 : at, oz + run.mTop * unit);
                face.mSide = static_cast<U8>(side);
                face.mArea = run.mArea;
                out.push_back(face);
            }
        }
    }

    void extractRunoff(const ColumnRaster& raster, S32 border_cells, S32 climb, S32 headroom, F32 drop_max, std::vector<WallSegment>& ledges,
                       std::vector<RunoffSegment>& runoff)
    {
        SK_WORLD_ZONE("air.walls.runoff");
        const ColumnFrame& f = raster.mFrame;
        const S32 W = f.mWidth;
        const F32 ox = f.mOrigin.mV[VX], oy = f.mOrigin.mV[VY], oz = f.mOrigin.mV[VZ], cs = f.mCell, unit = f.mUnit;

        // Every floor side with nothing within climb of its top or in its headroom beyond it.
        std::vector<EdgeCell> cells;
        for (S32 iy = border_cells; iy < W - border_cells; ++iy)
        {
            for (S32 ix = border_cells; ix < W - border_cells; ++ix)
            {
                const S32 c = ix + iy * W;
                for (U32 k = raster.mStart[c]; k < raster.mStart[c + 1]; ++k)
                {
                    if (!(raster.mWalk[k] & TOP_PRE_LEDGE))
                    {
                        continue;
                    }
                    const S32 top = raster.mTop[k];
                    for (S32 side = 0; side < 4; ++side)
                    {
                        const S32 n = (ix + DX[side]) + (iy + DY[side]) * W;
                        S32 landing = -1;
                        bool open = true;
                        for (U32 m = raster.mStart[n]; m < raster.mStart[n + 1] && open; ++m)
                        {
                            if (raster.mTop[m] > top - climb && raster.mBottom[m] < top + headroom)
                            {
                                open = false;
                            }
                            else if (raster.mTop[m] <= top - climb)
                            {
                                landing = std::max<S32>(landing, raster.mTop[m]);
                            }
                        }
                        if (open)
                        {
                            cells.push_back({ ix, iy, top, landing, side });
                        }
                    }
                }
            }
        }
        // A point on a cell's side, a fraction t along it, at its top.
        const auto edgeAt = [&](const EdgeCell& c, F32 t) {
            const bool along_y = c.mSide < 2;
            const F32 rim = (c.mSide == 0 || c.mSide == 2) ? 1.f : 0.f;
            return LLVector3(ox + (c.mX + (along_y ? rim : t)) * cs, oy + (c.mY + (along_y ? t : rim)) * cs, oz + c.mTop * unit);
        };

        // Ledges: runs of one side along a line of columns, each cell joining the run ending beside it within the climb.
        {
            const auto along = [](const EdgeCell& c) { return c.mSide < 2 ? c.mY : c.mX; };
            const auto across = [](const EdgeCell& c) { return c.mSide < 2 ? c.mX : c.mY; };
            std::vector<U32> order(cells.size());
            for (size_t i = 0; i < cells.size(); ++i)
            {
                order[i] = static_cast<U32>(i);
            }
            std::sort(order.begin(), order.end(), [&](U32 a, U32 b) {
                const EdgeCell& p = cells[a];
                const EdgeCell& q = cells[b];
                return std::make_tuple(p.mSide, across(p), along(p), p.mTop) < std::make_tuple(q.mSide, across(q), along(q), q.mTop);
            });
            size_t i = 0;
            while (i < order.size())
            {
                size_t j = i + 1;
                while (j < order.size() && cells[order[j]].mSide == cells[order[i]].mSide && across(cells[order[j]]) == across(cells[order[i]]) &&
                       along(cells[order[j]]) == along(cells[order[j - 1]]) + 1 && std::abs(cells[order[j]].mTop - cells[order[j - 1]].mTop) <= climb)
                {
                    ++j;
                }
                WallSegment seg;
                seg.mA = edgeAt(cells[order[i]], 0.f);
                seg.mB = edgeAt(cells[order[j - 1]], 1.f);
                seg.mKind = EdgeKind::LEDGE;
                seg.mValue = -1.f;
                for (size_t k = i; k < j; ++k)
                {
                    const EdgeCell& c = cells[order[k]];
                    const F32 drop = c.mLanding < 0 ? -1.f : (c.mTop - c.mLanding) * unit;
                    seg.mValue = drop >= 0.f && (seg.mValue < 0.f || drop < seg.mValue) ? drop : seg.mValue;
                }
                ledges.push_back(seg);
                i = j;
            }
        }

        // Run-off: the edge followed round as a chain of unit edges, the floor on its left, then laid as chords.
        // Each unit edge runs from p0 to p1 on the lattice of column corners.
        struct UnitEdge
        {
            S32 mX0, mY0, mX1, mY1;
            U32 mCell;
        };
        std::vector<UnitEdge> edges;
        edges.reserve(cells.size());
        for (U32 i = 0; i < cells.size(); ++i)
        {
            const EdgeCell& c = cells[i];
            switch (c.mSide)
            {
            case 0: edges.push_back({ c.mX + 1, c.mY, c.mX + 1, c.mY + 1, i }); break;
            case 1: edges.push_back({ c.mX, c.mY + 1, c.mX, c.mY, i }); break;
            case 2: edges.push_back({ c.mX + 1, c.mY + 1, c.mX, c.mY + 1, i }); break;
            default: edges.push_back({ c.mX, c.mY, c.mX + 1, c.mY, i }); break;
            }
        }
        // By where they start, then height, to find each edge's successor.
        std::vector<U32> by_start(edges.size());
        for (size_t i = 0; i < edges.size(); ++i)
        {
            by_start[i] = static_cast<U32>(i);
        }
        const auto startKey = [&](U32 e) { return std::make_tuple(edges[e].mX0, edges[e].mY0, cells[edges[e].mCell].mTop); };
        std::sort(by_start.begin(), by_start.end(), [&](U32 a, U32 b) { return startKey(a) < startKey(b); });
        std::vector<S32> next(edges.size(), -1);
        std::vector<U8> has_prev(edges.size(), 0);
        for (size_t e = 0; e < edges.size(); ++e)
        {
            const UnitEdge& u = edges[e];
            const S32 top = cells[u.mCell].mTop;
            auto it = std::lower_bound(by_start.begin(), by_start.end(), std::make_tuple(u.mX1, u.mY1, top - climb),
                                       [&](U32 a, const std::tuple<S32, S32, S32>& k) { return startKey(a) < k; });
            S32 best = -1, best_d = climb + 1;
            for (; it != by_start.end() && edges[*it].mX0 == u.mX1 && edges[*it].mY0 == u.mY1; ++it)
            {
                const S32 d = std::abs(cells[edges[*it].mCell].mTop - top);
                if (d <= climb && d < best_d && !has_prev[*it] && *it != e)
                {
                    best = static_cast<S32>(*it);
                    best_d = d;
                }
            }
            if (best >= 0)
            {
                next[e] = best;
                has_prev[best] = 1;
            }
        }
        // The chains, open ones from their first edge, then what is left as loops.
        std::vector<U8> used(edges.size(), 0);
        std::vector<U32> chain;
        const auto lay = [&](size_t first) {
            chain.clear();
            for (S32 e = static_cast<S32>(first); e >= 0 && !used[e]; e = next[e])
            {
                used[e] = 1;
                chain.push_back(static_cast<U32>(e));
            }
            const auto drop = [&](U32 e) { return (cells[edges[e].mCell].mTop - cells[edges[e].mCell].mLanding) * unit; };
            const auto runs = [&](U32 e) {
                const EdgeCell& c = cells[edges[e].mCell];
                return c.mLanding >= 0 && (c.mTop - c.mLanding) * unit <= drop_max;
            };
            size_t i = 0;
            while (i < chain.size())
            {
                if (!runs(chain[i]))
                {
                    ++i;
                    continue;
                }
                // Grow a chord from edge i while every edge's middle stays within a cell of it (a staircase is one
                // line), the landing stays one surface, and the drop stays on its line within a unit.
                size_t j = i;
                while (j + 1 < chain.size() && runs(chain[j + 1]))
                {
                    const size_t k = j + 1;
                    const EdgeCell& prev = cells[edges[chain[j]].mCell];
                    const EdgeCell& cur = cells[edges[chain[k]].mCell];
                    if (std::abs(cur.mLanding - prev.mLanding) > climb)
                    {
                        break;
                    }
                    const UnitEdge& a = edges[chain[i]];
                    const UnitEdge& b = edges[chain[k]];
                    const F32 vx = static_cast<F32>(b.mX1 - a.mX0), vy = static_cast<F32>(b.mY1 - a.mY0);
                    const F32 len = std::sqrt(vx * vx + vy * vy);
                    bool fits = len > 0.f;
                    const F32 da = drop(chain[i]), db = drop(chain[k]);
                    for (size_t m = i; m <= k && fits; ++m)
                    {
                        const UnitEdge& u = edges[chain[m]];
                        const F32 mx = 0.5f * (u.mX0 + u.mX1) - a.mX0, my = 0.5f * (u.mY0 + u.mY1) - a.mY0;
                        const F32 t = (mx * vx + my * vy) / (len * len);
                        fits = std::fabs(mx * vy - my * vx) / len <= 1.f && std::fabs(drop(chain[m]) - (da + (db - da) * t)) <= unit;
                    }
                    if (!fits)
                    {
                        break;
                    }
                    j = k;
                }
                const UnitEdge& a = edges[chain[i]];
                const UnitEdge& b = edges[chain[j]];
                const EdgeCell& ca = cells[a.mCell];
                const EdgeCell& cb = cells[b.mCell];
                RunoffSegment seg;
                seg.mA.set(ox + a.mX0 * cs, oy + a.mY0 * cs, oz + ca.mTop * unit);
                seg.mB.set(ox + b.mX1 * cs, oy + b.mY1 * cs, oz + cb.mTop * unit);
                seg.mDropA = drop(chain[i]);
                seg.mDropB = drop(chain[j]);
                seg.mSide = static_cast<U8>(ca.mSide);
                runoff.push_back(seg);
                i = j + 1;
            }
        };
        for (size_t e = 0; e < edges.size(); ++e)
        {
            if (!has_prev[e] && !used[e])
            {
                lay(e);
            }
        }
        // A loop starts at a turn, so a straight side is not cut where the walk round it happened to begin.
        std::vector<S32> prev(edges.size(), -1);
        for (size_t e = 0; e < edges.size(); ++e)
        {
            if (next[e] >= 0)
            {
                prev[next[e]] = static_cast<S32>(e);
            }
        }
        const auto turns = [&](S32 e) {
            const S32 p = prev[e];
            return p < 0 || edges[p].mX1 - edges[p].mX0 != edges[e].mX1 - edges[e].mX0 || edges[p].mY1 - edges[p].mY0 != edges[e].mY1 - edges[e].mY0;
        };
        for (size_t e = 0; e < edges.size(); ++e)
        {
            if (used[e])
            {
                continue;
            }
            S32 start = static_cast<S32>(e);
            for (S32 k = next[e]; k >= 0 && k != static_cast<S32>(e); k = next[k])
            {
                if (turns(start))
                {
                    break;
                }
                start = k;
            }
            lay(static_cast<size_t>(start));
        }
    }
}
