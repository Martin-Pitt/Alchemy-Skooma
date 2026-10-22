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

#include "skairraster.h"
#include "skcensusprofile.h"

#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace
{
    using namespace skworld;

    struct Interval
    {
        F32 mBottom;
        F32 mTop;
    };

    /// Solid intervals of a heightfield column, ascending, in Recast y metres.
    void column(const rcHeightfield& hf, S32 cx, S32 cz, std::vector<Interval>& out)
    {
        out.clear();
        if (cx < 0 || cz < 0 || cx >= hf.width || cz >= hf.height)
        {
            return;
        }
        for (const rcSpan* s = hf.spans[cx + cz * hf.width]; s; s = s->next)
        {
            out.push_back({ hf.bmin[1] + s->smin * hf.ch, hf.bmin[1] + s->smax * hf.ch });
        }
    }

    struct FaceRun
    {
        S32 mAlong0, mAlong1;   // inclusive cell range along the wall
        S32 mAcross;            // the column across the wall
        F32 mBottom, mTop;
        U8 mArea;
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

    void extractWallFaces(const rcHeightfield& hf, S32 border_cells, F32 min_height, std::vector<WallFace>& out, const U8* area_lut)
    {
        SK_WORLD_ZONE("air.walls.faces");
        // Sides in Recast: 0 +x, 1 -x, 2 +z, 3 -z. Faces of one side, one
        // across-column and one z-interval merge along the wall's direction.
        static const S32 DX[4] = { 1, -1, 0, 0 };
        static const S32 DZ[4] = { 0, 0, 1, -1 };
        std::vector<Interval> theirs;
        for (S32 side = 0; side < 4; ++side)
        {
            const bool alongZ = (side < 2);     // ±x faces run along z
            std::map<std::pair<S32, S32>, std::vector<FaceRun>> runs;  // key: (across, bottom cells)
            const S32 x0 = border_cells, x1 = hf.width - border_cells, z0 = border_cells, z1 = hf.height - border_cells;
            for (S32 cz = z0; cz < z1; ++cz)
            {
                for (S32 cx = x0; cx < x1; ++cx)
                {
                    const rcSpan* first = hf.spans[cx + cz * hf.width];
                    if (!first)
                    {
                        continue;
                    }
                    column(hf, cx + DX[side], cz + DZ[side], theirs);
                    for (const rcSpan* s = first; s; s = s->next)
                    {
                        const U8 area = area_lut ? area_lut[s->area] : static_cast<U8>(s->area);
                        // Subtract the neighbour's solids from this span's interval.
                        F32 lo = hf.bmin[1] + s->smin * hf.ch;
                        const F32 hi = hf.bmin[1] + s->smax * hf.ch;
                        for (const Interval& t : theirs)
                        {
                            if (t.mTop <= lo || t.mBottom >= hi)
                            {
                                continue;
                            }
                            if (t.mBottom > lo)
                            {
                                const S32 along = alongZ ? cz : cx, across = alongZ ? cx : cz;
                                runs[{ across, static_cast<S32>(std::lround(lo / hf.ch)) }].push_back({ along, along, across, lo, t.mBottom, area });
                            }
                            lo = std::max(lo, t.mTop);
                        }
                        if (lo < hi)
                        {
                            const S32 along = alongZ ? cz : cx, across = alongZ ? cx : cz;
                            runs[{ across, static_cast<S32>(std::lround(lo / hf.ch)) }].push_back({ along, along, across, lo, hi, area });
                        }
                    }
                }
            }
            for (auto& entry : runs)
            {
                std::vector<FaceRun>& list = entry.second;
                std::sort(list.begin(), list.end(), [](const FaceRun& a, const FaceRun& b) { return a.mAlong0 < b.mAlong0; });
                size_t i = 0;
                while (i < list.size())
                {
                    FaceRun run = list[i];
                    size_t j = i + 1;
                    while (j < list.size() && list[j].mAlong0 == run.mAlong1 + 1 && std::fabs(list[j].mTop - run.mTop) < hf.ch * 0.5f && list[j].mArea == run.mArea)
                    {
                        run.mAlong1 = list[j].mAlong0;
                        ++j;
                    }
                    i = j;
                    // Recast cells back to SL mesh-local metres: x = Recast z, y = Recast x, z = Recast y.
                    if (run.mTop - run.mBottom < min_height)
                    {
                        continue;
                    }
                    WallFace f;
                    const F32 rx0 = hf.bmin[0] + (alongZ ? run.mAcross : run.mAlong0) * hf.cs;
                    const F32 rx1 = hf.bmin[0] + (alongZ ? run.mAcross + 1 : run.mAlong1 + 1) * hf.cs;
                    const F32 rz0 = hf.bmin[2] + (alongZ ? run.mAlong0 : run.mAcross) * hf.cs;
                    const F32 rz1 = hf.bmin[2] + (alongZ ? run.mAlong1 + 1 : run.mAcross + 1) * hf.cs;
                    // The face sits on the side's boundary of the column.
                    const F32 fx0 = side == 0 ? rx1 : side == 1 ? rx0 : rx0, fx1 = side == 0 ? rx1 : side == 1 ? rx0 : rx1;
                    const F32 fz0 = side == 2 ? rz1 : side == 3 ? rz0 : rz0, fz1 = side == 2 ? rz1 : side == 3 ? rz0 : rz1;
                    f.mMin.set(std::min(fz0, fz1), std::min(fx0, fx1), run.mBottom);
                    f.mMax.set(std::max(fz0, fz1), std::max(fx0, fx1), run.mTop);
                    // Recast +x is SL +y, Recast +z is SL +x.
                    f.mSide = side == 0 ? 2 : side == 1 ? 3 : side == 2 ? 0 : 1;
                    f.mArea = run.mArea;
                    out.push_back(f);
                }
            }
        }
    }

    void extractRunoff(const rcHeightfield& hf, S32 border_cells, S32 climb, S32 headroom, F32 drop_max, F32 spacing,
                       std::vector<WallSegment>& ledges, std::vector<DropLink>& links)
    {
        SK_WORLD_ZONE("air.walls.runoff");
        // Sides in Recast: 0 +x, 1 -x, 2 +z, 3 -z, as for the faces.
        static const S32 DX[4] = { 1, -1, 0, 0 };
        static const S32 DZ[4] = { 0, 0, 1, -1 };
        struct Cell
        {
            S32 mX;
            S32 mZ;
            S32 mTop;
            S32 mLanding;   ///< the landing's top in cells, -1 with nothing below
            S32 mSide;
        };
        std::vector<Cell> cells;
        const S32 x0 = border_cells, x1 = hf.width - border_cells, z0 = border_cells, z1 = hf.height - border_cells;
        for (S32 cz = z0; cz < z1; ++cz)
        {
            for (S32 cx = x0; cx < x1; ++cx)
            {
                for (const rcSpan* s = hf.spans[cx + cz * hf.width]; s; s = s->next)
                {
                    if (s->area == RC_NULL_AREA)
                    {
                        continue;
                    }
                    // Over a side, nothing within the climb of the top or in its headroom.
                    const S32 top = static_cast<S32>(s->smax);
                    for (S32 side = 0; side < 4; ++side)
                    {
                        S32 landing = -1;
                        bool open = true;
                        for (const rcSpan* o = hf.spans[(cx + DX[side]) + (cz + DZ[side]) * hf.width]; o && open; o = o->next)
                        {
                            if (static_cast<S32>(o->smax) > top - climb && static_cast<S32>(o->smin) < top + headroom)
                            {
                                open = false;
                            }
                            else if (static_cast<S32>(o->smax) <= top - climb)
                            {
                                landing = std::max(landing, static_cast<S32>(o->smax));
                            }
                        }
                        if (open)
                        {
                            cells.push_back({ cx, cz, top, landing, side });
                        }
                    }
                }
            }
        }
        // A point on a cell's side, at a fraction along it, at the top.
        const auto edgeAt = [&](const Cell& c, F32 t) {
            F32 rc[3];
            rc[1] = hf.bmin[1] + c.mTop * hf.ch;
            const bool alongZ = c.mSide < 2;
            const F32 rim = (c.mSide == 0 || c.mSide == 2) ? 1.f : 0.f;
            rc[0] = hf.bmin[0] + (c.mX + (alongZ ? rim : t)) * hf.cs;
            rc[2] = hf.bmin[2] + (c.mZ + (alongZ ? t : rim)) * hf.cs;
            return fromRecast(rc);
        };

        // Ledges: runs of one side along a line of columns, each cell joining
        // the run ending beside it within the climb.
        std::vector<size_t> order(cells.size());
        for (size_t i = 0; i < cells.size(); ++i)
        {
            order[i] = i;
        }
        const auto along = [](const Cell& c) { return c.mSide < 2 ? c.mZ : c.mX; };
        const auto across = [](const Cell& c) { return c.mSide < 2 ? c.mX : c.mZ; };
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const Cell& p = cells[a];
            const Cell& q = cells[b];
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
                const Cell& c = cells[order[k]];
                const F32 drop = c.mLanding < 0 ? -1.f : (c.mTop - c.mLanding) * hf.ch;
                seg.mValue = drop >= 0.f && (seg.mValue < 0.f || drop < seg.mValue) ? drop : seg.mValue;
            }
            ledges.push_back(seg);
            i = j;
        }

        // Run-off: the ledge cells joined across lines and sides where a floor's
        // edge runs on, eight ways within the climb, and down each such edge a
        // link every spacing metres, from the cell to its own landing.
        std::map<std::tuple<S32, S32>, std::vector<size_t>> at;
        for (size_t k = 0; k < cells.size(); ++k)
        {
            at[{ cells[k].mX, cells[k].mZ }].push_back(k);
        }
        std::vector<S32> component(cells.size(), -1);
        std::vector<size_t> stack;
        const F32 gap = std::max(spacing, hf.cs) / hf.cs;
        for (size_t seed = 0; seed < cells.size(); ++seed)
        {
            if (component[seed] >= 0)
            {
                continue;
            }
            std::vector<const Cell*> picked;
            component[seed] = static_cast<S32>(seed);
            stack.assign(1, seed);
            while (!stack.empty())
            {
                const size_t k = stack.back();
                stack.pop_back();
                const Cell& c = cells[k];
                bool near_pick = false;
                for (const Cell* p : picked)
                {
                    const F32 dx = static_cast<F32>(p->mX - c.mX), dz = static_cast<F32>(p->mZ - c.mZ);
                    near_pick = near_pick || dx * dx + dz * dz < gap * gap;
                }
                const F32 drop = (c.mTop - c.mLanding) * hf.ch;
                if (!near_pick && c.mLanding >= 0 && drop <= drop_max)
                {
                    picked.push_back(&c);
                    DropLink link;
                    link.mFrom = edgeAt(c, 0.5f);
                    const LLVector3 out_dir(static_cast<F32>(DZ[c.mSide]), static_cast<F32>(DX[c.mSide]), 0.f);   // Recast x is SL y
                    link.mTo = link.mFrom + out_dir * (hf.cs * 0.5f) - LLVector3(0.f, 0.f, drop);
                    link.mHeight = drop;
                    link.mKind = LinkKind::DROP;
                    links.push_back(link);
                }
                for (S32 dz = -1; dz <= 1; ++dz)
                {
                    for (S32 dx = -1; dx <= 1; ++dx)
                    {
                        const auto it = at.find({ c.mX + dx, c.mZ + dz });
                        if (it == at.end())
                        {
                            continue;
                        }
                        for (const size_t n : it->second)
                        {
                            if (component[n] < 0 && std::abs(cells[n].mTop - c.mTop) <= climb)
                            {
                                component[n] = static_cast<S32>(seed);
                                stack.push_back(n);
                            }
                        }
                    }
                }
            }
        }
    }
}
