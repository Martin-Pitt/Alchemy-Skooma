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
    constexpr F64 GRID_M = 1.0;         ///< the probe index's cell
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
            mGw = static_cast<S32>((x1 - x0) / GRID_M) + 1;
            mGh = static_cast<S32>((y1 - y0) / GRID_M) + 1;
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
                const S32 cx0 = static_cast<S32>((tx0 - mGx0) / GRID_M), cx1 = static_cast<S32>((tx1 - mGx0) / GRID_M);
                const S32 cy0 = static_cast<S32>((ty0 - mGy0) / GRID_M), cy1 = static_cast<S32>((ty1 - mGy0) / GRID_M);
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
            const S32 cx = static_cast<S32>(std::floor((px - mGx0) / GRID_M)), cy = static_cast<S32>(std::floor((py - mGy0) / GRID_M));
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

    constexpr F64 STEP_DZ = 0.5;            ///< a connector steps onto a floor or into another of its groups within this
    constexpr F64 NEAR_XY = 0.15;           ///< this close in plan counts as touching
    constexpr F64 SAMPLE_M = 0.25;          ///< along a group's boundary
    constexpr F64 TREAD_MAX_M2 = 6.0;       ///< a level group bigger than this is a floor, whatever its risers say
    constexpr F64 MIN_SLOPE_DEG = 5.0;
    constexpr F64 FLOORISH_DEG = 10.0;      ///< a slope this gentle and FLOORISH_M2 big is a floor
    constexpr F64 FLOORISH_M2 = 20.0;
    constexpr F64 BEYOND_M = 0.6;           ///< the floor at an end carries on this far, or it is a rail or a wall top
    constexpr F64 LINK_XY = 1.5;            ///< contacts this close in plan and LEVEL_DZ in height are one end
    constexpr F64 LEVEL_DZ = 0.3;
    constexpr F64 MIN_END_SPAN_M = 0.4;     ///< a narrower end is a corner touching, not a way on
    constexpr F64 MAX_GRADE_DEG = 55.0;     ///< steeper is a wall, a ladder or furniture
    constexpr F64 MIN_RUN_M = 0.5;
    constexpr F64 MIN_RISE_UNCUED_M = 1.0;  ///< without a name or treads, a smaller climb is a step
    constexpr F64 DUP_M = 3.0;              ///< two connectors with both ends this close are one

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
        const S32 cx0 = static_cast<S32>(std::floor((px - NEAR_XY - P.mGx0) / GRID_M));
        const S32 cx1 = static_cast<S32>(std::floor((px + NEAR_XY - P.mGx0) / GRID_M));
        const S32 cy0 = static_cast<S32>(std::floor((py - NEAR_XY - P.mGy0) / GRID_M));
        const S32 cy1 = static_cast<S32>(std::floor((py + NEAR_XY - P.mGy0) / GRID_M));
        for (S32 cy = std::max(cy0, 0); cy <= std::min(cy1, P.mGh - 1); ++cy)
        {
            for (S32 cx = std::max(cx0, 0); cx <= std::min(cx1, P.mGw - 1); ++cx)
            {
                for (const S32 t : P.mGrid[static_cast<size_t>(cy) * P.mGw + cx])
                {
                    if (std::find(out.begin(), out.end(), t) == out.end())
                    {
                        out.push_back(t);
                    }
                }
            }
        }
    }

    struct Contact
    {
        F64 x, y, z;
    };

    struct End
    {
        std::vector<Contact> mPts;
        F64 mX = 0, mY = 0, mZMean = 0;
        F64 mZ = 0;         ///< median
        F64 mSpan = 0;      ///< plan diagonal of its contacts
    };

    /// Contacts linked within LINK_XY in plan and LEVEL_DZ in height, as ends.
    std::vector<End> clusterEnds(const std::vector<Contact>& pts)
    {
        const size_t n = pts.size();
        std::vector<size_t> par(n);
        for (size_t i = 0; i < n; ++i)
        {
            par[i] = i;
        }
        const auto f = [&](size_t i) {
            while (par[i] != i)
            {
                i = par[i] = par[par[i]];
            }
            return i;
        };
        std::map<std::pair<S64, S64>, std::vector<size_t>> cells;
        for (size_t i = 0; i < n; ++i)
        {
            cells[{ static_cast<S64>(std::floor(pts[i].x / LINK_XY)), static_cast<S64>(std::floor(pts[i].y / LINK_XY)) }].push_back(i);
        }
        for (const auto& [c, idx] : cells)
        {
            for (S64 dx = -1; dx <= 1; ++dx)
            {
                for (S64 dy = -1; dy <= 1; ++dy)
                {
                    const auto it = cells.find({ c.first + dx, c.second + dy });
                    if (it == cells.end())
                    {
                        continue;
                    }
                    for (const size_t j : it->second)
                    {
                        for (const size_t i : idx)
                        {
                            if (i < j && std::fabs(pts[i].z - pts[j].z) <= LEVEL_DZ &&
                                std::hypot(pts[i].x - pts[j].x, pts[i].y - pts[j].y) <= LINK_XY)
                            {
                                par[f(i)] = f(j);
                            }
                        }
                    }
                }
            }
        }
        std::map<size_t, std::vector<Contact>> groups;
        for (size_t i = 0; i < n; ++i)
        {
            groups[f(i)].push_back(pts[i]);
        }
        std::vector<End> out;
        for (auto& [r, ps] : groups)
        {
            End e;
            F64 x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            std::vector<F64> zs;
            for (const Contact& p : ps)
            {
                e.mX += p.x;
                e.mY += p.y;
                e.mZMean += p.z;
                x0 = std::min(x0, p.x);
                y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x);
                y1 = std::max(y1, p.y);
                zs.push_back(p.z);
            }
            const F64 count = static_cast<F64>(ps.size());
            e.mX /= count;
            e.mY /= count;
            e.mZMean /= count;
            std::sort(zs.begin(), zs.end());
            e.mZ = zs[zs.size() / 2];
            e.mSpan = std::hypot(x1 - x0, y1 - y0);
            e.mPts = std::move(ps);
            out.push_back(std::move(e));
        }
        return out;
    }

    /// An end's extent across the run (px, py), at its median height.
    void endExtent(const End& e, F64 px, F64 py, LLVector3& a, LLVector3& b)
    {
        F64 lo = 1e30, hi = -1e30;
        for (const Contact& p : e.mPts)
        {
            const F64 d = (p.x - e.mX) * px + (p.y - e.mY) * py;
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        a.set(static_cast<F32>(e.mX + lo * px), static_cast<F32>(e.mY + lo * py), static_cast<F32>(e.mZ));
        b.set(static_cast<F32>(e.mX + hi * px), static_cast<F32>(e.mY + hi * py), static_cast<F32>(e.mZ));
    }

    /// Stairs and ramps: slope groups, and tread groups too small to be a
    /// floor, joined where they step into each other. Each end is where such
    /// a set steps off onto a floor that carries on, and pairs with the
    /// nearest end on the next level up.
    void buildConnectors(Partitioner& P, const std::vector<S32>& grp, const std::vector<U8>& kind, S32 ngroups, const std::vector<U8>& codes,
                         const std::vector<U8>& cues, std::vector<Flight>& flights)
    {
        const S32 n = P.ntris();
        std::vector<F64> area(ngroups, 0.0), nz(ngroups, 0.0);
        std::vector<S32> first(ngroups, -1);
        for (S32 t = 0; t < n; ++t)
        {
            if (grp[t] >= 0)
            {
                area[grp[t]] += P.mArea[t];
                nz[grp[t]] += P.mN[t].z * P.mArea[t];
                first[grp[t]] = first[grp[t]] < 0 ? t : first[grp[t]];
            }
        }
        std::vector<U8> cand(ngroups, 0);
        bool any = false;
        for (S32 g = 0; g < ngroups; ++g)
        {
            const F64 deg = std::acos(std::min(1.0, nz[g] / std::max(area[g], 1e-9))) * RAD_TO_DEG;
            const bool slope = kind[g] == SLOPE_GROUP && deg >= MIN_SLOPE_DEG && !(deg < FLOORISH_DEG && area[g] > FLOORISH_M2);
            const bool tread = kind[g] == TREAD_GROUP && area[g] <= TREAD_MAX_M2;
            cand[g] = first[g] >= 0 && P.mPart[first[g]] && (slope || tread);
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
        std::vector<std::vector<Contact>> contacts(ngroups);
        std::vector<Boundary> edges;
        P.boundaries(grp, edges);
        std::vector<S32> nearby, beyond, seen;
        for (const Boundary& e : edges)
        {
            const S32 g = grp[e.mTri];
            if (g < 0 || !cand[g])
            {
                continue;
            }
            const V3 &a = P.mV[e.mA], &b = P.mV[e.mB];
            const F64 len = std::hypot(b.x - a.x, b.y - a.y);
            const S32 k = std::max(1, static_cast<S32>(len / SAMPLE_M));
            for (S32 j = 0; j <= k; ++j)
            {
                const F64 s = static_cast<F64>(j) / k;
                const F64 px = a.x + s * (b.x - a.x), py = a.y + s * (b.y - a.y), pz = a.z + s * (b.z - a.z);
                trianglesNear(P, px, py, nearby);
                seen.clear();
                for (const S32 u : nearby)
                {
                    const S32 o = grp[u];
                    F64 z = 0;
                    if (o < 0 || o == g || std::find(seen.begin(), seen.end(), o) != seen.end() || !nearZ(P, u, px, py, z) ||
                        std::fabs(z - pz) > STEP_DZ)
                    {
                        continue;
                    }
                    if (cand[o])
                    {
                        const S32 ra = find(g), rb = find(o);
                        if (ra != rb)
                        {
                            root[std::max(ra, rb)] = std::min(ra, rb);
                        }
                    }
                    else
                    {
                        const F64 qx = px + BEYOND_M * e.mOx, qy = py + BEYOND_M * e.mOy;
                        trianglesNear(P, qx, qy, beyond);
                        bool floor = false;
                        for (const S32 w : beyond)
                        {
                            F64 zw = 0;
                            floor = floor || (grp[w] >= 0 && !cand[grp[w]] && nearZ(P, w, qx, qy, zw) && std::fabs(zw - z) <= LEVEL_DZ);
                        }
                        if (!floor)
                        {
                            continue;
                        }
                        contacts[g].push_back({ px, py, z });
                    }
                    seen.push_back(o);
                }
            }
        }

        std::map<S32, std::vector<S32>> comps;
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (cand[g])
            {
                comps[find(g)].push_back(g);
            }
        }
        std::vector<std::vector<S32>> tris_of(ngroups);
        for (S32 t = 0; t < n; ++t)
        {
            if (grp[t] >= 0 && cand[grp[t]])
            {
                tris_of[grp[t]].push_back(t);
            }
        }
        for (const auto& [r, members] : comps)
        {
            std::vector<Contact> pts;
            for (const S32 g : members)
            {
                pts.insert(pts.end(), contacts[g].begin(), contacts[g].end());
            }
            std::vector<End> ends = clusterEnds(pts);
            ends.erase(std::remove_if(ends.begin(), ends.end(), [](const End& e) { return e.mSpan < MIN_END_SPAN_M; }), ends.end());
            if (ends.size() < 2)
            {
                continue;
            }
            U8 treads = 0;
            F64 comp_area = 0;
            F64 by_cue[3] = { 0, 0, 0 };
            std::vector<LLVector2> plan;
            std::vector<U8> member_codes;
            for (const S32 g : members)
            {
                treads = static_cast<U8>(std::min(255, treads + (kind[g] == TREAD_GROUP ? 1 : 0)));
                for (const S32 t : tris_of[g])
                {
                    by_cue[t < static_cast<S32>(cues.size()) && cues[t] < 3 ? cues[t] : CUE_NONE] += P.mArea[t];
                    comp_area += P.mArea[t];
                    for (S32 c = 0; c < 3; ++c)
                    {
                        const V3& v = P.mV[P.mT[t * 3 + c]];
                        plan.emplace_back(static_cast<F32>(v.x), static_cast<F32>(v.y));
                    }
                    if (std::find(member_codes.begin(), member_codes.end(), codes[t]) == member_codes.end())
                    {
                        member_codes.push_back(codes[t]);
                    }
                }
            }
            // the cue with most area, none included
            U8 cue = CUE_NONE;
            for (const U8 c : { CUE_RAMP, CUE_STAIR })
            {
                cue = by_cue[c] > by_cue[cue] ? c : cue;
            }
            const std::vector<LLVector2> footprint = hull(plan);

            std::vector<std::pair<Flight, F64>> found;
            for (const End& e : ends)
            {
                F64 level = 1e30;
                for (const End& f : ends)
                {
                    if (f.mZ >= e.mZ + FLIGHT_MIN_RISE_M)
                    {
                        level = std::min(level, f.mZ);
                    }
                }
                const End* t = nullptr;
                F64 run = 1e30;
                for (const End& f : ends)
                {
                    const F64 d = std::hypot(f.mX - e.mX, f.mY - e.mY);
                    if (f.mZ >= e.mZ + FLIGHT_MIN_RISE_M && f.mZ <= level + LEVEL_DZ && d < run)
                    {
                        run = d;
                        t = &f;
                    }
                }
                if (!t)
                {
                    continue;
                }
                const F64 rise = t->mZ - e.mZ;
                // Ends that nearly meet in plan wind round: walk the surface's length instead.
                const F64 walk = run >= MIN_RUN_M ? run : std::max(run, comp_area / std::max(std::max(e.mSpan, t->mSpan), MIN_END_SPAN_M));
                const F64 grade = std::atan2(rise, std::max(walk, 1e-6)) * RAD_TO_DEG;
                Flight fl;
                if (cue != CUE_NONE)
                {
                    fl.mKind = cue == CUE_STAIR ? ConnectorKind::STAIR : ConnectorKind::RAMP;
                    fl.mRule = ConnectorRule::NAME;
                }
                else if (treads >= 2)
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
                if (grade > MAX_GRADE_DEG || walk < MIN_RUN_M || (rise < MIN_RISE_UNCUED_M && !cued))
                {
                    continue;
                }
                const F64 dx = run > 1e-3 ? (t->mX - e.mX) / run : 1.0, dy = run > 1e-3 ? (t->mY - e.mY) / run : 0.0;
                fl.mBottom.set(static_cast<F32>(e.mX), static_cast<F32>(e.mY), static_cast<F32>(e.mZMean));
                fl.mTop.set(static_cast<F32>(t->mX), static_cast<F32>(t->mY), static_cast<F32>(t->mZMean));
                endExtent(e, -dy, dx, fl.mBottomA, fl.mBottomB);
                endExtent(*t, -dy, dx, fl.mTopA, fl.mTopB);
                fl.mRise = static_cast<F32>(rise);
                fl.mRun = static_cast<F32>(walk);
                const F64 slope = run > 1e-3 ? rise / walk : 0.0;
                fl.mPlane[0] = static_cast<F32>(slope * dx);
                fl.mPlane[1] = static_cast<F32>(slope * dy);
                fl.mPlane[2] = static_cast<F32>(e.mZ - slope * (dx * e.mX + dy * e.mY));
                fl.mZMin = static_cast<F32>(e.mZ);
                fl.mZMax = static_cast<F32>(t->mZ);
                fl.mFootprint = footprint;
                fl.mTreads = treads;
                fl.mCodes = member_codes;
                U64 key = 1469598103934665603ull;
                for (const F64 v : { e.mX, e.mY, e.mZ, t->mX, t->mY, t->mZ })
                {
                    key = (key ^ static_cast<U64>(std::llround(v * 4.0))) * 1099511628211ull;
                }
                fl.mKey = key;
                found.emplace_back(std::move(fl), e.mSpan + t->mSpan);
            }
            std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            const size_t before = flights.size();
            for (const auto& [f, w] : found)
            {
                const bool dup = std::any_of(flights.begin() + before, flights.end(), [&](const Flight& k) {
                    return (k.mBottom - f.mBottom).length() < DUP_M && (k.mTop - f.mTop).length() < DUP_M;
                });
                if (!dup)
                {
                    flights.push_back(f);
                }
            }
        }
    }
}

namespace sknav
{
    SlopePartition partitionSlopes(const std::vector<LLVector3>& verts, const std::vector<S32>& tris, const std::vector<U8>& areas,
                                   const std::vector<U8>& two_sided, U8 partitioned_area, F32 climb, S32 max_codes,
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
        std::vector<std::pair<S32, S32>> touching;
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
        buildConnectors(P, grp, kind, ngroups, out.mCodes, cues, out.mFlights);
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
                        if ((g.mFlags & AirGap::WALKABLE) && z >= f.mZMin - 0.25f && z <= f.mZMax + 0.25f &&
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
