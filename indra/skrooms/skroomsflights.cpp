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
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>

namespace skworld
{
    namespace
    {
        constexpr U32 NONE = ~0u;
        constexpr U32 NO_REGION = NONE - 1;     ///< a candidate cell in no region, once cells hold their regions
        constexpr U8 AREA_OF_GROUND = 1;
        constexpr U8 AREA_OF_OBJECT = 2;
        constexpr U8 CUE_NAMED = AirGap::CUE_STAIR | AirGap::CUE_RAMP;
        constexpr F64 DEG = 180.0 / 3.14159265358979323846;

        U8 areaOf(U8 flags)
        {
            return (flags & AirGap::AREA_MASK) >> AirGap::AREA_SHIFT;
        }

        /// A candidate's fitted grade (degrees) and uphill direction.
        struct CandFit
        {
            F64 mGrade;
            F64 mUx;
            F64 mUy;
        };

        /// A merged region's sums, kept at its union-find root, and the length of its summed direction.
        struct Root
        {
            F64 mX;
            F64 mY;
            F64 mGrade;
            F64 mLen;
        };

        /// A grown region's run axis and its extent along it; the extent is worked out when first asked for.
        struct Axis
        {
            F64 mUx = 0.0, mUy = 0.0, mS0 = 0.0, mS1 = 0.0;
            bool mOk = false;
            bool mExtent = false;
        };

        constexpr U32 SLOPE_MEMO_BITS = 9;

        /// A pair of slopes seen, by their bits, and what they give: a pure function of the pair.
        struct SlopeMemo
        {
            U64 mX = 0;
            U64 mY = 0;
            F64 mGrad = 0.0;
            F64 mGrade = 0.0;
            F64 mUx = 0.0;
            F64 mUy = 0.0;
            bool mUsed = false;
        };

        /// A set of cells as one bit each, small enough to stay in cache, emptied by clearing the bits it set.
        struct CellBits
        {
            std::vector<U64> mWords;
            std::vector<U32> mSet;

            void reset(size_t n)
            {
                if (mWords.size() < (n + 63) / 64)
                {
                    mWords.assign((n + 63) / 64, 0);
                }
                clear();
            }
            bool test(U32 c) const { return (mWords[c >> 6] >> (c & 63)) & 1; }
            void set(U32 c)
            {
                mWords[c >> 6] |= 1ull << (c & 63);
                mSet.push_back(c);
            }
            void clear()
            {
                for (U32 c : mSet)
                {
                    mWords[c >> 6] = 0;
                }
                mSet.clear();
            }
        };

        /// Scratch a detection reuses on its thread: bit sets, flat arrays, no allocation per cell. What is kept per
        /// candidate is in an array of its own, in candidate order.
        struct Scratch
        {
            CellBits mSeen;     ///< a deck search's seeds
            CellBits mSeen2;    ///< and the cells it has taken from its queue
            std::vector<U32> mQueue;
            std::vector<U32> mSeeds;
            std::vector<U32> mVisited;
            std::array<SlopeMemo, 1u << SLOPE_MEMO_BITS> mSlopeMemo;     ///< kept across detections
            std::vector<U32> mCandidate;    ///< per cell, its candidate index or NONE
            std::vector<U32> mCands;        ///< candidate cells, in index order
            std::vector<CandFit> mFits;     ///< per candidate
            std::vector<S32> mRegionOf;     ///< per candidate, its region or -1
            std::vector<U32> mParent;       ///< per candidate; a root is its region's least candidate
            std::vector<U32> mCount;        ///< per root, its cells once merged, else 0
            std::vector<Root> mRoots;
            std::vector<U32> mNamed;        ///< per root, then per region: its cells named a stair or a ramp
            std::vector<S32> mIndex;
            std::vector<U32> mRegionStart;
            std::vector<U32> mRegionCells;
            std::vector<U32> mFill;
            std::vector<Axis> mAxes;
            std::vector<U8> mSmall;
            std::vector<U8> mHillside;
            std::vector<FlightCell> mLocal; ///< the cells of the region being measured, in its order
            std::vector<F64> mS;            ///< and their places along its run
            std::vector<F64> mValues;
            std::vector<U32> mSliceStart;   ///< per slice of the region being measured
            std::vector<S32> mSliceFloor;
            std::vector<F64> mSliceLo;
            std::vector<F64> mSliceHi;
            std::vector<S32> mSliceMedian;

            void reset(size_t n)
            {
                mSeen.reset(n);
                mSeen2.reset(n);
            }
        };

        Scratch& scratch()
        {
            thread_local Scratch s;
            return s;
        }

        /// Rise per metre along one axis: between the cells FLIGHT_FIT_HOPS links out each way along it (ep and em), or
        /// as far as it links; false when that spans less than two cells.
        bool axisSlope(const FlightCell* cells, U32 ep, U32 em, S32 hops, S32 axis, F64 unit, F64& out)
        {
            if (hops < 2)
            {
                return false;
            }
            const F64 pe = axis == 0 ? cells[ep].mX : cells[ep].mY;
            const F64 pm = axis == 0 ? cells[em].mX : cells[em].mY;
            const F64 run = pe - pm;
            if (std::fabs(run) < 1e-9)
            {
                return false;
            }
            out = static_cast<F64>(cells[ep].mFloor - cells[em].mFloor) * unit / run;
            return true;
        }

        U32 findRoot(U32* parent, U32 a)
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
        const FlightCell* cells = g.mCells.data();
        const U32* link = g.mLink.data();

        // Fit: a grade and an uphill direction per object floor cell; the rest are never candidates.
        auto t0 = std::chrono::steady_clock::now();
        // Candidates in index order, and per cell its candidate index or NONE: what grows, measures and floods is
        // sized by the sloped cells, not by every floor cell.
        std::vector<U32>& cands = s.mCands;
        std::vector<CandFit>& cfit = s.mFits;
        cands.clear();
        cfit.clear();
        s.mCandidate.resize(n);
        U32* ci = s.mCandidate.data();
        {
            SK_WORLD_ZONE("flights.fit");
            for (U32 c = 0; c < n; ++c)
            {
                ci[c] = NONE;
                if (areaOf(cells[c].mFlags) != AREA_OF_OBJECT)
                {
                    continue;
                }
                // The grade and uphill direction from central differences along x and y: four link walks, no window,
                // taken side by side and without branches, so the loads of one do not wait on another's.
                U32 e0 = c, e1 = c, e2 = c, e3 = c;
                S32 hx_hops = 0, hy_hops = 0;
                for (S32 k = 0; k < FLIGHT_FIT_HOPS; ++k)
                {
                    const U32 o0 = link[static_cast<size_t>(e0) * 4 + 0];
                    const U32 o1 = link[static_cast<size_t>(e1) * 4 + 1];
                    const U32 o2 = link[static_cast<size_t>(e2) * 4 + 2];
                    const U32 o3 = link[static_cast<size_t>(e3) * 4 + 3];
                    hx_hops += (o0 != NONE) + (o1 != NONE);
                    hy_hops += (o2 != NONE) + (o3 != NONE);
                    e0 = o0 != NONE ? o0 : e0;
                    e1 = o1 != NONE ? o1 : e1;
                    e2 = o2 != NONE ? o2 : e2;
                    e3 = o3 != NONE ? o3 : e3;
                }
                // Level along both walks is no grade, which most floor is: nothing to fit.
                if (cells[e0].mFloor == cells[e1].mFloor && cells[e2].mFloor == cells[e3].mFloor)
                {
                    continue;
                }
                ++st.mFitted;
                F64 sx = 0.0, sy = 0.0;
                const bool hx = axisSlope(cells, e0, e1, hx_hops, 0, unit, sx);
                const bool hy = axisSlope(cells, e2, e3, hy_hops, 1, unit, sy);
                if ((!hx && !hy) || (sx == 0.0 && sy == 0.0))
                {
                    continue;     // no grade: level, or too few cells linked to fit
                }
                // The slopes come from a few rises over a few runs, so the grade and direction of each pair seen is
                // kept: the same pair gives the same values without the trigonometry.
                U64 kx, ky;
                std::memcpy(&kx, &sx, sizeof(kx));
                std::memcpy(&ky, &sy, sizeof(ky));
                SlopeMemo& memo = s.mSlopeMemo[((kx * 0x9e3779b97f4a7c15ull) ^ (ky * 0xc2b2ae3d27d4eb4full)) >> (64 - SLOPE_MEMO_BITS)];
                if (!memo.mUsed || memo.mX != kx || memo.mY != ky)
                {
                    const F64 grad = std::hypot(sx, sy);
                    memo.mX = kx;
                    memo.mY = ky;
                    memo.mUsed = true;
                    memo.mGrad = grad;
                    memo.mGrade = std::atan(grad) * DEG;
                    memo.mUx = sx / grad;
                    memo.mUy = sy / grad;
                }
                const F64 grad = memo.mGrad, grade = memo.mGrade;
                if (grade < FLIGHT_MIN_GRADE_DEG || grade > FLIGHT_MAX_GRADE_DEG || grad < 1e-12)
                {
                    continue;
                }
                const F64 ux = memo.mUx, uy = memo.mUy;
                if (ux == 0.0 && uy == 0.0)
                {
                    continue;
                }
                ci[c] = static_cast<U32>(cands.size());
                cands.push_back(c);
                cfit.push_back({ grade, ux, uy });
            }
        }
        const U32 ncand = static_cast<U32>(cands.size());
        st.mCandidates = ncand;
        st.mFitMs = static_cast<F32>(msSince(t0));

        // Grow: union neighbours rising the same way at much the same grade; the least cell is a region's root.
        t0 = std::chrono::steady_clock::now();
        std::vector<U32>& region_start = s.mRegionStart;
        std::vector<U32>& region_cells = s.mRegionCells;
        region_start.clear();
        s.mRegionOf.resize(ncand);
        S32* rof = s.mRegionOf.data();
        {
            SK_WORLD_ZONE("flights.grow");
            // Per root, the sum of its cells' uphill directions and grades, and its count; a root of one cell (count
            // 0 here) is read from its own fit, so only merged roots are ever written.
            s.mParent.resize(ncand);
            s.mCount.resize(ncand);
            s.mRoots.resize(ncand);
            U32* parent = s.mParent.data();
            U32* rn = s.mCount.data();
            Root* roots = s.mRoots.data();
            const CandFit* fit = cfit.data();
            for (U32 i = 0; i < ncand; ++i)
            {
                parent[i] = i;
                rn[i] = 0;
            }
            const auto rootOf = [&](U32 r) {
                if (rn[r])
                {
                    return roots[r];
                }
                const CandFit& f = fit[r];
                return Root{ f.mUx, f.mUy, f.mGrade, std::sqrt(f.mUx * f.mUx + f.mUy * f.mUy) };
            };
            const F64 cos_turn = std::cos(FLIGHT_JOIN_TURN_DEG / DEG);
            for (U32 i = 0; i < ncand; ++i)
            {
                const U32 c = cands[i];
                const U32* l = link + static_cast<size_t>(c) * 4;
                const CandFit fi = fit[i];
                U32 ra = NONE;      // i's root, found once and moved by its merges
                for (S32 d = 0; d < 4; ++d)
                {
                    const U32 o = l[d];
                    if (o == NONE || o <= c || ci[o] == NONE)
                    {
                        continue;
                    }
                    const U32 j = ci[o];
                    const CandFit& fj = fit[j];
                    if (fi.mUx * fj.mUx + fi.mUy * fj.mUy >= cos_turn && std::fabs(fi.mGrade - fj.mGrade) <= FLIGHT_JOIN_GRADE_DEG)
                    {
                        ra = ra == NONE ? findRoot(parent, i) : ra;
                        const U32 rb = findRoot(parent, j);
                        if (ra == rb)
                        {
                            continue;
                        }
                        // And the two regions as a whole rise the same way, so no chain of cells turns a corner.
                        const Root qa = rootOf(ra), qb = rootOf(rb);
                        const U32 na = std::max(rn[ra], 1u), nb = std::max(rn[rb], 1u);
                        if (qa.mLen < 1e-12 || qb.mLen < 1e-12 || (qa.mX * qb.mX + qa.mY * qb.mY) / (qa.mLen * qb.mLen) < cos_turn ||
                            std::fabs(qa.mGrade / na - qb.mGrade / nb) > FLIGHT_JOIN_GRADE_DEG)
                        {
                            continue;
                        }
                        const U32 lo = std::min(ra, rb), hi = std::max(ra, rb);
                        parent[hi] = lo;
                        Root& q = roots[lo];
                        q.mX = qa.mX + qb.mX;
                        q.mY = qa.mY + qb.mY;
                        q.mGrade = qa.mGrade + qb.mGrade;
                        q.mLen = std::sqrt(q.mX * q.mX + q.mY * q.mY);
                        rn[lo] = na + nb;
                        ra = lo;
                    }
                }
            }
            // A root is the least candidate of its set, so every parent comes before its child: one pass in order
            // resolves each candidate to its root. Then the regions in the order of their roots, their cells in index
            // order: a counting sort over the roots, counting each one's named cells as it goes.
            U32* count = rn;
            s.mNamed.assign(ncand, 0);
            U32* named = s.mNamed.data();
            for (U32 i = 0; i < ncand; ++i)
            {
                count[i] = 0;
            }
            for (U32 i = 0; i < ncand; ++i)
            {
                const U32 r = parent[i] == i ? i : parent[parent[i]];
                parent[i] = r;
                ++count[r];
                named[r] += (cells[cands[i]].mFlags & CUE_NAMED) ? 1 : 0;
            }
            s.mIndex.resize(ncand);
            S32* index = s.mIndex.data();
            region_start.push_back(0);
            U32 nregions = 0;
            for (U32 i = 0; i < ncand; ++i)
            {
                index[i] = -1;
                if (count[i] >= FLIGHT_MIN_REGION_CELLS)
                {
                    named[nregions] = named[i];     // per region from here: a region's root is never before it
                    index[i] = static_cast<S32>(nregions++);
                    region_start.push_back(region_start.back() + count[i]);
                }
            }
            region_cells.resize(region_start.back());
            s.mFill.assign(region_start.begin(), region_start.end() - 1);
            U32* fill = s.mFill.data();
            // From here a candidate cell holds its region (or NO_REGION) in place of its candidate index, so a cell's
            // region is one read.
            for (U32 i = 0; i < ncand; ++i)
            {
                const S32 r = index[parent[i]];
                rof[i] = r;
                ci[cands[i]] = r >= 0 ? static_cast<U32>(r) : NO_REGION;
                if (r >= 0)
                {
                    region_cells[fill[r]++] = cands[i];
                }
            }
        }
        const auto regionOf = [&](U32 c) { return ci[c] >= NO_REGION ? -1 : static_cast<S32>(ci[c]); };
        const U32 regions = static_cast<U32>(region_start.size() - 1);
        st.mRegions = regions;
        st.mGrowMs = static_cast<F32>(msSince(t0));

        // Regions: each one's run axis first, so a deck can tell another flight's far end from its near one.
        t0 = std::chrono::steady_clock::now();
        {
            SK_WORLD_ZONE("flights.regions");
            std::vector<Axis>& axes = s.mAxes;
            axes.assign(regions, Axis());
            // The sums of each region's uphill directions, in index order: one pass over the candidates.
            for (U32 i = 0; i < ncand; ++i)
            {
                if (rof[i] >= 0)
                {
                    axes[rof[i]].mUx += cfit[i].mUx;
                    axes[rof[i]].mUy += cfit[i].mUy;
                }
            }
            for (U32 r = 0; r < regions; ++r)
            {
                Axis& a = axes[r];
                const F64 ux = a.mUx, uy = a.mUy;
                const F64 m = std::hypot(ux, uy);
                a.mUx = 0.0;
                a.mUy = 0.0;
                if (m < 1e-9)
                {
                    continue;
                }
                a.mUx = ux / m;
                a.mUy = uy / m;
                a.mOk = true;
            }
            const auto sOf = [&](const Axis& a, U32 c) { return static_cast<F64>(cells[c].mX) * a.mUx + static_cast<F64>(cells[c].mY) * a.mUy; };
            const auto zOf = [&](U32 c) { return static_cast<F64>(cells[c].mFloor) * unit; };

            // Completion: sloped floor beside a region on its plane along the run (a stair's sides, whose grade the
            // ground beside them skews) joins it, within its run, region by region in order, taking cells of regions too
            // small to be a flight; then each region's cells are laid out again in index order.
            // An unnamed region already as big as a hillside stays refused whatever it takes in, and as no completion
            // takes its cells (it is not small), it is refused as it grew, with nothing measured.
            s.mSmall.resize(regions);
            s.mHillside.resize(regions);
            U8* small = s.mSmall.data();
            U8* hillside = s.mHillside.data();
            for (U32 r = 0; r < regions; ++r)
            {
                const U32 nr = region_start[r + 1] - region_start[r];
                small[r] = nr * cell2 < FLIGHT_MIN_AREA_M2 ? 1 : 0;
                hillside[r] = s.mNamed[r] * 2 <= nr && nr * cell2 > FLIGHT_MAX_AREA_UNCUED_M2 ? 1 : 0;
            }
            for (U32 r = 0; r < regions; ++r)
            {
                const Axis& ax = axes[r];
                if (!ax.mOk || hillside[r])
                {
                    continue;
                }
                const U32 b = region_start[r], e = region_start[r + 1];
                std::vector<F64>& sv = s.mS;
                std::vector<F64>& zv = s.mValues;
                sv.resize(e - b);
                zv.resize(e - b);
                F64 sbar = 0.0, zbar = 0.0;
                for (U32 k = b; k < e; ++k)
                {
                    sv[k - b] = sOf(ax, region_cells[k]);
                    zv[k - b] = zOf(region_cells[k]);
                    sbar += sv[k - b];
                    zbar += zv[k - b];
                }
                sbar /= (e - b);
                zbar /= (e - b);
                F64 vs = 0.0, cov = 0.0;
                for (U32 k = 0; k < e - b; ++k)
                {
                    const F64 ds = sv[k] - sbar;
                    vs += ds * ds;
                    cov += ds * (zv[k] - zbar);
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
                        const U32 o = link[a * 4 + d];
                        if (o == NONE || ci[o] == NONE)
                        {
                            continue;
                        }
                        const S32 ro = ci[o] == NO_REGION ? -1 : static_cast<S32>(ci[o]);
                        if (ro == static_cast<S32>(r) || (ro >= 0 && !small[ro]))
                        {
                            continue;
                        }
                        const F64 so = sOf(ax, o);
                        if (std::fabs(zOf(o) - (zbar + slope * (so - sbar))) > FLIGHT_COMPLETE_DZ_UNITS * unit)
                        {
                            continue;
                        }
                        ci[o] = r;
                        s.mQueue.push_back(o);
                    }
                }
            }
            {
                std::vector<U32>& fill = s.mFill;
                fill.assign(regions + 1, 0);
                for (U32 i = 0; i < ncand; ++i)
                {
                    rof[i] = regionOf(cands[i]);
                    if (rof[i] >= 0)
                    {
                        ++fill[rof[i] + 1];
                    }
                }
                for (U32 r = 0; r < regions; ++r)
                {
                    fill[r + 1] += fill[r];
                }
                region_start = fill;
                region_cells.resize(fill[regions]);
                for (U32 i = 0; i < ncand; ++i)
                {
                    if (rof[i] >= 0)
                    {
                        region_cells[fill[rof[i]]++] = cands[i];
                    }
                }
            }
            // Each region's extent along its run, over its cells now: when first asked for, as a hillside's is only
            // where a deck search reaches it.
            const auto axisOf = [&](U32 r) -> const Axis& {
                Axis& a = axes[r];
                if (a.mOk && !a.mExtent)
                {
                    a.mExtent = true;
                    if (region_start[r] != region_start[r + 1])
                    {
                        a.mS0 = 1e300;
                        a.mS1 = -1e300;
                        for (U32 k = region_start[r]; k < region_start[r + 1]; ++k)
                        {
                            const F64 sv = sOf(a, region_cells[k]);
                            a.mS0 = std::min(a.mS0, sv);
                            a.mS1 = std::max(a.mS1, sv);
                        }
                    }
                }
                return a;
            };

            std::vector<F64>& values = s.mValues;
            std::vector<F64>& sv_of = s.mS;
            std::vector<std::pair<F64, F64>> corners;
            for (U32 r = 0; r < regions; ++r)
            {
                if (!axes[r].mOk)
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
                U8 cue = 0;
                // Its cells copied out once, in its order: every pass over them below reads them in a row.
                std::vector<FlightCell>& loc = s.mLocal;
                if (!hillside[r])
                {
                    loc.resize(nc);
                    for (U32 k = 0; k < nc; ++k)
                    {
                        loc[k] = cells[rc[k]];
                    }
                    U32 stair_cues = 0, ramp_cues = 0;
                    for (U32 k = 0; k < nc; ++k)
                    {
                        const U8 flags = loc[k].mFlags;
                        stair_cues += (flags & AirGap::CUE_STAIR) ? 1 : 0;
                        ramp_cues += (flags & AirGap::CUE_RAMP) ? 1 : 0;
                    }
                    cue = stair_cues * 2 > nc ? 1 : (ramp_cues * 2 > nc ? 2 : 0);
                }
                if (!cue && (area < FLIGHT_MIN_AREA_M2 || area > FLIGHT_MAX_AREA_UNCUED_M2))
                {
                    if (trace)
                    {
                        const Axis& ax = axisOf(r);
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
                Axis& ax = axes[r];
                sv_of.resize(nc);
                for (U32 k = 0; k < nc; ++k)
                {
                    sv_of[k] = static_cast<F64>(loc[k].mX) * ax.mUx + static_cast<F64>(loc[k].mY) * ax.mUy;
                }
                if (!ax.mExtent)
                {
                    ax.mExtent = true;
                    ax.mS0 = 1e300;
                    ax.mS1 = -1e300;
                    for (U32 k = 0; k < nc; ++k)
                    {
                        ax.mS0 = std::min(ax.mS0, sv_of[k]);
                        ax.mS1 = std::max(ax.mS1, sv_of[k]);
                    }
                }
                const F64 ux = ax.mUx, uy = ax.mUy, px = -uy, py = ux, s0 = ax.mS0, s1 = ax.mS1;
                F64 lmin = 1e300, lmax = -1e300, zb_sum = 0.0, zt_sum = 0.0;
                U32 nb = 0, nt = 0;
                for (U32 k = 0; k < nc; ++k)
                {
                    const F64 sv = sv_of[k];
                    const F64 l = static_cast<F64>(loc[k].mX) * px + static_cast<F64>(loc[k].mY) * py;
                    lmin = std::min(lmin, l);
                    lmax = std::max(lmax, l);
                    if (sv <= s0 + FLIGHT_END_BAND_M)
                    {
                        zb_sum += static_cast<F64>(loc[k].mFloor) * unit;
                        ++nb;
                    }
                    if (sv >= s1 - FLIGHT_END_BAND_M)
                    {
                        zt_sum += static_cast<F64>(loc[k].mFloor) * unit;
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
                    s.mSeen.clear();
                    s.mSeeds.clear();
                    for (U32 k = 0; k < nc; ++k)
                    {
                        const F64 sv = sv_of[k];
                        if (top ? sv < s1 - FLIGHT_END_REACH_M : sv > s0 + FLIGHT_END_REACH_M)
                        {
                            continue;
                        }
                        const U32 c = rc[k];
                        for (S32 d = 0; d < 4; ++d)
                        {
                            const U32 o = link[c * 4 + d];
                            if (o == NONE || s.mSeen.test(o) || regionOf(o) == static_cast<S32>(r) || std::fabs(zOf(o) - band) > FLIGHT_FLOOR_DZ_M ||
                                !beyond(o))
                            {
                                continue;
                            }
                            s.mSeen.set(o);
                            s.mSeeds.push_back(o);
                        }
                    }
                    s.mSeen2.clear();     // a second set keeps this walk's visits apart from its seeds
                    s.mQueue.assign(s.mSeeds.begin(), s.mSeeds.end());
                    s.mVisited.clear();
                    bool other = false, ground = false;
                    for (size_t head = 0; head < s.mQueue.size() && s.mVisited.size() * cell2 < FLIGHT_FLOOR_MIN_M2; ++head)
                    {
                        const U32 a = s.mQueue[head];
                        if (s.mSeen2.test(a))
                        {
                            continue;
                        }
                        s.mSeen2.set(a);
                        const S32 ra = regionOf(a);
                        if (ra >= 0 && (region_start[ra + 1] - region_start[ra]) * cell2 >= FLIGHT_MIN_AREA_M2)
                        {
                            const Axis& oa = axisOf(ra);
                            if (oa.mOk)
                            {
                                const F64 sa = sOf(oa, a);
                                if (top && sa <= oa.mS0 + FLIGHT_END_REACH_M + cell)
                                {
                                    other = true;
                                }
                                if (!top && sa >= oa.mS1 - FLIGHT_END_REACH_M - cell)
                                {
                                    other = true;
                                }
                            }
                            continue;
                        }
                        s.mVisited.push_back(a);
                        ground = ground || areaOf(cells[a].mFlags) == AREA_OF_GROUND;
                        for (S32 d = 0; d < 4; ++d)
                        {
                            const U32 o = link[a * 4 + d];
                            if (o == NONE || s.mSeen2.test(o) || regionOf(o) == static_cast<S32>(r) || std::fabs(zOf(o) - band) > FLIGHT_FLOOR_DZ_M ||
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
                    if (values.empty())
                    {
                        z_out = band;
                    }
                    else
                    {
                        std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
                        z_out = values[values.size() / 2];
                    }
                    return ground || other || s.mVisited.size() * cell2 >= FLIGHT_FLOOR_MIN_M2;
                };
                // Unless traced, a region is left as soon as it is refused whatever else is measured: unnamed and not
                // decked at an end, or not rising enough between its decks.
                F64 fb = 0.0, ft = 0.0;
                const bool deck_b = deck(false, fb);
                if (!trace && !cue && !deck_b)
                {
                    continue;
                }
                const bool deck_t = deck(true, ft);
                if (!trace && !cue && !deck_t)
                {
                    continue;
                }
                const F64 rise = ft - fb;
                if (!trace && (rise < FLIGHT_MIN_RISE_M || (rise < FLIGHT_MIN_RISE_UNCUED_M && !cue)))
                {
                    continue;
                }

                // Width: the median across-run extent of one-cell slices along the run. Slice floors: their medians
                // in order, for the going between risers of two units or more. Slices are counted into buckets.
                S32 bmax = 0;
                for (U32 k = 0; k < nc; ++k)
                {
                    bmax = std::max(bmax, static_cast<S32>(std::floor((sv_of[k] - s0) / cell + 0.5)));
                }
                const U32 nslices = static_cast<U32>(bmax) + 1;
                s.mSliceStart.assign(nslices + 1, 0);
                s.mSliceLo.assign(nslices, 1e300);
                s.mSliceHi.assign(nslices, -1e300);
                s.mSliceFloor.resize(nc);
                U32* sstart = s.mSliceStart.data();
                F64* slo = s.mSliceLo.data();
                F64* shi = s.mSliceHi.data();
                for (U32 k = 0; k < nc; ++k)
                {
                    const S32 b = static_cast<S32>(std::floor((sv_of[k] - s0) / cell + 0.5));
                    const F64 l = static_cast<F64>(loc[k].mX) * px + static_cast<F64>(loc[k].mY) * py;
                    slo[b] = std::min(slo[b], l);
                    shi[b] = std::max(shi[b], l);
                    ++sstart[b + 1];
                }
                for (U32 b = 0; b < nslices; ++b)
                {
                    sstart[b + 1] += sstart[b];
                }
                {
                    s.mFill.assign(sstart, sstart + nslices);
                    U32* fill = s.mFill.data();
                    for (U32 k = 0; k < nc; ++k)
                    {
                        const S32 b = static_cast<S32>(std::floor((sv_of[k] - s0) / cell + 0.5));
                        s.mSliceFloor[fill[b]++] = loc[k].mFloor;
                    }
                }
                // Per slice, its median floor: the middle of its floors in order, the upper of two.
                s.mSliceMedian.resize(nslices);
                S32* smed = s.mSliceMedian.data();
                values.clear();
                for (U32 b = 0; b < nslices; ++b)
                {
                    const U32 i = sstart[b], j = sstart[b + 1];
                    if (i == j)
                    {
                        continue;
                    }
                    S32* f = s.mSliceFloor.data();
                    std::nth_element(f + i, f + i + (j - i) / 2, f + j);
                    smed[b] = f[i + (j - i) / 2];
                    values.push_back(shi[b] - slo[b] + cell);
                }
                std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
                const F64 width = values[values.size() / 2];
                // Plateaus: runs of slices at one floor. Where the floor steps between them by two units or more on
                // average they are treads, and treads deeper on average than a stair's are seating; a unit's step is a slope.
                // The run between the slices clear of both decks: the first above the lower, the last below the upper.
                S32 b_above = S32_MAX, b_below = S32_MIN, bfirst = S32_MAX, blast = S32_MIN;
                for (U32 bu = 0; bu < nslices; ++bu)
                {
                    if (sstart[bu] == sstart[bu + 1])
                    {
                        continue;
                    }
                    const S32 b = static_cast<S32>(bu);
                    bfirst = std::min(bfirst, b);
                    blast = std::max(blast, b);
                    const F64 z = smed[b] * unit;
                    if (z > fb)
                    {
                        b_above = std::min(b_above, b);
                    }
                    if (z < ft)
                    {
                        b_below = std::max(b_below, b);
                    }
                }
                S32 b0 = bfirst, b1 = blast;
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
                for (S32 b = std::max(b0, 0); b <= b1; ++b)
                {
                    if (sstart[b] == sstart[b + 1])
                    {
                        continue;
                    }
                    const S32 med = smed[b];
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
                    const FlightCell& fc = loc[k];
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
                    const F64 x = loc[k].mX, y = loc[k].mY;
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
