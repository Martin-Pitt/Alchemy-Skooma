/**
 * @file sknavstairalign.cpp
 * @brief Stair portals squared to the flights they enter.
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

#include "sknavstairalign.h"

#include "llmath.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

namespace sknav
{
    namespace
    {
        constexpr F32 CURVE_STEP_M = 0.5f;          ///< a curve walk's stride, taken in two
        constexpr F32 CURVE_REACH_M = 0.75f;        ///< and the patch it refits the rise over
        constexpr S32 CURVE_MAX_STEPS = 16;
        constexpr F32 CURVE_MIN_TURN_DEG = 2.f;     ///< a step turning less is straight
        constexpr F32 CURVE_MAX_TURN_DEG = 45.f;    ///< and one turning more is a corner, not a curve
        constexpr F32 WIDTH_STEP_M = 0.125f;
        constexpr F32 WIDTH_MAX_M = 12.f;
        constexpr S32 WIDTH_LINES = 5;              ///< lines the width is measured on, a quarter metre apart
        constexpr F32 WIDTH_LINE_M = 0.25f;
        constexpr F32 MIN_WIDTH_M = 0.5f;           ///< a squared end narrower than this keeps its cut
        constexpr F32 LEVEL_SLOPE = 0.1405f;        ///< tan 8 degrees: flatter is a landing
        constexpr S32 REFITS = 3;
        constexpr F32 SIDE_COS = 0.1736f;           ///< cos 80 degrees: a cut this far off the treads runs along the flight
        constexpr F32 EDGE_SHY = 0.375f;            ///< steps from a cell centre start this far in, so they never sample on a cell edge
        constexpr S32 INWARD_TRIES = 5;             ///< cells a contact's flight side may start this far in
        constexpr F32 SETTLED_COS = 0.9999996f;     ///< within 0.05 degrees

        LLVector2 perp(const LLVector2& v) { return LLVector2(-v.mV[VY], v.mV[VX]); }
        F32 cross(const LLVector2& a, const LLVector2& b) { return a.mV[VX] * b.mV[VY] - a.mV[VY] * b.mV[VX]; }
        LLVector2 rotated(const LLVector2& v, F32 rad)
        {
            const F32 c = std::cos(rad), s = std::sin(rad);
            return LLVector2(c * v.mV[VX] - s * v.mV[VY], s * v.mV[VX] + c * v.mV[VY]);
        }
        F32 signedDeg(const LLVector2& a, const LLVector2& b) { return std::atan2(cross(a, b), a * b) * RAD_TO_DEG; }

        /// The first point on the flight walking from p along dir, as a contact's own cell may lie on the floor's side.
        bool inward(const FlightFloor& floor, const LLVector2& p, const LLVector2& dir, F32 z, LLVector2& q, F32& zq)
        {
            for (S32 k = 0; k < INWARD_TRIES; ++k)
            {
                q = p + dir * ((k + EDGE_SHY) * STAIR_FIT_STEP_M);
                if (floor(q.mV[VX], q.mV[VY], z, zq))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool flightRise(const FlightFloor& floor, const LLVector2& c, F32 z, const LLVector2& dir, F32 reach, LLVector2& rise)
    {
        const F32 h = STAIR_FIT_STEP_M;
        const S32 n = std::max(1, static_cast<S32>(std::ceil(reach / h)));
        const bool half = dir.lengthSquared() > 0.f;
        LLVector2 u = half ? dir : LLVector2(1.f, 0.f);
        u.normVec();
        // The lattice runs at 45 degrees to the patch: one along it would line its rows up with the treads and keep any skew it started with.
        const LLVector2 lu = rotated(u, F_PI / 4.f), lv = perp(lu);
        const S32 m2 = 2 * n, w = 2 * m2 + 1;
        const F32 lead = half ? EDGE_SHY * h : 0.f;   // a half patch starts just inside the flight
        auto at = [&](S32 i, S32 j) { return c + u * lead + lv * (i * h) + lu * (j * h); };
        auto slot = [&](S32 i, S32 j) { return static_cast<size_t>(j + m2) * w + (i + m2); };
        // Within reach, and for a half patch on its side: lu and lv are u turned 45 degrees either way, so a point's reach along u is j - i.
        auto inside = [&](S32 i, S32 j) { return std::abs(i) <= m2 && std::abs(j) <= m2 && i * i + j * j <= n * n && (!half || j >= i); };
        std::vector<U8> tried(static_cast<size_t>(w) * w, 0);
        std::vector<F32> zs(tried.size(), 0.f);
        std::deque<std::pair<S32, S32>> open;
        F64 sx = 0, sy = 0, sz = 0, sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
        S32 count = 0;
        auto take = [&](S32 i, S32 j, F32 guess) {
            const size_t k = slot(i, j);
            if (tried[k])
            {
                return;
            }
            tried[k] = 1;
            const LLVector2 p = at(i, j);
            F32 f;
            if (!floor(p.mV[VX], p.mV[VY], guess, f))
            {
                return;
            }
            zs[k] = f;
            open.emplace_back(i, j);
            const F64 x = p.mV[VX] - c.mV[VX], y = p.mV[VY] - c.mV[VY];
            sx += x;
            sy += y;
            sz += f;
            sxx += x * x;
            sxy += x * y;
            syy += y * y;
            sxz += x * f;
            syz += y * f;
            ++count;
        };
        // A half patch seeds at the first lattice point on the flight along dir: (-k, k) lies k cells' diagonal along it.
        for (S32 k = 0; k < (half ? INWARD_TRIES : 1) && !count; ++k)
        {
            take(-k, k, z);
        }
        while (!open.empty())
        {
            const auto [i, j] = open.front();
            open.pop_front();
            const F32 here = zs[slot(i, j)];
            const S32 di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 a = i + di[d], b = j + dj[d];
                if (inside(a, b))
                {
                    take(a, b, here);
                }
            }
        }
        if (count < 6)
        {
            return false;
        }
        const F64 m = count;
        const F64 cxx = sxx - sx * sx / m, cxy = sxy - sx * sy / m, cyy = syy - sy * sy / m;
        const F64 cxz = sxz - sx * sz / m, cyz = syz - sy * sz / m;
        const F64 det = cxx * cyy - cxy * cxy, tr = cxx + cyy;
        // A patch much thinner than it is long cannot tell which way it climbs across itself.
        if (tr <= 0.0 || det <= 0.01 * tr * tr)
        {
            return false;
        }
        const F64 a = (cyy * cxz - cxy * cyz) / det, b = (cxx * cyz - cxy * cxz) / det;
        const F64 slope = std::sqrt(a * a + b * b);
        if (slope < LEVEL_SLOPE)
        {
            return false;
        }
        rise.set(static_cast<F32>(a / slope), static_cast<F32>(b / slope));
        return true;
    }

    bool flightCurves(const FlightFloor& floor, const LLVector2& c, F32 z, const LLVector2& rise, bool up)
    {
        const F32 sign = up ? 1.f : -1.f;
        LLVector2 h = rise, p = c;
        F32 zp = z, sum = 0.f;
        S32 run = 0;
        for (S32 k = 0; k < CURVE_MAX_STEPS; ++k)
        {
            for (S32 sub = 0; sub < 2; ++sub)
            {
                const LLVector2 q = p + h * (sign * 0.5f * CURVE_STEP_M);
                F32 zq;
                if (!floor(q.mV[VX], q.mV[VY], zp, zq))
                {
                    return false;
                }
                p = q;
                zp = zq;
            }
            LLVector2 g;
            if (!flightRise(floor, p, zp, LLVector2(), CURVE_REACH_M, g))
            {
                return false;   // a landing or the far end: what climbed so far was straight
            }
            const F32 turn = signedDeg(h, g);
            if (std::fabs(turn) > CURVE_MAX_TURN_DEG)
            {
                return false;
            }
            if (std::fabs(turn) < CURVE_MIN_TURN_DEG)
            {
                sum = 0.f;
                run = 0;
            }
            else if (run > 0 && (turn > 0.f) == (sum > 0.f))
            {
                sum += turn;
                ++run;
            }
            else
            {
                sum = turn;
                run = 1;
            }
            if (run >= STAIR_CURVE_STEPS && std::fabs(sum) >= STAIR_CURVE_DEG)
            {
                return true;
            }
            h = g;
        }
        return false;
    }

    void alignStairEnds(const FlightFloor& floor, std::vector<StairEnd>& ends)
    {
        std::vector<F32> inner_z(ends.size(), 0.f);
        for (size_t i = 0; i < ends.size(); ++i)
        {
            StairEnd& e = ends[i];
            e.mRise.setZero();
            e.mFree = false;
            e.mAligned = false;
            LLVector2 into = e.mInto;
            into.normVec();
            LLVector2 inner, g;
            if (!inward(floor, e.mSeed, into, e.mZ, inner, inner_z[i]))
            {
                continue;
            }
            F32 reach = STAIR_FIT_REACH_M;
            if (!flightRise(floor, e.mSeed, e.mZ, into, reach, g))
            {
                reach = STAIR_FIT_FAR_M;
                if (!flightRise(floor, e.mSeed, e.mZ, into, reach, g))
                {
                    continue;
                }
            }
            // Refit on a patch facing the rise until it settles, so its samples run along the treads, not across their edges.
            for (S32 k = 0; k < REFITS; ++k)
            {
                LLVector2 g2;
                if (!flightRise(floor, e.mSeed, e.mZ, g * (g * into > 0.f ? 1.f : -1.f), reach, g2))
                {
                    break;
                }
                const bool settled = g2 * g > SETTLED_COS;
                g = g2;
                if (settled)
                {
                    break;
                }
            }
            e.mRise = g;
            e.mFree = flightCurves(floor, inner, inner_z[i], g, g * into > 0.f);
        }
        // Each flight's widest measured end leads; the rest turn from it by whole right angles where they nearly do.
        std::map<U32, size_t> lead;
        for (size_t i = 0; i < ends.size(); ++i)
        {
            const StairEnd& e = ends[i];
            if (e.mFree || e.mRise.isExactlyZero())
            {
                continue;
            }
            const auto it = lead.find(e.mFlight);
            if (it == lead.end() || dist_vec(e.mA, e.mB) > dist_vec(ends[it->second].mA, ends[it->second].mB))
            {
                lead[e.mFlight] = i;
            }
        }
        for (size_t i = 0; i < ends.size(); ++i)
        {
            StairEnd& e = ends[i];
            if (e.mFree || e.mRise.isExactlyZero())
            {
                continue;
            }
            const LLVector2 ref = ends[lead[e.mFlight]].mRise;
            const F32 phi = signedDeg(ref, e.mRise);
            const F32 k = std::round(phi / 90.f);
            if (std::fabs(phi - 90.f * k) <= STAIR_SNAP_DEG)
            {
                e.mRise = rotated(ref, k * F_PI_BY_TWO);
            }
            const LLVector2 t = perp(e.mRise);
            LLVector2 cut = e.mB - e.mA;
            cut.normVec();
            if (std::fabs(cut * t) < SIDE_COS)
            {
                continue;   // a contact along the flight's side, not across an end
            }
            const LLVector2 in = e.mRise * (e.mRise * e.mInto > 0.f ? 1.f : -1.f);
            LLVector2 q0;
            F32 z0;
            if (!inward(floor, e.mSeed, in, inner_z[i], q0, z0))
            {
                continue;
            }
            // The flight's width along the treads just inside the end: the widest of a few lines, as its footprint's corner may clip the first.
            F32 reach[2] = { 0.f, 0.f };
            F32 zk = z0;
            for (S32 k = 0; k < WIDTH_LINES; ++k)
            {
                const LLVector2 qk = q0 + in * (k * WIDTH_LINE_M);
                if (k > 0 && !floor(qk.mV[VX], qk.mV[VY], zk, zk))
                {
                    break;
                }
                F32 line[2] = { 0.f, 0.f };
                for (S32 side = 0; side < 2; ++side)
                {
                    const F32 s = side ? 1.f : -1.f;
                    F32 zl = zk;
                    while (line[side] < 0.5f * WIDTH_MAX_M)
                    {
                        const LLVector2 q = qk + t * (s * (line[side] + WIDTH_STEP_M));
                        F32 zq;
                        if (!floor(q.mV[VX], q.mV[VY], zl, zq) || std::fabs(zq - zk) > STAIR_TREAD_DZ_M)
                        {
                            break;
                        }
                        line[side] += WIDTH_STEP_M;
                        zl = zq;
                    }
                }
                if (line[0] + line[1] > reach[0] + reach[1])
                {
                    reach[0] = line[0];
                    reach[1] = line[1];
                }
            }
            if (reach[0] + reach[1] < MIN_WIDTH_M)
            {
                continue;
            }
            // On the tread line through the cut's middle, across the flight where it was measured.
            const LLVector2 base = q0 + in * ((e.mPivot - q0) * in);
            LLVector2 a = base - t * reach[0], b = base + t * reach[1];
            if ((b - a) * (e.mB - e.mA) < 0.f)
            {
                std::swap(a, b);
            }
            e.mA = a;
            e.mB = b;
            e.mAligned = true;
        }
    }
}
