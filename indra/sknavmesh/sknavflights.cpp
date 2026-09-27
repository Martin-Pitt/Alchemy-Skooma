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
}

namespace sknav
{
    SlopePartition partitionSlopes(const std::vector<LLVector3>& verts, const std::vector<S32>& tris, const std::vector<U8>& areas,
                                   const std::vector<U8>& two_sided, U8 partitioned_area, F32 climb, S32 max_codes)
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
        enum Kind : U8 { SLOPE, LANDING, TREAD };
        std::vector<U8> kind(ngroups, SLOPE);
        std::vector<std::pair<Riser, Riser>> tread_edges(ngroups);
        std::vector<F64> going(ngroups, 0.0);
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
                        tread_edges[g] = { *u, *d };
                    }
                }
            }
            if (best >= 0)
            {
                kind[g] = TREAD;
                going[g] = best;
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

        // 4. flight records, treads bottom to top with their nosings
        std::map<S32, std::vector<S32>> by;
        for (S32 g = 0; g < ngroups; ++g)
        {
            if (kind[g] == TREAD)
            {
                by[ff(g)].push_back(g);
            }
        }
        std::vector<F64> zsum(ngroups, 0.0), asum(ngroups, 0.0);
        std::vector<std::vector<LLVector2>> pts(ngroups);
        for (S32 t = 0; t < n; ++t)
        {
            const S32 g = grp[t];
            if (g >= 0 && kind[g] == TREAD)
            {
                zsum[g] += P.mC[t].z * P.mArea[t];
                asum[g] += P.mArea[t];
                for (S32 k = 0; k < 3; ++k)
                {
                    const V3& v = P.mV[P.mT[t * 3 + k]];
                    pts[ff(g)].emplace_back(static_cast<F32>(v.x), static_cast<F32>(v.y));
                }
            }
        }
        const auto vec = [&](S32 i) { return LLVector3(static_cast<F32>(P.mV[i].x), static_cast<F32>(P.mV[i].y), static_cast<F32>(P.mV[i].z)); };
        for (const auto& [root, treads] : by)
        {
            Flight f;
            S32 top = treads.front();
            F64 rise = 0;
            for (const S32 g : treads)
            {
                const Riser& down = tread_edges[g].second;
                FlightTread tr;
                tr.mZ = static_cast<F32>(zsum[g] / asum[g]);
                tr.mNosingA = vec(down.mA);
                tr.mNosingB = vec(down.mB);
                tr.mNosingA.mV[VZ] = tr.mNosingB.mV[VZ] = tr.mZ;
                tr.mRise = static_cast<F32>(-down.mDz);
                tr.mGoing = static_cast<F32>(going[g]);
                rise += tr.mRise;
                f.mTreads.push_back(tr);
                top = zsum[g] / asum[g] > zsum[top] / asum[top] ? g : top;
            }
            std::sort(f.mTreads.begin(), f.mTreads.end(), [](const FlightTread& a, const FlightTread& b) { return a.mZ < b.mZ; });
            const Riser& up = tread_edges[top].first;
            // the top nosing is the landing's, at its height
            f.mTopNosingA = vec(up.mA);
            f.mTopNosingB = vec(up.mB);
            f.mTopNosingA.mV[VZ] = f.mTopNosingB.mV[VZ] = static_cast<F32>(zsum[top] / asum[top] + up.mDz);
            f.mRise = static_cast<F32>((rise + up.mDz) / (treads.size() + 1));
            f.mZMin = f.mTreads.front().mZ - f.mTreads.front().mRise;
            f.mZMax = f.mTreads.back().mZ + static_cast<F32>(up.mDz);
            f.mFootprint = hull(pts[root]);
            f.mCode = out.mCodes[first[root]];

            // least squares z = a x + b y + c through the nosings' midpoints
            std::vector<LLVector3> mids;
            for (const FlightTread& tr : f.mTreads)
            {
                mids.push_back((tr.mNosingA + tr.mNosingB) * 0.5f);
            }
            mids.push_back((f.mTopNosingA + f.mTopNosingB) * 0.5f);
            F64 sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sxz = 0, syz = 0, sz = 0;
            const F64 m = static_cast<F64>(mids.size());
            for (const LLVector3& p : mids)
            {
                sx += p.mV[VX];
                sy += p.mV[VY];
                sz += p.mV[VZ];
            }
            sx /= m;
            sy /= m;
            sz /= m;
            for (const LLVector3& p : mids)
            {
                const F64 x = p.mV[VX] - sx, y = p.mV[VY] - sy, z = p.mV[VZ] - sz;
                sxx += x * x;
                sxy += x * y;
                syy += y * y;
                sxz += x * z;
                syz += y * z;
            }
            const F64 det = sxx * syy - sxy * sxy;
            F64 a = 0, b = 0;
            const F64 spread = sxx + syy;
            if (std::fabs(det) > 1e-6 * spread * spread)
            {
                a = (sxz * syy - syz * sxy) / det;
                b = (syz * sxx - sxz * sxy) / det;
            }
            else if (spread > 1e-9)
            {
                // a straight flight's nosings are collinear in plan: climb along their line
                const F64 angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
                const F64 dx = std::cos(angle), dy = std::sin(angle);
                const F64 s = (sxz * dx + syz * dy) / (sxx * dx * dx + 2 * sxy * dx * dy + syy * dy * dy);
                a = s * dx;
                b = s * dy;
            }
            f.mPlane[0] = static_cast<F32>(a);
            f.mPlane[1] = static_cast<F32>(b);
            f.mPlane[2] = static_cast<F32>(sz - a * sx - b * sy);
            out.mFlights.push_back(std::move(f));
        }
        return out;
    }

    void mapFlightPolys(std::vector<Flight>& flights, const std::vector<LLVector3>& centroids, const std::vector<U8>& codes, F32 climb)
    {
        for (Flight& f : flights)
        {
            f.mPolys.clear();
            for (size_t p = 0; p < centroids.size() && p < 0xffff; ++p)
            {
                const LLVector3& c = centroids[p];
                if (codes[p] == f.mCode && c.mV[VZ] >= f.mZMin - climb && c.mV[VZ] <= f.mZMax + climb &&
                    outside(f.mFootprint, LLVector2(c.mV[VX], c.mV[VY])) < 0.25f)
                {
                    f.mPolys.push_back(static_cast<U16>(p));
                }
            }
        }
        flights.erase(std::remove_if(flights.begin(), flights.end(), [](const Flight& f) { return f.mPolys.empty(); }), flights.end());
    }
}
