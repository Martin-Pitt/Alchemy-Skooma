/**
 * @file sknavflights.cpp
 * @brief The slope partition by smoothing groups, and the stair flights it finds.
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

#include "sknavflights.h"

#include "llstring.h"

#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace
{
    using namespace sknav;

    constexpr F64 SEAM_DZ = 0.02;       ///< closer than this across a boundary is the same surface
    constexpr F64 PROBE_M = 0.05;       ///< how far past a boundary edge to look for the neighbour
    constexpr F64 OPPOSITE = -0.7;      ///< cos between a tread's up and down riser directions
    constexpr F64 GRID_M = 1.0;         ///< the probe index's cell, at its finest
    // The index spans every walkable triangle in the soup, which is not clipped
    // to the tile: one sky dome or kilometre-wide mesh in it would ask for a
    // grid of billions of cells. The cell grows until the grid fits this.
    constexpr size_t GRID_MAX_CELLS = 1u << 18;
    constexpr size_t MAX_PAIRS = 1 << 16; ///< a group with more riser pairs than this is no tread

    struct V3
    {
        F64 x = 0, y = 0, z = 0;
    };

    struct Boundary
    {
        S32 mGroup;
        S32 mTri;
        S32 mA;
        S32 mB;
        F64 mOx;    ///< outward in plan
        F64 mOy;
    };

    struct Riser
    {
        S32 mOther;     ///< the group across it
        F64 mDz;        ///< up is positive
        F64 mOx;
        F64 mOy;
        S32 mA;         ///< the boundary edge's welded vertices
        S32 mB;
    };

    struct WeldKey
    {
        S64 mX, mY, mZ;
        bool operator==(const WeldKey& o) const { return mX == o.mX && mY == o.mY && mZ == o.mZ; }
    };

    struct WeldHash
    {
        size_t operator()(const WeldKey& k) const
        {
            U64 h = 1469598103934665603ull;
            for (const S64 v : { k.mX, k.mY, k.mZ })
            {
                h = (h ^ static_cast<U64>(v)) * 1099511628211ull;
                h ^= h >> 29;
            }
            return static_cast<size_t>(h);
        }
    };

    struct Partitioner
    {
        std::vector<V3> mV;             // welded
        std::vector<S32> mT;            // welded, 3 per input triangle
        std::vector<U8> mValid;         // not degenerate after the weld
        std::vector<V3> mN;             // up-facing normal
        std::vector<V3> mC;             // centroid
        std::vector<F64> mArea;
        std::vector<U8> mWalk;
        std::vector<U8> mLevel;
        std::vector<U8> mPart;          // of the partitioned area
        std::vector<S32> mParent;
        std::vector<std::pair<U64, S32>> mEdges;   // sorted (edge key, triangle)
        F64 mCosTurn = 0;
        F64 mClimb = 0.75;

        // the probe index
        F64 mGx0 = 0, mGy0 = 0;
        F64 mGcell = GRID_M;
        S32 mGw = 0, mGh = 0;
        std::vector<std::vector<S32>> mGrid;

        S32 ntris() const { return static_cast<S32>(mT.size() / 3); }

        S32 find(S32 i)
        {
            while (mParent[i] != i)
            {
                mParent[i] = mParent[mParent[i]];
                i = mParent[i];
            }
            return i;
        }

        void unite(S32 a, S32 b)
        {
            a = find(a);
            b = find(b);
            if (a != b)
            {
                mParent[std::max(a, b)] = std::min(a, b);
            }
        }

        bool sameSlope(S32 a, S32 b) const
        {
            if (mPart[a] != mPart[b])
            {
                return false;
            }
            if (mLevel[a] || mLevel[b])
            {
                return mLevel[a] && mLevel[b];
            }
            // the uphill directions in plan
            const F64 ax = -mN[a].x / mN[a].z, ay = -mN[a].y / mN[a].z;
            const F64 bx = -mN[b].x / mN[b].z, by = -mN[b].y / mN[b].z;
            return ax * bx + ay * by >= mCosTurn * std::sqrt(ax * ax + ay * ay) * std::sqrt(bx * bx + by * by);
        }

        void build(const std::vector<LLVector3>& verts, const std::vector<S32>& tris, const std::vector<U8>& areas,
                   const std::vector<U8>& two_sided, U8 part_area)
        {
            // weld on the millimetre, across the whole soup
            std::unordered_map<WeldKey, S32, WeldHash> key;
            std::vector<S32> remap(verts.size());
            for (size_t i = 0; i < verts.size(); ++i)
            {
                const S64 kx = std::llround(verts[i].mV[VX] * 1000.0), ky = std::llround(verts[i].mV[VY] * 1000.0),
                          kz = std::llround(verts[i].mV[VZ] * 1000.0);
                const WeldKey k { kx, ky, kz };
                auto it = key.find(k);
                if (it == key.end())
                {
                    it = key.emplace(k, static_cast<S32>(mV.size())).first;
                    mV.push_back({ kx / 1000.0, ky / 1000.0, kz / 1000.0 });
                }
                remap[i] = it->second;
            }
            const S32 n = static_cast<S32>(tris.size() / 3);
            mT.resize(tris.size());
            mValid.assign(n, 0);
            mN.assign(n, V3());
            mC.assign(n, V3());
            mArea.assign(n, 0.0);
            mWalk.assign(n, 0);
            mLevel.assign(n, 0);
            mPart.assign(n, 0);
            mParent.resize(n);
            const F64 cos_level = std::cos(SLOPE_LEVEL_DEG * DEG_TO_RAD);
            for (S32 t = 0; t < n; ++t)
            {
                mParent[t] = t;
                const S32 a = remap[tris[t * 3]], b = remap[tris[t * 3 + 1]], c = remap[tris[t * 3 + 2]];
                mT[t * 3] = a;
                mT[t * 3 + 1] = b;
                mT[t * 3 + 2] = c;
                if (a == b || b == c || a == c)
                {
                    continue;
                }
                const V3 &pa = mV[a], &pb = mV[b], &pc = mV[c];
                const F64 ux = pb.x - pa.x, uy = pb.y - pa.y, uz = pb.z - pa.z;
                const F64 vx = pc.x - pa.x, vy = pc.y - pa.y, vz = pc.z - pa.z;
                V3 nn { uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
                const F64 len = std::sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
                if (len < 1e-12)
                {
                    continue;
                }
                nn.x /= len;
                nn.y /= len;
                nn.z /= len;
                if (nn.z < 0 && two_sided[t])
                {
                    nn.x = -nn.x;
                    nn.y = -nn.y;
                    nn.z = -nn.z;
                }
                mValid[t] = 1;
                mN[t] = nn;
                mC[t] = { (pa.x + pb.x + pc.x) / 3, (pa.y + pb.y + pc.y) / 3, (pa.z + pb.z + pc.z) / 3 };
                mArea[t] = len / 2;
                mWalk[t] = areas[t] != 0 && nn.z > 0;
                mLevel[t] = nn.z >= cos_level;
                mPart[t] = areas[t] == part_area;
                for (S32 k = 0; k < 3; ++k)
                {
                    const S32 i = mT[t * 3 + k], j = mT[t * 3 + (k + 1) % 3];
                    mEdges.emplace_back((static_cast<U64>(std::min(i, j)) << 32) | static_cast<U64>(std::max(i, j)), t);
                }
            }
            std::sort(mEdges.begin(), mEdges.end());
            buildGrid();
        }

        template <typename Fn> void eachEdge(Fn&& fn) const
        {
            for (size_t s = 0; s < mEdges.size();)
            {
                size_t e = s + 1;
                while (e < mEdges.size() && mEdges[e].first == mEdges[s].first)
                {
                    ++e;
                }
                fn(mEdges[s].first, s, e);
                s = e;
            }
        }

        void buildGrid()
        {
            F64 x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            for (S32 t = 0; t < ntris(); ++t)
            {
                if (!mWalk[t])
                {
                    continue;
                }
                for (S32 k = 0; k < 3; ++k)
                {
                    const V3& p = mV[mT[t * 3 + k]];
                    x0 = std::min(x0, p.x);
                    y0 = std::min(y0, p.y);
                    x1 = std::max(x1, p.x);
                    y1 = std::max(y1, p.y);
                }
            }
            if (x1 < x0)
            {
                return;
            }
            mGx0 = x0;
            mGy0 = y0;
            mGcell = std::max(GRID_M, std::sqrt((x1 - x0 + GRID_M) * (y1 - y0 + GRID_M) / static_cast<F64>(GRID_MAX_CELLS)));
            mGw = static_cast<S32>((x1 - x0) / mGcell) + 1;
            mGh = static_cast<S32>((y1 - y0) / mGcell) + 1;
            mGrid.assign(static_cast<size_t>(mGw) * mGh, {});
            for (S32 t = 0; t < ntris(); ++t)
            {
                if (!mWalk[t])
                {
                    continue;
                }
                F64 tx0 = 1e30, ty0 = 1e30, tx1 = -1e30, ty1 = -1e30;
                for (S32 k = 0; k < 3; ++k)
                {
                    const V3& p = mV[mT[t * 3 + k]];
                    tx0 = std::min(tx0, p.x);
                    ty0 = std::min(ty0, p.y);
                    tx1 = std::max(tx1, p.x);
                    ty1 = std::max(ty1, p.y);
                }
                const S32 cx0 = static_cast<S32>((tx0 - mGx0) / mGcell), cx1 = static_cast<S32>((tx1 - mGx0) / mGcell);
                const S32 cy0 = static_cast<S32>((ty0 - mGy0) / mGcell), cy1 = static_cast<S32>((ty1 - mGy0) / mGcell);
                for (S32 cy = cy0; cy <= cy1; ++cy)
                {
                    for (S32 cx = cx0; cx <= cx1; ++cx)
                    {
                        mGrid[static_cast<size_t>(cy) * mGw + cx].push_back(t);
                    }
                }
            }
        }

        /// z of triangle t over a point in plan, if it covers it.
        bool zAt(S32 t, F64 px, F64 py, F64& z) const
        {
            const V3 &a = mV[mT[t * 3]], &b = mV[mT[t * 3 + 1]], &c = mV[mT[t * 3 + 2]];
            const F64 d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::fabs(d) < 1e-12)
            {
                return false;
            }
            const F64 l1 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / d;
            const F64 l2 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / d;
            const F64 l3 = 1 - l1 - l2;
            if (std::min(l1, std::min(l2, l3)) < -1e-9)
            {
                return false;
            }
            z = l1 * a.z + l2 * b.z + l3 * c.z;
            return true;
        }

        void boundaries(const std::vector<S32>& g_of, std::vector<Boundary>& out) const
        {
            out.clear();
            eachEdge([&](U64 key, size_t s, size_t e) {
                const S32 i = static_cast<S32>(key >> 32), j = static_cast<S32>(key & 0xffffffff);
                for (size_t a = s; a < e; ++a)
                {
                    const S32 t = mEdges[a].second;
                    if (!mWalk[t])
                    {
                        continue;
                    }
                    bool inner = false;
                    for (size_t b = s; b < e && !inner; ++b)
                    {
                        const S32 u = mEdges[b].second;
                        inner = u != t && mWalk[u] && g_of[u] == g_of[t];
                    }
                    if (inner)
                    {
                        continue;
                    }
                    const V3 &p = mV[i], &q = mV[j];
                    const F64 dx = q.x - p.x, dy = q.y - p.y, len = std::sqrt(dx * dx + dy * dy);
                    if (len < 1e-6)
                    {
                        continue;
                    }
                    F64 ox = dy / len, oy = -dx / len;
                    if (ox * ((p.x + q.x) / 2 - mC[t].x) + oy * ((p.y + q.y) / 2 - mC[t].y) < 0)
                    {
                        ox = -ox;
                        oy = -oy;
                    }
                    out.push_back({ g_of[t], t, i, j, ox, oy });
                }
            });
        }

        /// The walkable surface of another group nearest in height just past
        /// the edge, within climb.
        bool probe(const Boundary& e, const std::vector<S32>& g_of, S32& u_out, F64& dz_out) const
        {
            const V3 &p = mV[e.mA], &q = mV[e.mB];
            const F64 px = (p.x + q.x) / 2 + e.mOx * PROBE_M, py = (p.y + q.y) / 2 + e.mOy * PROBE_M;
            const V3& n = mN[e.mTri];
            const V3& c = mC[e.mTri];
            const F64 z_here = c.z - (n.x * (px - c.x) + n.y * (py - c.y)) / n.z;
            const S32 cx = static_cast<S32>(std::floor((px - mGx0) / mGcell)), cy = static_cast<S32>(std::floor((py - mGy0) / mGcell));
            if (cx < 0 || cy < 0 || cx >= mGw || cy >= mGh)
            {
                return false;
            }
            bool found = false;
            for (const S32 u : mGrid[static_cast<size_t>(cy) * mGw + cx])
            {
                F64 z = 0;
                if (g_of[u] == e.mGroup || !zAt(u, px, py, z))
                {
                    continue;
                }
                const F64 dz = z - z_here;
                if (std::fabs(dz) <= mClimb && (!found || std::fabs(dz) < std::fabs(dz_out)))
                {
                    found = true;
                    u_out = u;
                    dz_out = dz;
                }
            }
            return found;
        }

        std::vector<S32> groupsOf()
        {
            std::vector<S32> g(ntris(), -1);
            for (S32 t = 0; t < ntris(); ++t)
            {
                if (mWalk[t])
                {
                    g[t] = find(t);
                }
            }
            return g;
        }
    };

    F64 cross(const LLVector2& o, const LLVector2& a, const LLVector2& b)
    {
        return static_cast<F64>(a.mV[0] - o.mV[0]) * (b.mV[1] - o.mV[1]) - static_cast<F64>(a.mV[1] - o.mV[1]) * (b.mV[0] - o.mV[0]);
    }

    std::vector<LLVector2> hull(std::vector<LLVector2> pts)
    {
        std::sort(pts.begin(), pts.end(), [](const LLVector2& a, const LLVector2& b) {
            return a.mV[0] < b.mV[0] || (a.mV[0] == b.mV[0] && a.mV[1] < b.mV[1]);
        });
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        if (pts.size() < 3)
        {
            return pts;
        }
        std::vector<LLVector2> h(pts.size() * 2);
        size_t k = 0;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0)
            {
                --k;
            }
            h[k++] = pts[i];
        }
        for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
        {
            while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0)
            {
                --k;
            }
            h[k++] = pts[i - 1];
        }
        h.resize(k - 1);
        return h;
    }

    /// Distance from a point to a convex counter-clockwise polygon, 0 inside.
    F32 outside(const std::vector<LLVector2>& poly, const LLVector2& p)
    {
        if (poly.size() < 3)
        {
            F32 best = 1e30f;
            for (const LLVector2& q : poly)
            {
                best = llmin(best, (q - p).length());
            }
            return best;
        }
        F32 worst = 0.f;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const LLVector2& a = poly[i];
            const LLVector2& b = poly[(i + 1) % poly.size()];
            const LLVector2 d = b - a;
            const F32 len = d.length();
            if (len < 1e-6f)
            {
                continue;
            }
            // positive to the right of a counter-clockwise edge: outside
            worst = llmax(worst, (d.mV[0] * (p.mV[1] - a.mV[1]) - d.mV[1] * (p.mV[0] - a.mV[0])) / -len);
        }
        return worst;
    }

    constexpr F64 NEAR_XY = 0.15;           ///< this close in plan counts as touching
    constexpr F64 MIN_SLOPE_DEG = 5.0;
    constexpr F64 FLOORISH_DEG = 10.0;      ///< a slope this gentle and FLOORISH_M2 big is a floor
    constexpr F64 FLOORISH_M2 = 20.0;
    constexpr F64 TREAD_MAX_M2 = 6.0;       ///< a level group bigger than this is a floor, whatever its risers say
    constexpr F64 PLANAR = 0.92;            ///< a slope group whose normals agree less than this is furniture
    constexpr F64 TURN_DEG = 30.0;          ///< joined slopes whose runs differ more are two flights
    constexpr F64 JOIN_DZ = 0.5;
    constexpr F64 JOIN_SLOPE_DEG = 20.0;    ///< slope groups steeper or gentler than each other by more are two flights
    constexpr F64 MIN_WIDTH_M = 0.8;        ///< narrower is a rail or a wall top; a ship's stair is 0.88 m
    constexpr F64 MIN_AREA_M2 = 1.5;        ///< without a name, smaller is a prop
    constexpr F64 MAX_AREA_UNCUED_M2 = 100.0;   ///< and bigger is a roof or a hillside
    constexpr F64 MAX_RUN_M = 12.0;
    constexpr F64 PAST_M = 0.3;             ///< a floor is looked for this far past an end
    constexpr F64 BEYOND_M = 0.6;           ///< and must carry on this much further, or it is a rail or a wall top
    constexpr F64 FLOOR_DZ = 0.75;          ///< from the end's height
    constexpr F64 FLOOR_UP = 1.25;          ///< a named stair may sink this far into its floors
    constexpr F64 LEVEL_DZ = 0.3;
    constexpr F64 FLOOR_MIN_M2 = 4.0;       ///< without a name, a smaller floor at an end is a prop top
    constexpr F64 MIN_RISE_UNCUED_M = 1.0;  ///< without a name or treads, a smaller climb is a step
    constexpr F64 MAX_GOING_M = 0.6;        ///< and deeper than this, seating
    constexpr F64 MIN_RUN_M = 0.5;
    constexpr F64 BURIED_DZ = 0.1;          ///< a floor this far above a flight's slope covers it
    constexpr F64 EXPOSED_STEP_M = 0.1;     ///< where a sunk flight comes out of its floor, found to this

    /// Height of walkable triangle t at a point in plan: over it, or within NEAR_XY of an edge.
    bool nearZ(const Partitioner& P, S32 t, F64 px, F64 py, F64& z)
    {
        if (P.zAt(t, px, py, z))
        {
            return true;
        }
        for (S32 k = 0; k < 3; ++k)
        {
            const V3 &a = P.mV[P.mT[t * 3 + k]], &b = P.mV[P.mT[t * 3 + (k + 1) % 3]];
            const F64 ax = b.x - a.x, ay = b.y - a.y, d = ax * ax + ay * ay;
            const F64 u = d > 0 ? std::clamp(((px - a.x) * ax + (py - a.y) * ay) / d, 0.0, 1.0) : 0.0;
            if (std::hypot(a.x + u * ax - px, a.y + u * ay - py) <= NEAR_XY)
            {
                z = a.z + u * (b.z - a.z);
                return true;
            }
        }
        return false;
    }

    /// Walkable triangles whose index cells come within NEAR_XY of a point, each once.
    void trianglesNear(const Partitioner& P, F64 px, F64 py, std::vector<S32>& out)
    {
        out.clear();
        if (!std::isfinite(px) || !std::isfinite(py))
        {
            return;
        }
        // Clamp before the cast: a point far off the grid would otherwise wrap and cover all of it.
        const auto cell = [](F64 v, S32 n) { return static_cast<S32>(std::clamp(std::floor(v), -1.0, static_cast<F64>(n))); };
        const S32 cx0 = cell((px - NEAR_XY - P.mGx0) / P.mGcell, P.mGw);
        const S32 cx1 = cell((px + NEAR_XY - P.mGx0) / P.mGcell, P.mGw);
        const S32 cy0 = cell((py - NEAR_XY - P.mGy0) / P.mGcell, P.mGh);
        const S32 cy1 = cell((py + NEAR_XY - P.mGy0) / P.mGcell, P.mGh);
        // A triangle spans several cells; a per-triangle stamp dedups in linear time and keeps grid order.
        thread_local std::vector<U32> seen;
        thread_local U32 stamp = 0;
        if (seen.size() < P.mArea.size())
        {
            seen.assign(P.mArea.size(), 0);
            stamp = 0;
        }
        if (++stamp == 0)
        {
            std::fill(seen.begin(), seen.end(), 0);
            stamp = 1;
        }
        for (S32 cy = std::max(cy0, 0); cy <= std::min(cy1, P.mGh - 1); ++cy)
        {
            for (S32 cx = std::max(cx0, 0); cx <= std::min(cx1, P.mGw - 1); ++cx)
            {
                for (const S32 t : P.mGrid[static_cast<size_t>(cy) * P.mGw + cx])
                {
                    if (seen[t] != stamp)
                    {
                        seen[t] = stamp;
                        out.push_back(t);
                    }
                }
            }
        }
    }

    /// Uphill in plan over the sloped triangles, area weighted; false when they are level.
    bool uphill(const Partitioner& P, const std::vector<S32>& tris, F64& ux, F64& uy)
    {
        const F64 cos_level = std::cos(SLOPE_LEVEL_DEG * DEG_TO_RAD);
        F64 gx = 0, gy = 0;
        for (const S32 t : tris)
        {
            if (P.mN[t].z < cos_level)
            {
                gx += P.mN[t].x * P.mArea[t];
                gy += P.mN[t].y * P.mArea[t];
            }
        }
        const F64 l = std::hypot(gx, gy);
        if (l < 1e-6)
        {
            return false;
        }
        ux = -gx / l;
        uy = -gy / l;
        return true;
    }

    /// For a chain of level treads: the plan direction height rises along, fitted over the treads' centroids.
    bool risesAlong(const Partitioner& P, const std::vector<S32>& groups, const std::vector<std::vector<S32>>& tris_of, F64& ux, F64& uy)
    {
        std::vector<V3> c(groups.size());
        std::vector<F64> wg(groups.size(), 0.0);
        for (size_t i = 0; i < groups.size(); ++i)
        {
            for (const S32 t : tris_of[groups[i]])
            {
                wg[i] += P.mArea[t];
                c[i].x += P.mC[t].x * P.mArea[t];
                c[i].y += P.mC[t].y * P.mArea[t];
                c[i].z += P.mC[t].z * P.mArea[t];
            }
            const F64 w = std::max(wg[i], 1e-9);
            c[i] = { c[i].x / w, c[i].y / w, c[i].z / w };
        }
        F64 w = 0, mx = 0, my = 0, mz = 0;
        for (size_t i = 0; i < groups.size(); ++i)
        {
            w += wg[i];
            mx += c[i].x * wg[i];
            my += c[i].y * wg[i];
            mz += c[i].z * wg[i];
        }
        if (w <= 0)
        {
            return false;
        }
        mx /= w;
        my /= w;
        mz /= w;
        F64 sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0;
        for (size_t i = 0; i < groups.size(); ++i)
        {
            const F64 x = c[i].x - mx, y = c[i].y - my, z = c[i].z - mz, a = wg[i];
            sxx += x * x * a;
            syy += y * y * a;
            sxy += x * y * a;
            sxz += x * z * a;
            syz += y * z * a;
        }
        const F64 det = sxx * syy - sxy * sxy, spread = sxx + syy;
        if (spread <= 1e-12)
        {
            return false;
        }
        if (std::fabs(det) <= 1e-6 * spread * spread)
        {
            // collinear in plan: along the line, the way height rises
            const F64 angle = 0.5 * std::atan2(2 * sxy, sxx - syy), dx = std::cos(angle), dy = std::sin(angle);
            const F64 sign = sxz * dx + syz * dy;
            if (std::fabs(sign) < 1e-9)
            {
                return false;
            }
            ux = sign > 0 ? dx : -dx;
            uy = sign > 0 ? dy : -dy;
            return true;
        }
        const F64 a = (sxz * syy - syz * sxy) / det, b = (syz * sxx - sxz * sxy) / det, l = std::hypot(a, b);
        if (l < 1e-6)
        {
            return false;
        }
        ux = a / l;
        uy = b / l;
        return true;
    }

    /// Stairs and ramps, one per flight: slope groups and small tread groups,
    /// joined where they step into each other unless their runs turn. A
    /// flight's ends are its own extent along its run, on its centreline,
    /// clipped to its floors' heights; the floors just past each end only say
    /// that it connects two of them.
    void buildConnectors(Partitioner& P, const std::vector<S32>& grp, const std::vector<U8>& kind, S32 ngroups, const std::vector<U8>& codes,
                         const std::vector<U8>& cues, const std::vector<std::pair<S32, S32>>& level_links, F64 max_grade_deg,
                         std::vector<Flight>& flights)
    {
        const S32 n = P.ntris();
        std::vector<std::vector<S32>> tris_of(ngroups);
        std::vector<F64> area(ngroups, 0.0);
        std::vector<V3> mean(ngroups);
        for (S32 t = 0; t < n; ++t)
        {
            if (grp[t] >= 0)
            {
                const S32 g = grp[t];
                tris_of[g].push_back(t);
                area[g] += P.mArea[t];
                mean[g].x += P.mN[t].x * P.mArea[t];
                mean[g].y += P.mN[t].y * P.mArea[t];
                mean[g].z += P.mN[t].z * P.mArea[t];
            }
        }
        std::vector<U8> cand(ngroups, 0), has_dir(ngroups, 0);
        std::vector<F64> dir_x(ngroups, 0.0), dir_y(ngroups, 0.0), slope_deg(ngroups, 0.0), own_rise(ngroups, 0.0);
        bool any = false;
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (tris_of[g].empty() || !P.mPart[tris_of[g][0]])
            {
                continue;
            }
            const F64 a = std::max(area[g], 1e-9);
            const F64 mz = mean[g].z / a;
            const F64 deg = std::acos(std::min(1.0, mz)) * RAD_TO_DEG;
            const F64 planar = std::sqrt(mean[g].x * mean[g].x + mean[g].y * mean[g].y + mean[g].z * mean[g].z) / a;
            F64 x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30, z0 = 1e30, z1 = -1e30;
            bool named = false;
            for (const S32 t : tris_of[g])
            {
                named = named || (t < static_cast<S32>(cues.size()) && (cues[t] == CUE_STAIR || cues[t] == CUE_RAMP));
                for (S32 c = 0; c < 3; ++c)
                {
                    const V3& v = P.mV[P.mT[t * 3 + c]];
                    x0 = std::min(x0, v.x);
                    y0 = std::min(y0, v.y);
                    z0 = std::min(z0, v.z);
                    x1 = std::max(x1, v.x);
                    y1 = std::max(y1, v.y);
                    z1 = std::max(z1, v.z);
                }
            }
            slope_deg[g] = deg;
            own_rise[g] = z1 - z0;
            // a slope that climbs less than a step, unnamed, is a hull skirt or a prop's bevel
            const bool slope = kind[g] == SLOPE_GROUP && deg >= MIN_SLOPE_DEG && planar >= PLANAR && !(deg < FLOORISH_DEG && area[g] > FLOORISH_M2) &&
                               (own_rise[g] >= P.mClimb || named);
            // a tread is shallow: a deeper level group is a landing
            const bool tread = kind[g] == TREAD_GROUP && area[g] <= TREAD_MAX_M2 && std::min(x1 - x0, y1 - y0) <= MAX_GOING_M;
            cand[g] = slope || tread;
            if (kind[g] == SLOPE_GROUP)
            {
                has_dir[g] = uphill(P, tris_of[g], dir_x[g], dir_y[g]);
            }
            any = any || cand[g];
        }
        if (!any)
        {
            return;
        }

        std::vector<S32> root(ngroups);
        for (S32 g = 0; g < ngroups; ++g)
        {
            root[g] = g;
        }
        const auto find = [&](S32 g) {
            while (root[g] != g)
            {
                g = root[g] = root[root[g]];
            }
            return g;
        };
        const F64 cos_turn = std::cos(TURN_DEG * DEG_TO_RAD);
        // Each unit's run so far, area weighted: a tread or a landing between two flights joins neither across a turn.
        std::vector<F64> run_x(ngroups, 0.0), run_y(ngroups, 0.0);
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (has_dir[g])
            {
                run_x[g] = dir_x[g] * area[g];
                run_y[g] = dir_y[g] * area[g];
            }
        }
        const auto turns = [&](S32 a, S32 b) {
            const F64 la = std::hypot(run_x[a], run_y[a]), lb = std::hypot(run_x[b], run_y[b]);
            return la > 0 && lb > 0 && (run_x[a] * run_x[b] + run_y[a] * run_y[b]) < cos_turn * la * lb;
        };
        std::vector<S32> nearby;
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (!cand[g])
            {
                continue;
            }
            for (const S32 t : tris_of[g])
            {
                for (S32 c = 0; c < 3; ++c)
                {
                    const V3& v = P.mV[P.mT[t * 3 + c]];
                    trianglesNear(P, v.x, v.y, nearby);
                    for (const S32 u : nearby)
                    {
                        const S32 o = grp[u];
                        F64 z = 0;
                        if (o < 0 || o == g || !cand[o] || find(o) == find(g) || !nearZ(P, u, v.x, v.y, z) || std::fabs(z - v.z) > JOIN_DZ)
                        {
                            continue;
                        }
                        // a low or much gentler slope beside a steep one is not part of its flight
                        if (kind[g] == SLOPE_GROUP && kind[o] == SLOPE_GROUP &&
                            (std::fabs(slope_deg[g] - slope_deg[o]) > JOIN_SLOPE_DEG || std::min(own_rise[g], own_rise[o]) < FLIGHT_MIN_RISE_M))
                        {
                            continue;
                        }
                        if ((has_dir[g] && has_dir[o] && dir_x[g] * dir_x[o] + dir_y[g] * dir_y[o] < cos_turn) || turns(find(g), find(o)))
                        {
                            continue;
                        }
                        const S32 keep = std::min(find(g), find(o)), gone = std::max(find(g), find(o));
                        root[gone] = keep;
                        run_x[keep] += run_x[gone];
                        run_y[keep] += run_y[gone];
                    }
                }
            }
        }
        std::map<S32, std::vector<S32>> units;
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (cand[g])
            {
                units[find(g)].push_back(g);
            }
        }

        // The deck each floor group belongs to: floor groups joined where one steps level onto the next.
        std::vector<S32> deck(ngroups);
        for (S32 g = 0; g < ngroups; ++g)
        {
            deck[g] = g;
        }
        const auto deck_of = [&](S32 g) {
            while (deck[g] != g)
            {
                g = deck[g] = deck[deck[g]];
            }
            return g;
        };
        for (const auto& [a, b] : level_links)
        {
            if (a >= 0 && b >= 0 && !cand[a] && !cand[b] && deck_of(a) != deck_of(b))
            {
                const S32 ra = deck_of(a), rb = deck_of(b);
                deck[std::max(ra, rb)] = std::min(ra, rb);
            }
        }
        std::vector<F64> deck_area(ngroups, 0.0);
        for (S32 g = 0; g < ngroups; ++g)
        {
            deck_area[deck_of(g)] += area[g];
        }

        std::vector<S32> beyond;
        for (const auto& [r, members] : units)
        {
            std::vector<S32> all, sloped;
            F64 unit_area = 0;
            U8 treads = 0;
            std::vector<F64> tread_z;
            for (const S32 g : members)
            {
                all.insert(all.end(), tris_of[g].begin(), tris_of[g].end());
                unit_area += area[g];
                if (kind[g] == SLOPE_GROUP)
                {
                    sloped.insert(sloped.end(), tris_of[g].begin(), tris_of[g].end());
                }
                else
                {
                    treads = static_cast<U8>(std::min(255, treads + 1));
                    F64 z = 0;
                    for (const S32 t : tris_of[g])
                    {
                        z += P.mC[t].z * P.mArea[t];
                    }
                    tread_z.push_back(z / std::max(area[g], 1e-9));
                }
            }
            F64 ux = 0, uy = 0;
            if (!(sloped.empty() ? risesAlong(P, members, tris_of, ux, uy) : uphill(P, sloped, ux, uy)))
            {
                continue;
            }
            const F64 px = -uy, py = ux;
            const std::vector<S32>& body = sloped.empty() ? all : sloped;
            F64 s0 = 1e30, s1 = -1e30, l0 = 1e30, l1 = -1e30;
            for (const S32 t : body)
            {
                for (S32 c = 0; c < 3; ++c)
                {
                    const V3& v = P.mV[P.mT[t * 3 + c]];
                    const F64 s = v.x * ux + v.y * uy, l = v.x * px + v.y * py;
                    s0 = std::min(s0, s);
                    s1 = std::max(s1, s);
                    l0 = std::min(l0, l);
                    l1 = std::max(l1, l);
                }
            }
            if (s1 - s0 < 1e-3)
            {
                continue;
            }
            const F64 width = l1 - l0, lc = (l0 + l1) / 2;
            const F64 band = std::max(0.15, (s1 - s0) * 0.1);
            F64 zb = 1e30, zt = -1e30;
            for (const S32 t : body)
            {
                for (S32 c = 0; c < 3; ++c)
                {
                    const V3& v = P.mV[P.mT[t * 3 + c]];
                    const F64 s = v.x * ux + v.y * uy;
                    zb = s <= s0 + band ? std::min(zb, v.z) : zb;
                    zt = s >= s1 - band ? std::max(zt, v.z) : zt;
                }
            }
            // A flight sunk into a floor ends where its slope comes out of it: past
            // that the surface underfoot is the floor's, so the ends are the first
            // and last places along the centreline with no floor just above.
            {
                const S32 unit = r;
                const auto bodyZ = [&](F64 s, F64& z) {
                    const F64 x = s * ux + lc * px, y = s * uy + lc * py;
                    trianglesNear(P, x, y, nearby);
                    bool got = false, buried = false;
                    for (const S32 i : nearby)
                    {
                        F64 zi = 0;
                        if (grp[i] >= 0 && cand[grp[i]] && find(grp[i]) == unit && P.zAt(i, x, y, zi) && (!got || zi > z))
                        {
                            z = zi;
                            got = true;
                        }
                    }
                    for (const S32 i : nearby)
                    {
                        F64 zi = 0;
                        buried = buried || (got && grp[i] >= 0 && !cand[grp[i]] && P.zAt(i, x, y, zi) && zi - z > BURIED_DZ && zi - z <= FLOOR_UP);
                    }
                    return got && !buried;
                };
                F64 lo = s1 + 1.0, hi = s0 - 1.0, zlo = 0, zhi = 0;
                for (F64 s = s0; s <= s1; s += EXPOSED_STEP_M)
                {
                    F64 z = 0;
                    if (bodyZ(s, z))
                    {
                        lo = s;
                        zlo = z;
                        break;
                    }
                }
                for (F64 s = s1; s >= s0; s -= EXPOSED_STEP_M)
                {
                    F64 z = 0;
                    if (bodyZ(s, z))
                    {
                        hi = s;
                        zhi = z;
                        break;
                    }
                }
                if (hi - lo >= MIN_RUN_M && zhi > zlo)
                {
                    if (lo > s0 + EXPOSED_STEP_M)
                    {
                        s0 = lo;
                        zb = zlo;
                    }
                    if (hi < s1 - EXPOSED_STEP_M)
                    {
                        s1 = hi;
                        zt = zhi;
                    }
                }
            }

            F64 by_cue[3] = { 0, 0, 0 };
            std::vector<U8> member_codes;
            std::vector<LLVector2> plan;
            for (const S32 t : all)
            {
                by_cue[t < static_cast<S32>(cues.size()) && cues[t] < 3 ? cues[t] : CUE_NONE] += P.mArea[t];
                if (std::find(member_codes.begin(), member_codes.end(), codes[t]) == member_codes.end())
                {
                    member_codes.push_back(codes[t]);
                }
                for (S32 c = 0; c < 3; ++c)
                {
                    const V3& v = P.mV[P.mT[t * 3 + c]];
                    plan.emplace_back(static_cast<F32>(v.x), static_cast<F32>(v.y));
                }
            }
            // the cue with most area, none included
            U8 cue = CUE_NONE;
            for (const U8 c : { CUE_RAMP, CUE_STAIR })
            {
                cue = by_cue[c] > by_cue[cue] ? c : cue;
            }

            // The floor just past an end, across three lateral samples: the one most of them find.
            const auto floor_past = [&](F64 s, F64 z, F64 sign, S32& floor, F64& floor_z) {
                std::map<S32, std::pair<S32, F64>> found;
                for (const F64 f : { -0.25, 0.0, 0.25 })
                {
                    const F64 l = lc + f * width;
                    const F64 x = (s + sign * PAST_M) * ux + l * px, y = (s + sign * PAST_M) * uy + l * py;
                    const F64 x2 = (s + sign * (PAST_M + BEYOND_M)) * ux + l * px, y2 = (s + sign * (PAST_M + BEYOND_M)) * uy + l * py;
                    trianglesNear(P, x, y, nearby);
                    trianglesNear(P, x2, y2, beyond);
                    // each sample counts the one floor nearest the end's height
                    S32 pick = -1;
                    F64 pick_z = 0;
                    for (const S32 i : nearby)
                    {
                        F64 zi = 0;
                        const S32 k = grp[i];
                        if (k < 0 || cand[k] || !nearZ(P, i, x, y, zi))
                        {
                            continue;
                        }
                        const F64 into = -(zi - z) * sign;
                        if (std::fabs(zi - z) > FLOOR_DZ && !(into >= -FLOOR_DZ && into <= (cue != CUE_NONE ? FLOOR_UP : 0.0)))
                        {
                            continue;
                        }
                        const bool carries = std::any_of(beyond.begin(), beyond.end(), [&](S32 j) {
                            F64 zj = 0;
                            return grp[j] >= 0 && !cand[grp[j]] && nearZ(P, j, x2, y2, zj) && std::fabs(zj - zi) <= LEVEL_DZ;
                        });
                        if (carries && (pick < 0 || std::fabs(zi - z) < std::fabs(pick_z - z)))
                        {
                            pick = k;
                            pick_z = zi;
                        }
                    }
                    if (pick >= 0)
                    {
                        ++found[pick].first;
                        found[pick].second += pick_z;
                    }
                }
                floor = -1;
                S32 most = 0;
                for (const auto& [k, hits] : found)
                {
                    const F64 fz = hits.second / hits.first;
                    if (hits.first > most || (hits.first == most && std::fabs(fz - z) < std::fabs(floor_z - z)))
                    {
                        most = hits.first;
                        floor = k;
                        floor_z = fz;
                    }
                }
                return floor >= 0;
            };
            S32 fb = -1, ft = -1;
            F64 fbz = 0, ftz = 0;
            if (!floor_past(s0, zb, -1.0, fb, fbz) ||!floor_past(s1, zt, 1.0, ft, ftz) || fb == ft)
            {
                continue;
            }

            std::sort(tread_z.begin(), tread_z.end());
            std::vector<F64> gaps;
            for (size_t i = 1; i < tread_z.size(); ++i)
            {
                gaps.push_back(tread_z[i] - tread_z[i - 1]);
            }
            std::sort(gaps.begin(), gaps.end());
            const F64 riser = gaps.empty() ? 1e9 : gaps[gaps.size() / 2];
            const F64 going = (s1 - s0) / std::max<F64>(treads, 1);
            const bool stepped = treads >= 2 && riser <= P.mClimb && going <= MAX_GOING_M;

            const F64 rise = ftz - fbz;
            // clip each end to where the run reaches its floor's height
            F64 sb = s0, st = s1;
            if (zt > zb)
            {
                const F64 per = (s1 - s0) / (zt - zb);
                sb = s0 + std::max(0.0, fbz - zb) * per;
                st = s1 - std::max(0.0, zt - ftz) * per;
            }
            const F64 run = st - sb;
            const F64 grade = std::atan2(rise, std::max(run, 1e-6)) * RAD_TO_DEG;
            Flight fl;
            if (cue != CUE_NONE)
            {
                fl.mKind = cue == CUE_STAIR ? ConnectorKind::STAIR : ConnectorKind::RAMP;
                fl.mRule = ConnectorRule::NAME;
            }
            else if (stepped)
            {
                fl.mKind = ConnectorKind::STAIR;
                fl.mRule = ConnectorRule::TREADS;
            }
            else
            {
                fl.mKind = grade > STAIR_GRADE_DEG ? ConnectorKind::STAIR : ConnectorKind::RAMP;
                fl.mRule = ConnectorRule::GRADE;
            }
            const bool cued = fl.mRule != ConnectorRule::GRADE;
            // A floor outside the partitioned area is ground, never a prop top; inside, it is the whole deck.
            const auto floor_area = [&](S32 f) { return P.mPart[tris_of[f][0]] ? deck_area[deck_of(f)] : 1e30; };
            if (cue == CUE_NONE && (floor_area(fb) < FLOOR_MIN_M2 || floor_area(ft) < FLOOR_MIN_M2))
            {
                continue;
            }
            if (rise < FLIGHT_MIN_RISE_M || (rise < MIN_RISE_UNCUED_M && !cued) || run < MIN_RUN_M || (grade > max_grade_deg && !stepped) ||
                width < MIN_WIDTH_M || s1 - s0 > MAX_RUN_M || (!cued && (unit_area < MIN_AREA_M2 || unit_area > MAX_AREA_UNCUED_M2)) ||
                (sloped.empty() && treads >= 2 && !stepped && cue == CUE_NONE))
            {
                continue;
            }
            const auto at = [&](F64 s, F64 l, F64 z) { return LLVector3(static_cast<F32>(s * ux + l * px), static_cast<F32>(s * uy + l * py), static_cast<F32>(z)); };
            fl.mBottom = at(sb, lc, fbz);
            fl.mTop = at(st, lc, ftz);
            fl.mBottomA = at(sb, l0, fbz);
            fl.mBottomB = at(sb, l1, fbz);
            fl.mTopA = at(st, l0, ftz);
            fl.mTopB = at(st, l1, ftz);
            fl.mRise = static_cast<F32>(rise);
            fl.mRun = static_cast<F32>(run);
            const F64 slope = rise / std::max(run, 1e-6);
            fl.mPlane[0] = static_cast<F32>(slope * ux);
            fl.mPlane[1] = static_cast<F32>(slope * uy);
            fl.mPlane[2] = static_cast<F32>(fbz - slope * sb);
            fl.mZMin = static_cast<F32>(fbz);
            fl.mZMax = static_cast<F32>(ftz);
            fl.mFootprint = hull(plan);
            fl.mTreads = treads;
            fl.mCodes = member_codes;
            U64 key = 1469598103934665603ull;
            for (const F32 v : { fl.mBottom.mV[VX], fl.mBottom.mV[VY], fl.mBottom.mV[VZ], fl.mTop.mV[VX], fl.mTop.mV[VY], fl.mTop.mV[VZ] })
            {
                key = (key ^ static_cast<U64>(std::llround(v * 4.0))) * 1099511628211ull;
            }
            fl.mKey = key;
            flights.push_back(std::move(fl));
        }
    }
}

namespace sknav
{
    SlopePartition partitionSlopes(const std::vector<LLVector3>& verts, const std::vector<S32>& tris, const std::vector<U8>& areas,
                                   const std::vector<U8>& two_sided, U8 partitioned_area, F32 climb, F32 walk_slope_deg, S32 max_codes,
                                   const std::vector<U8>& cues)
    {
        SlopePartition out;
        Partitioner P;
        P.mCosTurn = std::cos(SLOPE_TURN_DEG * DEG_TO_RAD);
        P.mClimb = climb;
        P.build(verts, tris, areas, two_sided, partitioned_area);
        const S32 n = P.ntris();

        // 1. groups: flood over shared edges within the crease angle
        P.eachEdge([&](U64, size_t s, size_t e) {
            for (size_t a = s; a < e; ++a)
            {
                for (size_t b = a + 1; b < e; ++b)
                {
                    const S32 t = P.mEdges[a].second, u = P.mEdges[b].second;
                    if (P.mWalk[t] && P.mWalk[u] && P.sameSlope(t, u))
                    {
                        P.unite(t, u);
                    }
                }
            }
        });

        // coplanar neighbours that share no vertices (separate prims) join too
        std::vector<S32> g_of = P.groupsOf();
        std::vector<Boundary> edges;
        P.boundaries(g_of, edges);
        for (const Boundary& e : edges)
        {
            S32 u = -1;
            F64 dz = 0;
            if (P.probe(e, g_of, u, dz) && std::fabs(dz) < SEAM_DZ && P.sameSlope(e.mTri, u))
            {
                P.unite(e.mTri, u);
            }
        }
        g_of = P.groupsOf();

        // dense group ids
        std::vector<S32> dense(n, -1);
        S32 ngroups = 0;
        for (S32 t = 0; t < n; ++t)
        {
            if (g_of[t] >= 0 && dense[g_of[t]] < 0)
            {
                dense[g_of[t]] = ngroups++;
            }
        }
        std::vector<S32> grp(n, -1);
        std::vector<S32> first(ngroups, -1);
        for (S32 t = 0; t < n; ++t)
        {
            if (g_of[t] >= 0)
            {
                grp[t] = dense[g_of[t]];
                if (first[grp[t]] < 0)
                {
                    first[grp[t]] = t;
                }
            }
        }

        // risers: the probe again, across the boundaries of the final groups
        std::vector<std::vector<Riser>> risers(ngroups);
        std::vector<std::pair<S32, S32>> touching, level_links;
        P.boundaries(g_of, edges);
        for (const Boundary& e : edges)
        {
            S32 u = -1;
            F64 dz = 0;
            if (!P.probe(e, g_of, u, dz))
            {
                continue;
            }
            touching.emplace_back(grp[e.mTri], grp[u]);
            if (std::fabs(dz) <= LEVEL_DZ)
            {
                level_links.emplace_back(grp[e.mTri], grp[u]);
            }
            if (std::fabs(dz) >= SEAM_DZ)
            {
                // Seen from both sides: a stair of overlapping prims has each
                // step's riser standing on the step below, inside its top
                // rather than on an edge of it, so only the step above finds it.
                risers[grp[e.mTri]].push_back({ grp[u], dz, e.mOx, e.mOy, e.mA, e.mB });
                risers[grp[u]].push_back({ grp[e.mTri], -dz, -e.mOx, -e.mOy, e.mA, e.mB });
            }
        }

        // 2. level groups are treads or landings
        enum Kind : U8 { SLOPE = SLOPE_GROUP, LANDING = LANDING_GROUP, TREAD = TREAD_GROUP };
        std::vector<U8> kind(ngroups, SLOPE);
        for (S32 g = 0; g < ngroups; ++g)
        {
            const S32 t0 = first[g];
            if (!P.mLevel[t0])
            {
                ++out.mSlopes;
                continue;
            }
            kind[g] = LANDING;
            std::vector<const Riser*> ups, downs;
            for (const Riser& r : risers[g])
            {
                (r.mDz > 0 ? ups : downs).push_back(&r);
            }
            if (!P.mPart[t0] || ups.size() * downs.size() > MAX_PAIRS)
            {
                ++out.mLandings;
                continue;
            }
            F64 best = -1;
            for (const Riser* u : ups)
            {
                for (const Riser* d : downs)
                {
                    if (u->mOx * d->mOx + u->mOy * d->mOy > OPPOSITE)
                    {
                        continue;
                    }
                    // the going between the two risers at their midpoints: a
                    // spiral wedge's rim chord is longer than the tread it walks
                    F64 ax = u->mOx - d->mOx, ay = u->mOy - d->mOy;
                    const F64 al = std::sqrt(ax * ax + ay * ay);
                    ax /= al;
                    ay /= al;
                    const V3 &ua = P.mV[u->mA], &ub = P.mV[u->mB], &da = P.mV[d->mA], &db = P.mV[d->mB];
                    const F64 depth = std::fabs(((ua.x + ub.x) / 2 - (da.x + db.x) / 2) * ax + ((ua.y + ub.y) / 2 - (da.y + db.y) / 2) * ay);
                    if (depth < SLOPE_REACH_M && (best < 0 || depth < best))
                    {
                        best = depth;
                    }
                }
            }
            if (best >= 0)
            {
                kind[g] = TREAD;
                ++out.mTreads;
            }
            else
            {
                ++out.mLandings;
            }
        }

        // flights: treads joined through their risers to other treads
        std::vector<S32> fp(ngroups);
        for (S32 g = 0; g < ngroups; ++g)
        {
            fp[g] = g;
        }
        const auto ff = [&](S32 g) {
            while (fp[g] != g)
            {
                g = fp[g] = fp[fp[g]];
            }
            return g;
        };
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (kind[g] != TREAD)
            {
                continue;
            }
            for (const Riser& r : risers[g])
            {
                if (kind[r.mOther] == TREAD)
                {
                    const S32 a = ff(g), b = ff(r.mOther);
                    if (a != b)
                    {
                        fp[std::max(a, b)] = std::min(a, b);
                    }
                }
            }
        }

        // 3. regions, coloured so that no two touching share a code
        std::vector<S32> region_of_group(ngroups);
        for (S32 g = 0; g < ngroups; ++g)
        {
            region_of_group[g] = kind[g] == TREAD ? ff(g) : g;
        }
        out.mRegion.assign(n, -1);
        out.mGroup = grp;
        out.mKind = kind;
        for (S32 t = 0; t < n; ++t)
        {
            if (grp[t] >= 0)
            {
                out.mRegion[t] = region_of_group[grp[t]];
            }
        }
        // Two landings may share a code: Recast needs no cut along a curb.
        std::vector<std::set<S32>> adj(ngroups);
        const auto link = [&](S32 a, S32 b) {
            if (kind[a] == LANDING && kind[b] == LANDING)
            {
                return;
            }
            a = region_of_group[a];
            b = region_of_group[b];
            if (a != b)
            {
                adj[a].insert(b);
                adj[b].insert(a);
            }
        };
        for (const auto& [a, b] : touching)
        {
            link(a, b);
        }
        P.eachEdge([&](U64, size_t s, size_t e) {
            for (size_t a = s; a < e; ++a)
            {
                for (size_t b = a + 1; b < e; ++b)
                {
                    const S32 t = P.mEdges[a].second, u = P.mEdges[b].second;
                    if (grp[t] >= 0 && grp[u] >= 0)
                    {
                        link(grp[t], grp[u]);
                    }
                }
            }
        });
        std::vector<S32> colour(ngroups, -1);
        for (S32 r = 0; r < ngroups; ++r)
        {
            if (region_of_group[r] != r || !P.mPart[first[r]])
            {
                continue;
            }
            std::set<S32> used;
            for (const S32 q : adj[r])
            {
                used.insert(colour[q]);
            }
            S32 c = 0;
            while (used.count(c))
            {
                ++c;
            }
            colour[r] = c;
        }

        // codes by (area, colour), ascending, so the code order is the area order
        std::set<std::pair<U8, S32>> keys;
        for (S32 t = 0; t < n; ++t)
        {
            if (grp[t] >= 0)
            {
                keys.insert({ areas[t], P.mPart[t] ? colour[out.mRegion[t]] : 0 });
            }
            else if (areas[t] && !P.mValid[t])
            {
                keys.insert({ areas[t], 0 });
            }
        }
        // Each area's first code is reserved before any colour gets one, so
        // running out only ever shares a code within an area.
        out.mReal.assign(1, 0);
        std::map<std::pair<U8, S32>, U8> code_of;
        std::map<U8, U8> last_of;
        std::set<U8> pending;
        for (const auto& k : keys)
        {
            pending.insert(k.first);
        }
        for (const auto& k : keys)
        {
            const bool first_of_area = pending.erase(k.first) > 0;
            if (first_of_area || static_cast<S32>(out.mReal.size() + pending.size()) < max_codes)
            {
                last_of[k.first] = static_cast<U8>(out.mReal.size());
                out.mReal.push_back(k.first);
            }
            code_of[k] = last_of[k.first];
        }
        out.mCodes.assign(n, 0);
        for (S32 t = 0; t < n; ++t)
        {
            if (grp[t] >= 0)
            {
                out.mCodes[t] = code_of[{ areas[t], P.mPart[t] ? colour[out.mRegion[t]] : 0 }];
            }
            else if (areas[t] && !P.mValid[t])
            {
                // a needle the weld collapsed still rasterises, as its own area
                out.mCodes[t] = code_of[{ areas[t], 0 }];
            }
        }

        // 4. connectors: slope and tread groups that step off onto a floor at each end
        buildConnectors(P, grp, kind, ngroups, out.mCodes, cues, level_links, walk_slope_deg, out.mFlights);
        return out;
    }

    U8 nameCue(const std::string& name, const std::string& description)
    {
        std::string text = name + " " + description;
        LLStringUtil::toLower(text);
        if (text.find("stair") != std::string::npos || text.find("step") != std::string::npos)
        {
            return CUE_STAIR;
        }
        return text.find("ramp") != std::string::npos ? CUE_RAMP : CUE_NONE;
    }

    void mapFlightPolys(std::vector<Flight>& flights, const std::vector<LLVector3>& centroids, const std::vector<U8>& codes, F32 climb)
    {
        for (Flight& f : flights)
        {
            f.mPolys.clear();
            for (size_t p = 0; p < centroids.size() && p < 0xffff; ++p)
            {
                const LLVector3& c = centroids[p];
                if (std::find(f.mCodes.begin(), f.mCodes.end(), codes[p]) != f.mCodes.end() && c.mV[VZ] >= f.mZMin - climb && c.mV[VZ] <= f.mZMax + climb &&
                    outside(f.mFootprint, LLVector2(c.mV[VX], c.mV[VY])) < 0.25f)
                {
                    f.mPolys.push_back(static_cast<U16>(p));
                }
            }
        }
        flights.erase(std::remove_if(flights.begin(), flights.end(), [](const Flight& f) { return f.mPolys.empty(); }), flights.end());
    }

    void fitFlightEdges(std::vector<Flight>& flights, const std::vector<std::vector<LLVector3>>& polys, F32 radius, const LLVector2& lo,
                        const LLVector2& hi)
    {
        const auto on_edge = [&](const LLVector3& v) {
            return v.mV[VX] - lo.mV[VX] < 0.05f || hi.mV[VX] - v.mV[VX] < 0.05f || v.mV[VY] - lo.mV[VY] < 0.05f || hi.mV[VY] - v.mV[VY] < 0.05f;
        };
        for (Flight& f : flights)
        {
            LLVector2 u(f.mTop.mV[VX] - f.mBottom.mV[VX], f.mTop.mV[VY] - f.mBottom.mV[VY]);
            const F32 run = u.normVec();
            if (run < 1e-3f || f.mPolys.empty())
            {
                continue;
            }
            const LLVector2 p(-u.mV[VY], u.mV[VX]), base(f.mBottom.mV[VX], f.mBottom.mV[VY]);
            F32 k0 = 1e30f, k1 = -1e30f, s0 = 1e30f, s1 = -1e30f;
            bool cut0 = false, cut1 = false;
            for (const U16 i : f.mPolys)
            {
                for (const LLVector3& v : polys[i])
                {
                    const LLVector2 d(v.mV[VX] - base.mV[VX], v.mV[VY] - base.mV[VY]);
                    if (d * p < k0)
                    {
                        k0 = d * p;
                        cut0 = on_edge(v);
                    }
                    if (d * p > k1)
                    {
                        k1 = d * p;
                        cut1 = on_edge(v);
                    }
                    s0 = llmin(s0, d * u);
                    s1 = llmax(s1, d * u);
                }
            }
            if (k1 < k0 || llmin(s1, run) - llmax(s0, 0.f) < 0.5f * run)
            {
                continue;
            }
            const auto lateral = [&](const LLVector3& v) { return (LLVector2(v.mV[VX], v.mV[VY]) - base) * p; };
            // A side the tile cuts off may carry on in the next tile.
            const F32 a0 = llmin(lateral(f.mBottomA), lateral(f.mBottomB)), b0 = llmax(lateral(f.mBottomA), lateral(f.mBottomB));
            const F32 a = cut0 ? a0 : llmax(a0, k0 - radius);
            const F32 b = cut1 ? b0 : llmin(b0, k1 + radius);
            if (b - a < 2.f * radius)
            {
                continue;
            }
            // Ends keep their place along the run and their height; only their span across it narrows.
            const auto place = [&](const LLVector3& end, F32 l) {
                const LLVector2 at = base + u * ((LLVector2(end.mV[VX], end.mV[VY]) - base) * u) + p * l;
                return LLVector3(at.mV[VX], at.mV[VY], end.mV[VZ]);
            };
            const bool flip = lateral(f.mBottomA) > lateral(f.mBottomB);
            f.mBottomA = place(f.mBottomA, flip ? b : a);
            f.mBottomB = place(f.mBottomB, flip ? a : b);
            const bool flip_top = lateral(f.mTopA) > lateral(f.mTopB);
            f.mTopA = place(f.mTopA, flip_top ? b : a);
            f.mTopB = place(f.mTopB, flip_top ? a : b);
        }
    }

    bool flightSurvives(const rcCompactHeightfield& chf, S32 border, const Flight& flight, F32 climb)
    {
        for (S32 z = border; z < chf.height - border; ++z)
        {
            for (S32 x = border; x < chf.width - border; ++x)
            {
                const rcCompactCell& cell = chf.cells[x + z * chf.width];
                for (U32 i = cell.index, n = cell.index + cell.count; i < n; ++i)
                {
                    if (std::find(flight.mCodes.begin(), flight.mCodes.end(), chf.areas[i]) == flight.mCodes.end())
                    {
                        continue;
                    }
                    // Recast x is SL y, Recast z SL x.
                    const LLVector2 at(chf.bmin[2] + (z + 0.5f) * chf.cs, chf.bmin[0] + (x + 0.5f) * chf.cs);
                    const F32 top = chf.bmin[1] + chf.spans[i].y * chf.ch;
                    if (top >= flight.mZMin - climb && top <= flight.mZMax + climb && outside(flight.mFootprint, at) < 0.25f)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void markFlights(AirGrid& grid, const std::vector<Flight>& flights)
    {
        for (const Flight& f : flights)
        {
            if (f.mFootprint.size() < 3)
            {
                continue;
            }
            F32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (const LLVector2& p : f.mFootprint)
            {
                x0 = std::min(x0, p.mV[VX]);
                y0 = std::min(y0, p.mV[VY]);
                x1 = std::max(x1, p.mV[VX]);
                y1 = std::max(y1, p.mV[VY]);
            }
            const S32 ix0 = std::max(0, static_cast<S32>(std::floor((x0 - grid.mOrigin.mV[VX]) / grid.mCell)));
            const S32 iy0 = std::max(0, static_cast<S32>(std::floor((y0 - grid.mOrigin.mV[VY]) / grid.mCell)));
            const S32 ix1 = std::min(grid.mWidth - 1, static_cast<S32>(std::floor((x1 - grid.mOrigin.mV[VX]) / grid.mCell)));
            const S32 iy1 = std::min(grid.mHeight - 1, static_cast<S32>(std::floor((y1 - grid.mOrigin.mV[VY]) / grid.mCell)));
            const size_t n = f.mFootprint.size();
            for (S32 iy = iy0; iy <= iy1; ++iy)
            {
                for (S32 ix = ix0; ix <= ix1; ++ix)
                {
                    const LLVector2 p(grid.mOrigin.mV[VX] + (ix + 0.5f) * grid.mCell, grid.mOrigin.mV[VY] + (iy + 0.5f) * grid.mCell);
                    bool inside = true;
                    for (size_t k = 0; k < n && inside; ++k)
                    {
                        const LLVector2 edge = f.mFootprint[(k + 1) % n] - f.mFootprint[k], to = p - f.mFootprint[k];
                        inside = edge.mV[VX] * to.mV[VY] - edge.mV[VY] * to.mV[VX] >= 0.f;
                    }
                    if (!inside)
                    {
                        continue;
                    }
                    const F32 plane = f.mPlane[0] * p.mV[VX] + f.mPlane[1] * p.mV[VY] + f.mPlane[2];
                    const U32 base = grid.mStart[ix + iy * grid.mWidth];
                    for (U32 i = 0; i < grid.count(ix, iy); ++i)
                    {
                        AirGap& g = grid.mGaps[base + i];
                        const F32 z = grid.height(g.mFloor);
                        // A tread stands off the fitted plane by up to half a riser.
                        if ((g.mFlags & AirGap::TOP) && z >= f.mZMin - 0.25f && z <= f.mZMax + 0.25f &&
                            std::fabs(z - plane) <= std::max(0.5f, f.mRise))
                        {
                            g.mFlags |= AirGap::FLIGHT;
                        }
                    }
                }
            }
        }
    }
}
