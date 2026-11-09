/**
 * @file skcensusclusterspots.cpp
 * @brief Where a cluster's fill probes go: one spot per occupied cell, per region, on its largest clump.
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

#include "skcensusclusterspots.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace
{
    using namespace skworld;

    // A member's footprint clipped to one cell, flat so a cell's clumps walk contiguous memory.
    struct Piece
    {
        F64 mX0, mY0, mX1, mY1, mZ0, mZ1;
        S32 mParent;
    };

    // A thin member still weighs something in the centre.
    constexpr F64 MIN_SIDE_M = 0.1;

    U32 cellsAlong(F64 extent, F64 cell_m)
    {
        return static_cast<U32>(llclamp(static_cast<S32>(std::ceil(extent / llmax(cell_m, 1.0))), 1, static_cast<S32>(MAX_SPOT_CELLS)));
    }

    S32 root(std::vector<Piece>& pieces, S32 i)
    {
        while (pieces[i].mParent != i)
        {
            pieces[i].mParent = pieces[pieces[i].mParent].mParent;
            i = pieces[i].mParent;
        }
        return i;
    }

    F64 area(const Piece& p)
    {
        return llmax(p.mX1 - p.mX0, MIN_SIDE_M) * llmax(p.mY1 - p.mY0, MIN_SIDE_M);
    }

    // The largest clump of one cell's pieces, if the cell holds any.
    void spotOfCell(std::vector<Piece>& pieces, std::vector<F64>& areas, std::vector<ClusterSpot>& out)
    {
        if (pieces.empty())
        {
            return;
        }
        const S32 n = static_cast<S32>(pieces.size());
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.mX0 < b.mX0; });
        for (S32 i = 0; i < n; ++i)
        {
            pieces[i].mParent = i;
        }
        // Sweep along x: only pieces starting before this one ends can touch it.
        for (S32 i = 0; i < n; ++i)
        {
            const F64 x_end = pieces[i].mX1 + CLUMP_TOUCH_M;
            for (S32 j = i + 1; j < n && pieces[j].mX0 <= x_end; ++j)
            {
                if (pieces[j].mY0 <= pieces[i].mY1 + CLUMP_TOUCH_M && pieces[i].mY0 <= pieces[j].mY1 + CLUMP_TOUCH_M)
                {
                    const S32 a = root(pieces, i);
                    const S32 b = root(pieces, j);
                    if (a != b)
                    {
                        pieces[llmax(a, b)].mParent = llmin(a, b);
                    }
                }
            }
        }
        areas.assign(n, 0.0);
        for (S32 i = 0; i < n; ++i)
        {
            areas[root(pieces, i)] += area(pieces[i]);
        }
        const S32 best = static_cast<S32>(std::max_element(areas.begin(), areas.end()) - areas.begin());
        ClusterSpot s;
        s.mMin.set(DBL_MAX, DBL_MAX, DBL_MAX);
        s.mMax.set(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        F64 wx = 0.0, wy = 0.0;
        for (S32 i = 0; i < n; ++i)
        {
            const Piece& p = pieces[i];
            if (root(pieces, i) != best)
            {
                continue;
            }
            const F64 a = area(p);
            wx += a * (p.mX0 + p.mX1) * 0.5;
            wy += a * (p.mY0 + p.mY1) * 0.5;
            s.mMin.mdV[VX] = llmin(s.mMin.mdV[VX], p.mX0);
            s.mMin.mdV[VY] = llmin(s.mMin.mdV[VY], p.mY0);
            s.mMin.mdV[VZ] = llmin(s.mMin.mdV[VZ], p.mZ0);
            s.mMax.mdV[VX] = llmax(s.mMax.mdV[VX], p.mX1);
            s.mMax.mdV[VY] = llmax(s.mMax.mdV[VY], p.mY1);
            s.mMax.mdV[VZ] = llmax(s.mMax.mdV[VZ], p.mZ1);
        }
        s.mX = wx / areas[best];
        s.mY = wy / areas[best];
        out.push_back(s);
    }
}

void skworld::clusterSpots(const Cluster& cluster, F64 cell_m, std::vector<ClusterSpot>& out)
{
    const size_t count = cluster.mRecords.size();
    // Members ordered by region, so each region's part is one contiguous run.
    std::vector<const Record*> members(count);
    for (size_t i = 0; i < count; ++i)
    {
        members[i] = cluster.mRecords[i].get();
    }
    std::sort(members.begin(), members.end(), [](const Record* a, const Record* b)
    {
        if (a->mOrigin.mdV[VX] != b->mOrigin.mdV[VX])
        {
            return a->mOrigin.mdV[VX] < b->mOrigin.mdV[VX];
        }
        return a->mOrigin.mdV[VY] < b->mOrigin.mdV[VY];
    });

    std::vector<Piece> pieces;
    std::vector<F64> areas;
    for (size_t begin = 0; begin < count;)
    {
        size_t end = begin + 1;
        while (end < count && members[end]->mOrigin.mdV[VX] == members[begin]->mOrigin.mdV[VX] &&
               members[end]->mOrigin.mdV[VY] == members[begin]->mOrigin.mdV[VY])
        {
            ++end;
        }
        LLVector3d lo(DBL_MAX, DBL_MAX, DBL_MAX);
        LLVector3d hi(-DBL_MAX, -DBL_MAX, -DBL_MAX);
        for (size_t i = begin; i < end; ++i)
        {
            for (S32 k = 0; k < 2; ++k)
            {
                lo.mdV[k] = llmin(lo.mdV[k], members[i]->mMin.mdV[k]);
                hi.mdV[k] = llmax(hi.mdV[k], members[i]->mMax.mdV[k]);
            }
        }
        const U32 nx = cellsAlong(hi.mdV[VX] - lo.mdV[VX], cell_m);
        const U32 ny = cellsAlong(hi.mdV[VY] - lo.mdV[VY], cell_m);
        const F64 sx = (hi.mdV[VX] - lo.mdV[VX]) / nx;
        const F64 sy = (hi.mdV[VY] - lo.mdV[VY]) / ny;
        for (U32 ix = 0; ix < nx; ++ix)
        {
            for (U32 iy = 0; iy < ny; ++iy)
            {
                const F64 cx0 = lo.mdV[VX] + sx * ix;
                const F64 cy0 = lo.mdV[VY] + sy * iy;
                const F64 cx1 = cx0 + sx;
                const F64 cy1 = cy0 + sy;
                pieces.clear();
                for (size_t i = begin; i < end; ++i)
                {
                    const Record& r = *members[i];
                    if (r.mMax.mdV[VX] < cx0 || r.mMin.mdV[VX] > cx1 || r.mMax.mdV[VY] < cy0 || r.mMin.mdV[VY] > cy1)
                    {
                        continue;
                    }
                    pieces.push_back({ llmax(r.mMin.mdV[VX], cx0), llmax(r.mMin.mdV[VY], cy0), llmin(r.mMax.mdV[VX], cx1),
                                       llmin(r.mMax.mdV[VY], cy1), r.mMin.mdV[VZ], r.mMax.mdV[VZ], 0 });
                }
                spotOfCell(pieces, areas, out);
            }
        }
        begin = end;
    }
}
