/**
 * @file skroomsflights.cpp
 * @brief Stairs, ramps and climbs found on the spaces' floor cells.
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

#include "skroomsflights.h"

#include "skair.h"
#include "skcensusprofile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <istream>
#include <ostream>
#include <string>

namespace skworld
{
    namespace
    {
        constexpr U32 NONE = ~0u;
        constexpr U8 AREA_OF_GROUND = 1;
        constexpr U8 AREA_OF_OBJECT = 2;
        constexpr F64 DEG = 180.0 / 3.14159265358979323846;

        U8 areaOf(U8 flags)
        {
            return (flags & AirGap::AREA_MASK) >> AirGap::AREA_SHIFT;
        }

        /// A cell's fitted grade (degrees) and uphill direction; mOk false where too few cells were linked to fit.
        struct Fit
        {
            F64 mGrade = 0.0;
            F64 mUx = 0.0;
            F64 mUy = 0.0;
            bool mOk = false;
        };

        /// Scratch a detection reuses on its thread: stamps instead of sets, flat arrays, no allocation per cell.
        struct Scratch
        {
            std::vector<U32> mStamp;    ///< per cell, the last walk that saw it
            std::vector<U32> mStamp2;
            U32 mWalk = 0;
            std::vector<U32> mQueue;
            std::vector<U32> mSeeds;
            std::vector<U32> mVisited;
            std::vector<U8> mDist;

            void reset(size_t n)
            {
                if (mStamp.size() < n)
                {
                    mStamp.assign(n, 0);
                    mStamp2.assign(n, 0);
                    mWalk = 0;
                }
            }
            U32 nextWalk()
            {
                if (++mWalk == 0)
                {
                    std::fill(mStamp.begin(), mStamp.end(), 0);
                    std::fill(mStamp2.begin(), mStamp2.end(), 0);
                    mWalk = 1;
                }
                return mWalk;
            }
        };

        Scratch& scratch()
        {
            thread_local Scratch s;
            return s;
        }

        /// Rise per metre along one axis: between the cells FLIGHT_FIT_HOPS links out each way along it, or as far as
        /// it links; false when that spans less than two cells.
        bool axisSlope(const FlightGraph& g, U32 c, S32 plus, S32 minus, S32 axis, F64& out)
        {
            U32 ep = c, em = c;
            S32 hp = 0, hm = 0;
            while (hp < FLIGHT_FIT_HOPS && g.mLink[ep * 4 + plus] != NONE)
            {
                ep = g.mLink[ep * 4 + plus];
                ++hp;
            }
            while (hm < FLIGHT_FIT_HOPS && g.mLink[em * 4 + minus] != NONE)
            {
                em = g.mLink[em * 4 + minus];
                ++hm;
            }
            if (hp + hm < 2)
            {
                return false;
            }
            const F64 pe = axis == 0 ? g.mCells[ep].mX : g.mCells[ep].mY;
            const F64 pm = axis == 0 ? g.mCells[em].mX : g.mCells[em].mY;
            const F64 run = pe - pm;
            if (std::fabs(run) < 1e-9)
            {
                return false;
            }
            out = static_cast<F64>(g.mCells[ep].mFloor - g.mCells[em].mFloor) * static_cast<F64>(g.mUnit) / run;
            return true;
        }

        /// The grade and uphill direction from central differences along x and y: four link walks, no window.
        Fit fitCell(const FlightGraph& g, U32 c)
        {
            Fit out;
            F64 sx = 0.0, sy = 0.0;
            const bool hx = axisSlope(g, c, 0, 1, 0, sx);
            const bool hy = axisSlope(g, c, 2, 3, 1, sy);
            if (!hx && !hy)
            {
                return out;
            }
            sx = hx ? sx : 0.0;
            sy = hy ? sy : 0.0;
            const F64 grad = std::hypot(sx, sy);
            out.mGrade = std::atan(grad) * DEG;
            out.mOk = true;
            if (grad >= 1e-12)
            {
                out.mUx = sx / grad;
                out.mUy = sy / grad;
            }
            return out;
        }

        U32 findRoot(std::vector<U32>& parent, U32 a)
        {
            while (parent[a] != a)
            {
                parent[a] = parent[parent[a]];
                a = parent[a];
            }
            return a;
        }

        /// Half away from zero, as llround, on the quarter metre.
        U64 quarter(F64 v)
        {
            const F64 q = v * 4.0;
            const S64 r = q >= 0.0 ? static_cast<S64>(std::floor(q + 0.5)) : -static_cast<S64>(std::floor(-q + 0.5));
            return static_cast<U64>(r);
        }

        /// The convex hull of points, counter-clockwise, collinear points dropped (Andrew's monotone chain).
        std::vector<LLVector2> convexHull(std::vector<std::pair<F64, F64>>& pts)
        {
            std::sort(pts.begin(), pts.end());
            pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
            std::vector<LLVector2> out;
            if (pts.size() < 3)
            {
                for (const auto& p : pts)
                {
                    out.emplace_back(static_cast<F32>(p.first), static_cast<F32>(p.second));
                }
                return out;
            }
            const auto cross = [](const std::pair<F64, F64>& o, const std::pair<F64, F64>& a, const std::pair<F64, F64>& b) {
                return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
            };
            std::vector<std::pair<F64, F64>> h(2 * pts.size());
            size_t k = 0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0.0)
                {
                    --k;
                }
                h[k++] = pts[i];
            }
            for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
            {
                while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0.0)
                {
                    --k;
                }
                h[k++] = pts[i - 1];
            }
            h.resize(k - 1);
            for (const auto& p : h)
            {
                out.emplace_back(static_cast<F32>(p.first), static_cast<F32>(p.second));
            }
            return out;
        }

        struct Axis
        {
            F64 mUx = 0.0, mUy = 0.0, mS0 = 0.0, mS1 = 0.0;
            bool mOk = false;
        };

        F64 msSince(std::chrono::steady_clock::time_point t)
        {
            return std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t).count();
        }
    }

    void detectFlights(const FlightGraph& g, std::vector<AirFlight>& out, std::vector<U16>& cell_flight, FlightStats* stats,
                       std::vector<FlightRegionTrace>* trace)
    {
        SK_WORLD_ZONE("skrooms - flights");
        out.clear();
        const U32 n = static_cast<U32>(g.mCells.size());
        cell_flight.assign(n, 0);
        FlightStats st;
        st.mCells = n;
        Scratch& s = scratch();
        s.reset(n);
        const F64 cell = g.mCell, unit = g.mUnit, cell2 = cell * cell;

        // Fit: a grade and an uphill direction per object floor cell; the rest are never candidates.
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Fit> fits(n);
        std::vector<U8> cand(n, 0);
        {
            SK_WORLD_ZONE("flights.fit");
            // Only cells within FLIGHT_FIT_HOPS links of a step (a link to another floor) can fit a grade: the rest
            // are level, which most floor is. A breadth-first pass from every step, in index order.
            std::vector<U8>& dist = s.mDist;
            dist.assign(n, static_cast<U8>(FLIGHT_FIT_HOPS + 1));
            s.mQueue.clear();
            for (U32 c = 0; c < n; ++c)
            {
                const S32 f0 = g.mCells[c].mFloor;
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = g.mLink[c * 4 + d];
                    if (o != NONE && g.mCells[o].mFloor != f0)
                    {
                        dist[c] = 0;
                        s.mQueue.push_back(c);
                        break;
                    }
                }
            }
            for (size_t head = 0; head < s.mQueue.size(); ++head)
            {
                const U32 a = s.mQueue[head];
                if (dist[a] >= FLIGHT_FIT_HOPS)
                {
                    continue;
                }
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = g.mLink[a * 4 + d];
                    if (o != NONE && dist[o] > dist[a] + 1)
                    {
                        dist[o] = static_cast<U8>(dist[a] + 1);
                        s.mQueue.push_back(o);
                    }
                }
            }
            for (U32 c = 0; c < n; ++c)
            {
                if (areaOf(g.mCells[c].mFlags) != AREA_OF_OBJECT || dist[c] > FLIGHT_FIT_HOPS)
                {
                    continue;
                }
                fits[c] = fitCell(g, c);
                ++st.mFitted;
                const Fit& f = fits[c];
                if (f.mOk && f.mGrade >= FLIGHT_MIN_GRADE_DEG && f.mGrade <= FLIGHT_MAX_GRADE_DEG && (f.mUx != 0.0 || f.mUy != 0.0))
                {
                    cand[c] = 1;
                    ++st.mCandidates;
                }
            }
        }
        st.mFitMs = static_cast<F32>(msSince(t0));

        // Grow: union neighbours rising the same way at much the same grade; the least cell is a region's root.
        t0 = std::chrono::steady_clock::now();
        std::vector<U32> parent(n);
        std::vector<U32> region_start, region_cells;
        std::vector<S32> region_of(n, -1);
        {
            SK_WORLD_ZONE("flights.grow");
            // Per root, the sum of its cells' uphill directions and grades, and its count.
            std::vector<F64> sum_x(n, 0.0), sum_y(n, 0.0), sum_g(n, 0.0);
            std::vector<U32> members(n, 1);
            for (U32 c = 0; c < n; ++c)
            {
                parent[c] = c;
                if (cand[c])
                {
                    sum_x[c] = fits[c].mUx;
                    sum_y[c] = fits[c].mUy;
                    sum_g[c] = fits[c].mGrade;
                }
            }
            const F64 cos_turn = std::cos(FLIGHT_JOIN_TURN_DEG / DEG);
            for (U32 c = 0; c < n; ++c)
            {
                if (!cand[c])
                {
                    continue;
                }
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = g.mLink[c * 4 + d];
                    if (o == NONE || o <= c || !cand[o])
                    {
                        continue;
                    }
                    const Fit& fc = fits[c];
                    const Fit& fo = fits[o];
                    if (fc.mUx * fo.mUx + fc.mUy * fo.mUy >= cos_turn && std::fabs(fc.mGrade - fo.mGrade) <= FLIGHT_JOIN_GRADE_DEG)
                    {
                        const U32 ra = findRoot(parent, c), rb = findRoot(parent, o);
                        if (ra == rb)
                        {
                            continue;
                        }
                        // And the two regions as a whole rise the same way, so no chain of cells turns a corner.
                        const F64 ma = std::hypot(sum_x[ra], sum_y[ra]), mb = std::hypot(sum_x[rb], sum_y[rb]);
                        if (ma < 1e-12 || mb < 1e-12 || (sum_x[ra] * sum_x[rb] + sum_y[ra] * sum_y[rb]) / (ma * mb) < cos_turn ||
                            std::fabs(sum_g[ra] / members[ra] - sum_g[rb] / members[rb]) > FLIGHT_JOIN_GRADE_DEG)
                        {
                            continue;
                        }
                        const U32 lo = std::min(ra, rb), hi = std::max(ra, rb);
                        parent[hi] = lo;
                        sum_x[lo] += sum_x[hi];
                        sum_y[lo] += sum_y[hi];
                        sum_g[lo] += sum_g[hi];
                        members[lo] += members[hi];
                    }
                }
            }
            // Regions in the order of their roots, their cells in index order: a counting sort over the roots.
            std::vector<U32> count(n, 0);
            for (U32 c = 0; c < n; ++c)
            {
                if (cand[c])
                {
                    ++count[findRoot(parent, c)];
                }
            }
            std::vector<S32> index(n, -1);
            region_start.push_back(0);
            for (U32 c = 0; c < n; ++c)
            {
                if (count[c] >= FLIGHT_MIN_REGION_CELLS)
                {
                    index[c] = static_cast<S32>(region_start.size() - 1);
                    region_start.push_back(region_start.back() + count[c]);
                }
            }
            region_cells.resize(region_start.back());
            std::vector<U32> fill(region_start.begin(), region_start.end() - 1);
            for (U32 c = 0; c < n; ++c)
            {
                if (!cand[c])
                {
                    continue;
                }
                const S32 r = index[findRoot(parent, c)];
                if (r >= 0)
                {
                    region_cells[fill[r]++] = c;
                    region_of[c] = r;
                }
            }
        }
        const U32 regions = static_cast<U32>(region_start.size() - 1);
        st.mRegions = regions;
        st.mGrowMs = static_cast<F32>(msSince(t0));

        // Regions: each one's run axis first, so a deck can tell another flight's far end from its near one.
        t0 = std::chrono::steady_clock::now();
        {
            SK_WORLD_ZONE("flights.regions");
            std::vector<Axis> axes(regions);
            for (U32 r = 0; r < regions; ++r)
            {
                F64 ux = 0.0, uy = 0.0;
                for (U32 k = region_start[r]; k < region_start[r + 1]; ++k)
                {
                    ux += fits[region_cells[k]].mUx;
                    uy += fits[region_cells[k]].mUy;
                }
                const F64 m = std::hypot(ux, uy);
                if (m < 1e-9)
                {
                    continue;
                }
                Axis& a = axes[r];
                a.mUx = ux / m;
                a.mUy = uy / m;
                a.mS0 = 1e300;
                a.mS1 = -1e300;
                for (U32 k = region_start[r]; k < region_start[r + 1]; ++k)
                {
                    const FlightCell& fc = g.mCells[region_cells[k]];
                    const F64 sv = static_cast<F64>(fc.mX) * a.mUx + static_cast<F64>(fc.mY) * a.mUy;
                    a.mS0 = std::min(a.mS0, sv);
                    a.mS1 = std::max(a.mS1, sv);
                }
                a.mOk = true;
            }
            const auto sOf = [&](const Axis& a, U32 c) { return static_cast<F64>(g.mCells[c].mX) * a.mUx + static_cast<F64>(g.mCells[c].mY) * a.mUy; };
            const auto zOf = [&](U32 c) { return static_cast<F64>(g.mCells[c].mFloor) * unit; };

            // Completion: sloped floor beside a region on its plane along the run (a stair's sides, whose grade the
            // ground beside them skews) joins it, within its run, region by region in order, taking cells of regions too
            // small to be a flight; then each region's cells are laid out again in index order.
            std::vector<U8> small(regions);
            for (U32 r = 0; r < regions; ++r)
            {
                small[r] = (region_start[r + 1] - region_start[r]) * cell2 < FLIGHT_MIN_AREA_M2 ? 1 : 0;
            }
            for (U32 r = 0; r < regions; ++r)
            {
                const Axis& ax = axes[r];
                if (!ax.mOk)
                {
                    continue;
                }
                const U32 b = region_start[r], e = region_start[r + 1];
                // An unnamed region already as big as a hillside stays refused whatever it takes in.
                U32 named = 0;
                for (U32 k = b; k < e; ++k)
                {
                    named += (g.mCells[region_cells[k]].mFlags & (AirGap::CUE_STAIR | AirGap::CUE_RAMP)) ? 1 : 0;
                }
                if (named * 2 <= e - b && (e - b) * cell2 > FLIGHT_MAX_AREA_UNCUED_M2)
                {
                    continue;
                }
                F64 sbar = 0.0, zbar = 0.0;
                for (U32 k = b; k < e; ++k)
                {
                    sbar += sOf(ax, region_cells[k]);
                    zbar += zOf(region_cells[k]);
                }
                sbar /= (e - b);
                zbar /= (e - b);
                F64 vs = 0.0, cov = 0.0;
                for (U32 k = b; k < e; ++k)
                {
                    const F64 ds = sOf(ax, region_cells[k]) - sbar;
                    vs += ds * ds;
                    cov += ds * (zOf(region_cells[k]) - zbar);
                }
                if (vs < 1e-9)
                {
                    continue;
                }
                const F64 slope = cov / vs;
                s.mQueue.assign(region_cells.begin() + b, region_cells.begin() + e);
                for (size_t head = 0; head < s.mQueue.size(); ++head)
                {
                    const U32 a = s.mQueue[head];
                    for (S32 d = 0; d < 4; ++d)
                    {
                        const U32 o = g.mLink[a * 4 + d];
                        if (o == NONE || !cand[o] || region_of[o] == static_cast<S32>(r) || (region_of[o] >= 0 && !small[region_of[o]]))
                        {
                            continue;
                        }
                        const F64 so = sOf(ax, o);
                        if (std::fabs(zOf(o) - (zbar + slope * (so - sbar))) > FLIGHT_COMPLETE_DZ_UNITS * unit)
                        {
                            continue;
                        }
                        region_of[o] = static_cast<S32>(r);
                        s.mQueue.push_back(o);
                    }
                }
            }
            {
                std::vector<U32> fill(regions + 1, 0);
                for (U32 c = 0; c < n; ++c)
                {
                    if (region_of[c] >= 0)
                    {
                        ++fill[region_of[c] + 1];
                    }
                }
                for (U32 r = 0; r < regions; ++r)
                {
                    fill[r + 1] += fill[r];
                }
                region_start = fill;
                region_cells.resize(fill[regions]);
                for (U32 c = 0; c < n; ++c)
                {
                    if (region_of[c] >= 0)
                    {
                        region_cells[fill[region_of[c]]++] = c;
                    }
                }
            }
            // Each region's extent along its run again, over its cells now.
            for (U32 r = 0; r < regions; ++r)
            {
                Axis& a = axes[r];
                if (!a.mOk || region_start[r] == region_start[r + 1])
                {
                    continue;
                }
                a.mS0 = 1e300;
                a.mS1 = -1e300;
                for (U32 k = region_start[r]; k < region_start[r + 1]; ++k)
                {
                    const F64 sv = sOf(a, region_cells[k]);
                    a.mS0 = std::min(a.mS0, sv);
                    a.mS1 = std::max(a.mS1, sv);
                }
            }

            std::vector<std::pair<S32, std::pair<F64, F64>>> bins;     // slice, l range
            std::vector<std::pair<S32, S32>> slice_floor;               // slice, floor
            std::vector<F64> values;
            std::vector<std::pair<F64, F64>> corners;
            for (U32 r = 0; r < regions; ++r)
            {
                const Axis& ax = axes[r];
                if (!ax.mOk)
                {
                    continue;
                }
                const U32* rc = region_cells.data() + region_start[r];
                const U32 nc = region_start[r + 1] - region_start[r];
                if (!nc)
                {
                    continue;     // every cell taken by another region's completion
                }
                // Its name and area first: an unnamed region too small for a flight, or as big as a hillside, is
                // refused before anything is measured.
                const F64 area = nc * cell2;
                U32 stair_cues = 0, ramp_cues = 0;
                for (U32 k = 0; k < nc; ++k)
                {
                    const U8 flags = g.mCells[rc[k]].mFlags;
                    stair_cues += (flags & AirGap::CUE_STAIR) ? 1 : 0;
                    ramp_cues += (flags & AirGap::CUE_RAMP) ? 1 : 0;
                }
                const U8 cue = stair_cues * 2 > nc ? 1 : (ramp_cues * 2 > nc ? 2 : 0);
                if (!cue && (area < FLIGHT_MIN_AREA_M2 || area > FLIGHT_MAX_AREA_UNCUED_M2))
                {
                    if (trace)
                    {
                        FlightRegionTrace tr;
                        tr.mCells = nc;
                        tr.mUx = ax.mUx;
                        tr.mUy = ax.mUy;
                        tr.mS0 = ax.mS0;
                        tr.mS1 = ax.mS1;
                        tr.mArea = area;
                        tr.mVerdict = "area";
                        trace->push_back(tr);
                    }
                    continue;
                }
                const F64 ux = ax.mUx, uy = ax.mUy, px = -uy, py = ux, s0 = ax.mS0, s1 = ax.mS1;
                F64 lmin = 1e300, lmax = -1e300, zb_sum = 0.0, zt_sum = 0.0;
                U32 nb = 0, nt = 0;
                for (U32 k = 0; k < nc; ++k)
                {
                    const U32 c = rc[k];
                    const F64 sv = sOf(ax, c);
                    const F64 l = static_cast<F64>(g.mCells[c].mX) * px + static_cast<F64>(g.mCells[c].mY) * py;
                    lmin = std::min(lmin, l);
                    lmax = std::max(lmax, l);
                    if (sv <= s0 + FLIGHT_END_BAND_M)
                    {
                        zb_sum += zOf(c);
                        ++nb;
                    }
                    if (sv >= s1 - FLIGHT_END_BAND_M)
                    {
                        zt_sum += zOf(c);
                        ++nt;
                    }
                }
                const F64 lc = 0.5 * (lmin + lmax);
                const F64 zb_band = zb_sum / nb, zt_band = zt_sum / nt;

                // A deck: floor beyond an end within reach of its height, flooded until it is big enough;
                // land, or another flight met at its far end (a landing between two), is a deck however small.
                const auto deck = [&](bool top, F64& z_out) {
                    const F64 band = top ? zt_band : zb_band;
                    // A deck lies past its end along the run, nearer this end's height than the other's.
                    const F64 other_band = top ? zb_band : zt_band;
                    const auto beyond = [&](U32 o) {
                        const F64 so = sOf(ax, o), z = zOf(o);
                        return (top ? so > s1 : so < s0) && std::fabs(z - band) < std::fabs(z - other_band);
                    };
                    const U32 seed_walk = s.nextWalk();
                    s.mSeeds.clear();
                    for (U32 k = 0; k < nc; ++k)
                    {
                        const U32 c = rc[k];
                        const F64 sv = sOf(ax, c);
                        if (top ? sv < s1 - FLIGHT_END_REACH_M : sv > s0 + FLIGHT_END_REACH_M)
                        {
                            continue;
                        }
                        for (S32 d = 0; d < 4; ++d)
                        {
                            const U32 o = g.mLink[c * 4 + d];
                            if (o == NONE || s.mStamp[o] == seed_walk || region_of[o] == static_cast<S32>(r) || std::fabs(zOf(o) - band) > FLIGHT_FLOOR_DZ_M ||
                                !beyond(o))
                            {
                                continue;
                            }
                            s.mStamp[o] = seed_walk;
                            s.mSeeds.push_back(o);
                        }
                    }
                    const U32 walk = seed_walk;     // a second stamp array keeps this walk's visits apart from its seeds
                    s.mQueue.assign(s.mSeeds.begin(), s.mSeeds.end());
                    s.mVisited.clear();
                    bool other = false, ground = false;
                    for (size_t head = 0; head < s.mQueue.size() && s.mVisited.size() * cell2 < FLIGHT_FLOOR_MIN_M2; ++head)
                    {
                        const U32 a = s.mQueue[head];
                        if (s.mStamp2[a] == walk)
                        {
                            continue;
                        }
                        s.mStamp2[a] = walk;
                        const S32 ra = region_of[a];
                        if (ra >= 0 && (region_start[ra + 1] - region_start[ra]) * cell2 >= FLIGHT_MIN_AREA_M2)
                        {
                            if (axes[ra].mOk)
                            {
                                const F64 sa = sOf(axes[ra], a);
                                if (top && sa <= axes[ra].mS0 + FLIGHT_END_REACH_M + cell)
                                {
                                    other = true;
                                }
                                if (!top && sa >= axes[ra].mS1 - FLIGHT_END_REACH_M - cell)
                                {
                                    other = true;
                                }
                            }
                            continue;
                        }
                        s.mVisited.push_back(a);
                        ground = ground || areaOf(g.mCells[a].mFlags) == AREA_OF_GROUND;
                        for (S32 d = 0; d < 4; ++d)
                        {
                            const U32 o = g.mLink[a * 4 + d];
                            if (o == NONE || s.mStamp2[o] == walk || region_of[o] == static_cast<S32>(r) || std::fabs(zOf(o) - band) > FLIGHT_FLOOR_DZ_M ||
                                !beyond(o))
                            {
                                continue;
                            }
                            s.mQueue.push_back(o);
                        }
                    }
                    values.clear();
                    for (U32 a : s.mVisited)
                    {
                        values.push_back(zOf(a));
                    }
                    std::sort(values.begin(), values.end());
                    z_out = values.empty() ? band : values[values.size() / 2];
                    return ground || other || s.mVisited.size() * cell2 >= FLIGHT_FLOOR_MIN_M2;
                };
                F64 fb = 0.0, ft = 0.0;
                const bool deck_b = deck(false, fb);
                const bool deck_t = deck(true, ft);
                const F64 rise = ft - fb;

                // Width: the median across-run extent of one-cell slices along the run. Slice floors: their medians
                // in order, for the going between risers of two units or more.
                bins.clear();
                slice_floor.clear();
                for (U32 k = 0; k < nc; ++k)
                {
                    const U32 c = rc[k];
                    const F64 l = static_cast<F64>(g.mCells[c].mX) * px + static_cast<F64>(g.mCells[c].mY) * py;
                    const S32 b = static_cast<S32>(std::floor((sOf(ax, c) - s0) / cell + 0.5));
                    bins.push_back({ b, { l, l } });
                    slice_floor.push_back({ b, g.mCells[c].mFloor });
                }
                std::sort(bins.begin(), bins.end());
                values.clear();
                for (size_t i = 0; i < bins.size();)
                {
                    size_t j = i;
                    F64 lo = bins[i].second.first, hi = lo;
                    for (; j < bins.size() && bins[j].first == bins[i].first; ++j)
                    {
                        lo = std::min(lo, bins[j].second.first);
                        hi = std::max(hi, bins[j].second.first);
                    }
                    values.push_back(hi - lo + cell);
                    i = j;
                }
                std::sort(values.begin(), values.end());
                const F64 width = values[values.size() / 2];
                // Plateaus: runs of slices at one floor. Where the floor steps between them by two units or more on
                // average they are treads, and treads deeper on average than a stair's are seating; a unit's step is a slope.
                std::sort(slice_floor.begin(), slice_floor.end());
                // The run between the slices clear of both decks: the first above the lower, the last below the upper.
                S32 b_above = S32_MAX, b_below = S32_MIN;
                for (size_t i = 0; i < slice_floor.size();)
                {
                    size_t j = i;
                    while (j < slice_floor.size() && slice_floor[j].first == slice_floor[i].first)
                    {
                        ++j;
                    }
                    const F64 z = slice_floor[i + (j - i) / 2].second * unit;
                    if (z > fb)
                    {
                        b_above = std::min(b_above, slice_floor[i].first);
                    }
                    if (z < ft)
                    {
                        b_below = std::max(b_below, slice_floor[i].first);
                    }
                    i = j;
                }
                S32 b0 = slice_floor.front().first, b1 = slice_floor.back().first;
                if (b_above != S32_MAX && b_below != S32_MIN && b_above <= b_below)
                {
                    b0 = b_above;
                    b1 = b_below;
                }
                const F64 sb = s0 + (b0 - 0.5) * cell, stp = s0 + (b1 + 0.5) * cell;
                const F64 run = stp - sb;
                const F64 grade = std::atan2(rise, std::max(run, 1e-6)) * DEG;
                // Treads: runs of two slices or more at one floor within the run (a single slice between is the air's
                // merged leaves laying a riser over a few cells). Where they step two units or more apart on average they
                // are treads, and treads deeper on average than a stair's are seating; a unit's step is a slope's quantisation.
                S32 treads = 0, first_floor = 0, last_floor = 0, run_floor = 0, run_len = 0;
                bool any = false;
                for (size_t i = 0; i < slice_floor.size();)
                {
                    size_t j = i;
                    while (j < slice_floor.size() && slice_floor[j].first == slice_floor[i].first)
                    {
                        ++j;
                    }
                    const S32 b = slice_floor[i].first;
                    const S32 med = slice_floor[i + (j - i) / 2].second;     // sorted by floor within the slice
                    i = j;
                    if (b < b0 || b > b1)
                    {
                        continue;
                    }
                    if (!any)
                    {
                        first_floor = med;
                        run_floor = med;
                        run_len = 0;
                        any = true;
                    }
                    if (med != run_floor)
                    {
                        treads += run_len >= 2 ? 1 : 0;
                        run_floor = med;
                        run_len = 0;
                    }
                    ++run_len;
                    last_floor = med;
                }
                treads += any && run_len >= 2 ? 1 : 0;
                const F64 mean_jump = treads > 1 ? static_cast<F64>(last_floor - first_floor) / (treads - 1) : 0.0;
                const F64 going = treads >= 3 && mean_jump >= 2.0 ? run / treads : 0.0;

                S32 headroom = -1;
                U32 avatar = 0;
                for (U32 k = 0; k < nc; ++k)
                {
                    const FlightCell& fc = g.mCells[rc[k]];
                    if (fc.mCeil < FLIGHT_OPEN_CEIL)
                    {
                        headroom = headroom < 0 ? fc.mCeil - fc.mFloor : std::min(headroom, fc.mCeil - fc.mFloor);
                    }
                    avatar += (fc.mFlags & AirGap::AVATAR_FLOOR) ? 1 : 0;
                }
                const bool decked = deck_b && deck_t;
                FlightKind kind;
                FlightRule rule;
                if (grade > FLIGHT_CLIMB_GRADE_DEG)
                {
                    const bool narrow = decked && width <= FLIGHT_NARROW_WIDTH_M && rise >= FLIGHT_NARROW_RISE_MIN_M && rise <= FLIGHT_NARROW_RISE_MAX_M;
                    kind = narrow ? FlightKind::STAIR : FlightKind::CLIMB;
                    rule = narrow ? FlightRule::NARROW : FlightRule::GRADE;
                }
                else if (cue)
                {
                    kind = cue == 1 ? FlightKind::STAIR : FlightKind::RAMP;
                    rule = FlightRule::NAME;
                }
                else
                {
                    kind = grade > FLIGHT_STAIR_GRADE_DEG ? FlightKind::STAIR : FlightKind::RAMP;
                    rule = FlightRule::GRADE;
                }
                const bool cued = cue != 0;
                const char* refused = nullptr;
                if (kind == FlightKind::CLIMB && area / std::max(std::cos(grade / DEG), 1e-6) < FLIGHT_CLIMB_FACE_M2)
                {
                    refused = "climb face";
                }
                else if (!cued && !decked)
                {
                    refused = "not decked";
                }
                else if (rise < FLIGHT_MIN_RISE_M || (rise < FLIGHT_MIN_RISE_UNCUED_M && !cued))
                {
                    refused = "rise";
                }
                else if (run < FLIGHT_MIN_RUN_M || run > FLIGHT_MAX_RUN_M || width < FLIGHT_MIN_WIDTH_M)
                {
                    refused = "run or width";
                }
                else if (going > FLIGHT_MAX_GOING_M + cell)
                {
                    refused = "going";
                }
                if (trace)
                {
                    trace->push_back({ nc, ux, uy, s0, s1, rise, run, grade, width, area, going, deck_b, deck_t, fb, ft, cue,
                                       refused ? refused : flightKindName(kind) });
                }
                if (refused)
                {
                    continue;
                }

                const F64 la = lmin - 0.5 * cell, lb = lmax + 0.5 * cell;
                const auto at = [&](F64 sv, F64 l, F64 z) {
                    return LLVector3(static_cast<F32>(sv * ux + l * px), static_cast<F32>(sv * uy + l * py), static_cast<F32>(z));
                };
                U64 key = 1469598103934665603ull;
                for (const F64 v : { sb * ux + lc * px, sb * uy + lc * py, fb, stp * ux + lc * px, stp * uy + lc * py, ft })
                {
                    key = (key ^ quarter(v)) * 1099511628211ull;
                }
                AirFlight f;
                f.mKey = key;
                f.mId = static_cast<U32>((key ^ (key >> 32)) & 0xffffffffull);
                f.mId = f.mId ? f.mId : 1;
                f.mKind = kind;
                f.mRule = rule;
                f.mBottom = at(sb, lc, fb);
                f.mTop = at(stp, lc, ft);
                f.mBottomA = at(sb, la, fb);
                f.mBottomB = at(sb, lb, fb);
                f.mTopA = at(stp, la, ft);
                f.mTopB = at(stp, lb, ft);
                f.mRise = static_cast<F32>(rise);
                f.mRun = static_cast<F32>(run);
                f.mWidth = static_cast<F32>(width);
                f.mArea = static_cast<F32>(area);
                f.mHeadroomMin = headroom < 0 ? -1.f : static_cast<F32>(headroom * unit);
                f.mAvatarShare = static_cast<F32>(static_cast<F64>(avatar) / nc);
                const F64 slope = rise / std::max(run, 1e-6);
                f.mPlane[0] = static_cast<F32>(slope * ux);
                f.mPlane[1] = static_cast<F32>(slope * uy);
                f.mPlane[2] = static_cast<F32>(fb - slope * sb);
                f.mZMin = static_cast<F32>(fb);
                f.mZMax = static_cast<F32>(ft);
                f.mDecked = decked;
                f.mCue = cue;
                f.mCells = nc;
                corners.clear();
                const F64 h = 0.5 * cell;
                for (U32 k = 0; k < nc; ++k)
                {
                    const F64 x = g.mCells[rc[k]].mX, y = g.mCells[rc[k]].mY;
                    corners.push_back({ x - h, y - h });
                    corners.push_back({ x + h, y - h });
                    corners.push_back({ x + h, y + h });
                    corners.push_back({ x - h, y + h });
                }
                f.mFootprint = convexHull(corners);
                out.push_back(std::move(f));
                const U16 id = static_cast<U16>(std::min<size_t>(out.size(), 0xffff));
                for (U32 k = 0; k < nc; ++k)
                {
                    cell_flight[rc[k]] = id;
                }
            }
        }
        st.mFlights = static_cast<U32>(out.size());
        st.mRegionsMs = static_cast<F32>(msSince(t0));
        if (stats)
        {
            *stats = st;
        }
    }

    const char* flightKindName(FlightKind kind)
    {
        switch (kind)
        {
            case FlightKind::STAIR: return "stair";
            case FlightKind::RAMP: return "ramp";
            case FlightKind::CLIMB: return "climb";
        }
        return "?";
    }

    const char* flightRuleName(FlightRule rule)
    {
        switch (rule)
        {
            case FlightRule::NAME: return "name";
            case FlightRule::GRADE: return "grade";
            case FlightRule::NARROW: return "narrow";
        }
        return "?";
    }

    void writeFlightGraph(std::ostream& out, const FlightGraph& g)
    {
        const auto put = [&out](const auto& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
        out.write("SKFG", 4);
        put(static_cast<U32>(1));
        put(g.mCell);
        put(g.mUnit);
        put(static_cast<S32>(g.mCells.size()));
        for (size_t c = 0; c < g.mCells.size(); ++c)
        {
            const FlightCell& fc = g.mCells[c];
            put(fc.mX);
            put(fc.mY);
            put(fc.mFloor);
            put(fc.mCeil);
            put(fc.mFlags);
            for (S32 d = 0; d < 4; ++d)
            {
                put(g.mLink[c * 4 + d]);
            }
        }
    }

    bool readFlightGraph(std::istream& in, FlightGraph& g)
    {
        const auto get = [&in](auto& v) { in.read(reinterpret_cast<char*>(&v), sizeof(v)); };
        char magic[4] = {};
        in.read(magic, 4);
        U32 version = 0;
        S32 count = 0;
        get(version);
        get(g.mCell);
        get(g.mUnit);
        get(count);
        if (!in || std::string(magic, 4) != "SKFG" || version != 1 || count < 0)
        {
            return false;
        }
        g.mCells.resize(count);
        g.mLink.resize(static_cast<size_t>(count) * 4);
        for (S32 c = 0; c < count; ++c)
        {
            FlightCell& fc = g.mCells[c];
            get(fc.mX);
            get(fc.mY);
            get(fc.mFloor);
            get(fc.mCeil);
            get(fc.mFlags);
            for (S32 d = 0; d < 4; ++d)
            {
                get(g.mLink[c * 4 + d]);
            }
        }
        return static_cast<bool>(in);
    }

    void writeFlightsJson(std::ostream& out, const std::vector<AirFlight>& flights)
    {
        const auto v3 = [&out](const LLVector3& v) { out << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']'; };
        for (const AirFlight& f : flights)
        {
            char key[17];
            snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(f.mKey));
            out << "{\"id\":" << f.mId << ",\"key\":\"" << key << "\",\"kind\":\"" << flightKindName(f.mKind) << "\",\"rule\":\""
                << flightRuleName(f.mRule) << "\",\"bottom\":";
            v3(f.mBottom);
            out << ",\"top\":";
            v3(f.mTop);
            out << ",\"rise\":" << f.mRise << ",\"run\":" << f.mRun << ",\"width\":" << f.mWidth << ",\"area\":" << f.mArea
                << ",\"headroom\":" << f.mHeadroomMin << ",\"avatar\":" << f.mAvatarShare << ",\"decked\":" << (f.mDecked ? "true" : "false")
                << ",\"cue\":" << static_cast<S32>(f.mCue) << ",\"cells\":" << f.mCells << "}\n";
        }
    }
}
