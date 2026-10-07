/**
 * @file skmapsection.cpp
 * @brief Census records cut by horizontal planes: the mini-map's walls as a section through the geometry.
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

#include "skmapsection.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

namespace skmap
{
    namespace
    {
        constexpr F64 PI_64 = 3.14159265358979323846;

        struct D2
        {
            F64 x = 0.0, y = 0.0;
            bool operator<(const D2& o) const { return x != o.x ? x < o.x : y < o.y; }
            bool operator==(const D2& o) const { return x == o.x && y == o.y; }
        };

        struct D3
        {
            F64 x = 0.0, y = 0.0, z = 0.0;
        };

        D3 d3(const LLVector3& v)
        {
            return { v.mV[VX], v.mV[VY], v.mV[VZ] };
        }

        F64 cross(const D2& o, const D2& a, const D2& b)
        {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        F64 signedArea(const D2* p, size_t n)
        {
            F64 s = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                const D2& a = p[i];
                const D2& b = p[(i + 1) % n];
                s += a.x * b.y - a.y * b.x;
            }
            return 0.5 * s;
        }

        void appendLoop(SliceResult& s, const std::vector<LLVector2>& pts)
        {
            SliceLoop loop;
            loop.mFirst = static_cast<U32>(s.mPts.size());
            loop.mCount = static_cast<U32>(pts.size());
            s.mPts.insert(s.mPts.end(), pts.begin(), pts.end());
            s.mLoops.push_back(loop);
        }

        /// Monotone chain over pts[0, n), sorted in place, counter-clockwise; false when fewer than three distinct points
        /// remain. The lower chain then the upper, each without its last point, as two separate chains would give.
        bool hull2d(D2* pts, size_t n, std::vector<LLVector2>& out)
        {
            out.clear();
            std::sort(pts, pts + n);
            n = static_cast<size_t>(std::unique(pts, pts + n) - pts);
            if (n < 3)
            {
                return false;
            }
            // Small hulls on the stack; a decomposition body's many crossings in scratch kept per thread.
            constexpr size_t SMALL = 64;
            D2 small[2 * SMALL + 1];
            thread_local std::vector<D2> large;
            D2* h = small;
            if (n > SMALL)
            {
                large.resize(2 * n + 1);
                h = large.data();
            }
            size_t k = 0;
            for (size_t i = 0; i < n; ++i)
            {
                while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0.0)
                {
                    --k;
                }
                h[k++] = pts[i];
            }
            // The upper chain starts at the last point, which it never pops.
            const size_t base = k;
            for (size_t i = n - 1; i-- > 0;)
            {
                while (k > base && cross(h[k - 2], h[k - 1], pts[i]) <= 0.0)
                {
                    --k;
                }
                h[k++] = pts[i];
            }
            --k;    // the first point again
            if (k < 3 || std::fabs(signedArea(h, k)) < 1e-12)
            {
                return false;
            }
            for (size_t i = 0; i < k; ++i)
            {
                out.emplace_back(static_cast<F32>(h[i].x), static_cast<F32>(h[i].y));
            }
            return true;
        }

        bool hull2d(std::vector<D2>& pts, std::vector<LLVector2>& out)
        {
            return hull2d(pts.data(), pts.size(), out);
        }

        /// The most edges a convex polytope here has: a tilted cylinder's prism at the largest tessellation.
        constexpr size_t MAX_EDGES = 3 * 64;

        /// A convex polytope's section: each straddling edge's crossing, taken from its lower end, then the hull.
        bool sliceEdges(const D3* v, const std::pair<S32, S32>* edges, size_t count, F64 c, std::vector<LLVector2>& out)
        {
            D2 pts[MAX_EDGES];
            size_t n = 0;
            for (size_t i = 0; i < count; ++i)
            {
                const D3& p = v[edges[i].first];
                const D3& q = v[edges[i].second];
                if ((p.z >= c) != (q.z >= c))
                {
                    const D3& lo = p.z < q.z ? p : q;
                    const D3& hi = p.z < q.z ? q : p;
                    const F64 t = (c - lo.z) / (hi.z - lo.z);
                    pts[n++] = { lo.x + t * (hi.x - lo.x), lo.y + t * (hi.y - lo.y) };
                }
            }
            return n >= 3 && hull2d(pts, n, out);
        }

        S32 tessellation(F64 r)
        {
            if (r <= SECTION_EPS_CHORD_M * 0.5)
            {
                return 12;
            }
            const F64 n = std::ceil(PI_64 / std::acos(1.0 - SECTION_EPS_CHORD_M / r));
            return static_cast<S32>(llclamp(n, 12.0, 64.0));
        }

        /// The principal axes of a conjugate pair, from the symmetric A A^T of A = [v1 v2]; u2 is u1 turned left.
        void ellipseD(const D2& centre, const D2& v1, const D2& v2, std::vector<LLVector2>& out)
        {
            out.clear();
            const F64 a = v1.x * v1.x + v2.x * v2.x;
            const F64 b = v1.x * v1.y + v2.x * v2.y;
            const F64 d = v1.y * v1.y + v2.y * v2.y;
            const F64 half = 0.5 * (a + d);
            const F64 disc = std::sqrt(llmax(0.0, 0.25 * (a - d) * (a - d) + b * b));
            const F64 l1 = half + disc, l2 = llmax(0.0, half - disc);
            D2 u1;
            if (std::fabs(b) > 1e-300)
            {
                u1 = { l1 - d, b };
            }
            else
            {
                u1 = a >= d ? D2{ 1.0, 0.0 } : D2{ 0.0, 1.0 };
            }
            const F64 len = std::sqrt(u1.x * u1.x + u1.y * u1.y);
            if (len <= 0.0)
            {
                return;
            }
            u1.x /= len;
            u1.y /= len;
            const D2 u2 = { -u1.y, u1.x };
            const F64 s0 = std::sqrt(l1), s1 = std::sqrt(l2);
            const S32 n = tessellation(s0);
            for (S32 i = 0; i < n; ++i)
            {
                const F64 t = 2.0 * PI_64 * i / n;
                const F64 ct = std::cos(t) * s0, st = std::sin(t) * s1;
                out.emplace_back(static_cast<F32>(centre.x + ct * u1.x + st * u2.x), static_cast<F32>(centre.y + ct * u1.y + st * u2.y));
            }
        }

        bool sliceBox(const sknav::Record& r, F64 c, std::vector<LLVector2>& out)
        {
            // Built once, before any caller can see it, so slicing stays safe on any thread.
            static const std::vector<std::pair<S32, S32>> edges = [] {
                std::vector<std::pair<S32, S32>> out;
                for (S32 i = 0; i < 8; ++i)
                {
                    for (S32 b = 0; b < 3; ++b)
                    {
                        const S32 j = i ^ (1 << b);
                        if (i < j)
                        {
                            out.emplace_back(i, j);
                        }
                    }
                }
                return out;
            }();
            D3 v[8];
            for (S32 i = 0; i < 8; ++i)
            {
                D3 p = d3(r.mCenter);
                for (S32 b = 0; b < 3; ++b)
                {
                    const F64 s = ((i >> b) & 1) ? 1.0 : -1.0;
                    const F64 h = s * r.mHalf.mV[b];
                    p.x += h * r.mAxes[b].mV[VX];
                    p.y += h * r.mAxes[b].mV[VY];
                    p.z += h * r.mAxes[b].mV[VZ];
                }
                v[i] = p;
            }
            return sliceEdges(v, edges.data(), edges.size(), c, out);
        }

        /// The axis vector h_i a_i.
        D3 column(const sknav::Record& r, S32 i)
        {
            return { r.mHalf.mV[i] * static_cast<F64>(r.mAxes[i].mV[VX]), r.mHalf.mV[i] * static_cast<F64>(r.mAxes[i].mV[VY]),
                     r.mHalf.mV[i] * static_cast<F64>(r.mAxes[i].mV[VZ]) };
        }

        bool sliceEllipsoid(const sknav::Record& r, F64 c, std::vector<LLVector2>& out)
        {
            const D3 col[3] = { column(r, 0), column(r, 1), column(r, 2) };
            const F64 m[3] = { col[0].z, col[1].z, col[2].z };
            const F64 d = c - r.mCenter.mV[VZ];
            const F64 mm = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
            if (mm < 1e-12 || d * d >= mm)
            {
                return false;
            }
            const F64 rho = std::sqrt(1.0 - d * d / mm);
            S32 least = 0;
            for (S32 i = 1; i < 3; ++i)
            {
                if (std::fabs(m[i]) < std::fabs(m[least]))
                {
                    least = i;
                }
            }
            const F64 k[3] = { least == 0 ? 1.0 : 0.0, least == 1 ? 1.0 : 0.0, least == 2 ? 1.0 : 0.0 };
            F64 e1[3] = { m[1] * k[2] - m[2] * k[1], m[2] * k[0] - m[0] * k[2], m[0] * k[1] - m[1] * k[0] };
            const F64 l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
            for (F64& v : e1)
            {
                v /= l1;
            }
            const F64 ml = std::sqrt(mm);
            const F64 mn[3] = { m[0] / ml, m[1] / ml, m[2] / ml };
            const F64 e2[3] = { mn[1] * e1[2] - mn[2] * e1[1], mn[2] * e1[0] - mn[0] * e1[2], mn[0] * e1[1] - mn[1] * e1[0] };
            auto apply = [&](const F64 u[3]) {
                return D2{ col[0].x * u[0] + col[1].x * u[1] + col[2].x * u[2], col[0].y * u[0] + col[1].y * u[1] + col[2].y * u[2] };
            };
            const F64 u0[3] = { m[0] * d / mm, m[1] * d / mm, m[2] * d / mm };
            const D2 shift = apply(u0);
            const D2 centre = { r.mCenter.mV[VX] + shift.x, r.mCenter.mV[VY] + shift.y };
            const D2 a = apply(e1), b = apply(e2);
            ellipseD(centre, { rho * a.x, rho * a.y }, { rho * b.x, rho * b.y }, out);
            return out.size() >= 3;
        }

        bool sliceCylinder(const sknav::Record& r, F64 c, std::vector<LLVector2>& out)
        {
            const D3 c0 = column(r, 0), c1 = column(r, 1), c2 = column(r, 2);
            const F64 al = c0.z, be = c1.z, ga = c2.z;
            const F64 d = c - r.mCenter.mV[VZ];
            const F64 tol = 1e-5 * llmax(r.mHalf.mV[0], llmax(r.mHalf.mV[1], r.mHalf.mV[2]));
            const D3 centre = d3(r.mCenter);
            if (std::fabs(al) <= tol && std::fabs(be) <= tol)
            {
                // Upright: the whole cross-section, constant along the axis.
                const F64 g = std::fabs(ga);
                if (g < 1e-9 || d <= -g || d > g)
                {
                    return false;
                }
                const F64 w = d / ga;
                ellipseD({ centre.x + w * c2.x, centre.y + w * c2.y }, { c0.x, c0.y }, { c1.x, c1.y }, out);
                return out.size() >= 3;
            }
            if (std::fabs(ga) <= tol)
            {
                // Lying: a chord of the disc swept along the axis, a parallelogram.
                const F64 s = std::sqrt(al * al + be * be);
                if (d <= -s || d > s)
                {
                    return false;
                }
                const F64 bx = al * d / (s * s), by = be * d / (s * s);
                const F64 k = std::sqrt(llmax(0.0, 1.0 - d * d / (s * s))) / s;
                const F64 px = -be * k, py = al * k;
                D2 pts[4];
                size_t n = 0;
                for (S32 sign : { 1, -1 })
                {
                    const F64 qu = bx + sign * px, qv = by + sign * py;
                    for (S32 sw : { -1, 1 })
                    {
                        pts[n++] = { centre.x + qu * c0.x + qv * c1.x + sw * c2.x, centre.y + qu * c0.y + qv * c1.y + sw * c2.y };
                    }
                }
                return hull2d(pts, n, out);
            }
            // Tilted: the inscribed prism sliced like a box; the section stretches by 1 / |a2.z| and so does the bound.
            const S32 n = tessellation(llmax(r.mHalf.mV[0], r.mHalf.mV[1]) / llmax(std::fabs(static_cast<F64>(r.mAxes[2].mV[VZ])), 0.2));
            D3 v[2 * 64];
            for (S32 i = 0; i < n; ++i)
            {
                const F64 t = 2.0 * PI_64 * i / n;
                const F64 ct = std::cos(t), st = std::sin(t);
                const D3 ring = { centre.x + ct * c0.x + st * c1.x, centre.y + ct * c0.y + st * c1.y, centre.z + ct * c0.z + st * c1.z };
                v[i] = { ring.x - c2.x, ring.y - c2.y, ring.z - c2.z };
                v[n + i] = { ring.x + c2.x, ring.y + c2.y, ring.z + c2.z };
            }
            std::pair<S32, S32> edges[MAX_EDGES];
            size_t count = 0;
            for (S32 i = 0; i < n; ++i)
            {
                edges[count++] = { i, (i + 1) % n };
            }
            for (S32 i = 0; i < n; ++i)
            {
                edges[count++] = { n + i, n + (i + 1) % n };
            }
            for (S32 i = 0; i < n; ++i)
            {
                edges[count++] = { i, n + i };
            }
            return sliceEdges(v, edges, count, c, out);
        }

        struct Seg
        {
            LLVector2 mA, mB;
        };

        bool lexLess(const LLVector3& p, const LLVector3& q)
        {
            if (p.mV[VX] != q.mV[VX])
            {
                return p.mV[VX] < q.mV[VX];
            }
            if (p.mV[VY] != q.mV[VY])
            {
                return p.mV[VY] < q.mV[VY];
            }
            return p.mV[VZ] < q.mV[VZ];
        }

        /// One segment for a straddling triangle; crossings from each edge's lexicographically smaller end, so a shared edge
        /// gives bit-identical points in either triangle.
        bool triangleSegment(const LLVector3* tri, F32 c, Seg& out)
        {
            const F32 zmin = llmin(tri[0].mV[VZ], llmin(tri[1].mV[VZ], tri[2].mV[VZ]));
            const F32 zmax = llmax(tri[0].mV[VZ], llmax(tri[1].mV[VZ], tri[2].mV[VZ]));
            if (zmax < c || zmin >= c)
            {
                return false;
            }
            LLVector2 pts[2];
            S32 n = 0;
            for (S32 e = 0; e < 3; ++e)
            {
                const LLVector3* p = &tri[e];
                const LLVector3* q = &tri[(e + 1) % 3];
                if ((p->mV[VZ] >= c) == (q->mV[VZ] >= c))
                {
                    continue;
                }
                if (lexLess(*q, *p))
                {
                    std::swap(p, q);
                }
                const F64 t = (static_cast<F64>(c) - p->mV[VZ]) / (static_cast<F64>(q->mV[VZ]) - p->mV[VZ]);
                if (n < 2)
                {
                    // Adding 0 turns -0 into +0, so equal points hash equal.
                    pts[n++].set(static_cast<F32>(p->mV[VX] + t * (static_cast<F64>(q->mV[VX]) - p->mV[VX])) + 0.f,
                                 static_cast<F32>(p->mV[VY] + t * (static_cast<F64>(q->mV[VY]) - p->mV[VY])) + 0.f);
                }
            }
            if (n != 2 || pts[0] == pts[1])
            {
                return false;
            }
            out.mA = pts[0];
            out.mB = pts[1];
            return true;
        }

        U64 pointKey(const LLVector2& p)
        {
            U32 x, y;
            std::memcpy(&x, &p.mV[VX], sizeof(x));
            std::memcpy(&y, &p.mV[VY], sizeof(y));
            return (static_cast<U64>(x) << 32) | y;
        }

        F64 dist(const LLVector2& a, const LLVector2& b)
        {
            const F64 dx = static_cast<F64>(a.mV[VX]) - b.mV[VX], dy = static_cast<F64>(a.mV[VY]) - b.mV[VY];
            return std::sqrt(dx * dx + dy * dy);
        }

        typedef std::pair<S64, S64> GridKey;

        GridKey gridOf(const LLVector2& p, F64 size)
        {
            return { static_cast<S64>(std::floor(p.mV[VX] / size)), static_cast<S64>(std::floor(p.mV[VY] / size)) };
        }

        S32 findRoot(std::vector<S32>& parent, S32 x)
        {
            while (parent[x] != x)
            {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        }

        F64 dist2(const LLVector2& a, const LLVector2& b)
        {
            const F64 dx = static_cast<F64>(a.mV[VX]) - b.mV[VX], dy = static_cast<F64>(a.mV[VY]) - b.mV[VY];
            return dx * dx + dy * dy;
        }

        /// Up to this many nodes or open paths, pairs are tested directly rather than through a grid; both find the
        /// same pairs, since the grid only skips pairs further apart than the tolerance.
        constexpr size_t SMALL_CHAIN = 256;

        struct ChainScratch
        {
            std::vector<U64> mKeys;
            std::vector<std::pair<S32, S32>> mEnds, mJoined;
            std::vector<S32> mParent, mIncident, mFwd, mBack;
            std::vector<U32> mFirst, mFill, mNextFree;
            std::vector<U8> mUsed;
        };

        /// Segments into closed paths (front == back) and open ones: exact ends, a 1 mm snap, then a 2 cm close nearest first.
        void chain(const std::vector<Seg>& segs, std::vector<LLVector2>& nodes, std::vector<std::vector<S32>>& closed,
                   std::vector<std::vector<S32>>& open)
        {
            SK_SECTION_STAGE(CHAIN);
            // Scratch kept per thread: a soup's slice is small and frequent, so allocations would dominate it.
            thread_local ChainScratch scratch;
            ChainScratch& w = scratch;
            // Nodes by exact plan coordinates, numbered in order of first appearance.
            std::vector<U64>& keys = w.mKeys;
            keys.clear();
            // Built only for large soups: an empty unordered_map still allocates.
            std::unique_ptr<std::unordered_map<U64, S32>> index;
            const bool few = segs.size() * 2 <= SMALL_CHAIN;
            if (!few)
            {
                index = std::make_unique<std::unordered_map<U64, S32>>();
                index->reserve(segs.size() * 2);
            }
            auto node = [&](const LLVector2& p) {
                const U64 key = pointKey(p);
                if (few)
                {
                    for (size_t i = 0; i < keys.size(); ++i)
                    {
                        if (keys[i] == key)
                        {
                            return static_cast<S32>(i);
                        }
                    }
                }
                else
                {
                    const auto it = index->find(key);
                    if (it != index->end())
                    {
                        return it->second;
                    }
                    index->emplace(key, static_cast<S32>(keys.size()));
                }
                keys.push_back(key);
                nodes.push_back(p);
                return static_cast<S32>(keys.size()) - 1;
            };
            std::vector<std::pair<S32, S32>>& ends = w.mEnds;
            ends.clear();
            for (const Seg& sg : segs)
            {
                const S32 a = node(sg.mA);
                const S32 b = node(sg.mB);
                ends.emplace_back(a, b);
            }
            // Nodes within the snap join; each component keeps its lowest id, whatever the order of the joins.
            const size_t n = nodes.size();
            std::vector<S32>& parent = w.mParent;
            parent.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                parent[i] = static_cast<S32>(i);
            }
            const F64 snap2 = static_cast<F64>(SECTION_SNAP_M) * SECTION_SNAP_M;
            auto unite = [&](S32 i, S32 j) {
                const S32 a = findRoot(parent, i), b = findRoot(parent, j);
                if (a != b)
                {
                    parent[llmax(a, b)] = llmin(a, b);
                }
            };
            if (n <= SMALL_CHAIN)
            {
                for (size_t i = 1; i < n; ++i)
                {
                    for (size_t j = 0; j < i; ++j)
                    {
                        if (dist2(nodes[j], nodes[i]) <= snap2)
                        {
                            unite(static_cast<S32>(i), static_cast<S32>(j));
                        }
                    }
                }
            }
            else
            {
                std::map<GridKey, std::vector<S32>> grid;
                for (size_t i = 0; i < n; ++i)
                {
                    const GridKey g = gridOf(nodes[i], SECTION_SNAP_M);
                    for (S64 dx = -1; dx <= 1; ++dx)
                    {
                        for (S64 dy = -1; dy <= 1; ++dy)
                        {
                            const auto it = grid.find({ g.first + dx, g.second + dy });
                            if (it == grid.end())
                            {
                                continue;
                            }
                            for (S32 j : it->second)
                            {
                                if (dist2(nodes[j], nodes[i]) <= snap2)
                                {
                                    unite(static_cast<S32>(i), j);
                                }
                            }
                        }
                    }
                    grid[g].push_back(static_cast<S32>(i));
                }
            }
            std::vector<std::pair<S32, S32>>& joined = w.mJoined;
            joined.clear();
            for (const auto& e : ends)
            {
                const S32 a = findRoot(parent, e.first), b = findRoot(parent, e.second);
                if (a != b)
                {
                    joined.emplace_back(a, b);
                }
            }
            // Each node's segments in segment order, packed.
            std::vector<U32>& first = w.mFirst;
            first.assign(n + 1, 0);
            for (const auto& e : joined)
            {
                first[e.first + 1]++;
                first[e.second + 1]++;
            }
            for (size_t i = 0; i < n; ++i)
            {
                first[i + 1] += first[i];
            }
            std::vector<S32>& incident = w.mIncident;
            incident.resize(first[n]);
            std::vector<U32>& fill = w.mFill;
            fill.assign(first.begin(), first.end() - 1);
            for (size_t sgi = 0; sgi < joined.size(); ++sgi)
            {
                incident[fill[joined[sgi].first]++] = static_cast<S32>(sgi);
                incident[fill[joined[sgi].second]++] = static_cast<S32>(sgi);
            }
            std::vector<U32>& next_free = w.mNextFree;
            next_free.assign(first.begin(), first.end() - 1);
            std::vector<U8>& used = w.mUsed;
            used.assign(joined.size(), 0);
            auto nextAt = [&](S32 tip) -> S32 {
                U32& k = next_free[tip];
                while (k < first[tip + 1] && used[incident[k]])
                {
                    ++k;
                }
                return k < first[tip + 1] ? incident[k] : -1;
            };
            // A path is grown both ways: back holds its head end reversed, fwd the rest.
            std::vector<S32>& fwd = w.mFwd;
            std::vector<S32>& back = w.mBack;
            for (size_t sgi = 0; sgi < joined.size(); ++sgi)
            {
                if (used[sgi])
                {
                    continue;
                }
                used[sgi] = 1;
                fwd.assign({ joined[sgi].first, joined[sgi].second });
                back.clear();
                auto head = [&]() { return back.empty() ? fwd.front() : back.back(); };
                bool is_closed = false;
                for (S32 direction : { 1, -1 })
                {
                    while (!is_closed)
                    {
                        const S32 tip = direction == 1 ? fwd.back() : head();
                        if (direction == 1 && fwd.size() + back.size() > 2 && fwd.back() == head())
                        {
                            is_closed = true;
                            break;
                        }
                        const S32 nxt = nextAt(tip);
                        if (nxt < 0)
                        {
                            break;
                        }
                        used[nxt] = 1;
                        const S32 other = joined[nxt].first == tip ? joined[nxt].second : joined[nxt].first;
                        if (direction == 1)
                        {
                            fwd.push_back(other);
                        }
                        else
                        {
                            back.push_back(other);
                        }
                        if (head() == fwd.back())
                        {
                            is_closed = true;
                        }
                    }
                    if (is_closed)
                    {
                        break;
                    }
                }
                std::vector<S32> path(back.rbegin(), back.rend());
                path.insert(path.end(), fwd.begin(), fwd.end());
                (is_closed ? closed : open).push_back(std::move(path));
            }

            // Close what is left within 2 cm, nearest first; ties by the lower polyline index, then in the order the
            // ends are tried: tail to head, head to tail, tail to tail, head to head.
            const F64 close2 = static_cast<F64>(SECTION_CLOSE_M) * SECTION_CLOSE_M;
            SK_SECTION_STAGE(CLOSE);
            while (!open.empty())
            {
                bool found = false;
                F64 best_d = 0.0;
                S32 best_i = 0, best_j = 0, best_how = 0;     // how: 0 self, 1 ij, 2 ji, 3 ijr, 4 rij
                auto consider = [&](F64 d, S32 i, S32 j, S32 how) {
                    if (d > close2)
                    {
                        return;
                    }
                    if (!found || d < best_d || (d == best_d && (i < best_i || (i == best_i && (j < best_j || (j == best_j && how < best_how))))))
                    {
                        found = true;
                        best_d = d;
                        best_i = i;
                        best_j = j;
                        best_how = how;
                    }
                };
                // One end of path i against an end of path j >= i; a path's own pair is its tail against its head.
                auto pair = [&](S32 i, bool head_i, S32 j, bool head_j) {
                    const LLVector2& p = nodes[head_i ? open[i].front() : open[i].back()];
                    const LLVector2& q = nodes[head_j ? open[j].front() : open[j].back()];
                    const F64 d = dist2(p, q);
                    if (i == j)
                    {
                        consider(d, i, i, 0);
                        return;
                    }
                    consider(d, i, j, !head_i && head_j ? 1 : head_i && !head_j ? 2 : !head_i ? 3 : 4);
                };
                if (open.size() <= SMALL_CHAIN)
                {
                    for (size_t i = 0; i < open.size(); ++i)
                    {
                        const S32 si = static_cast<S32>(i);
                        pair(si, false, si, true);
                        for (size_t j = i + 1; j < open.size(); ++j)
                        {
                            const S32 sj = static_cast<S32>(j);
                            pair(si, false, sj, true);
                            pair(si, true, sj, false);
                            pair(si, false, sj, false);
                            pair(si, true, sj, true);
                        }
                    }
                }
                else
                {
                    struct End
                    {
                        S32 mPath;
                        bool mHead;
                    };
                    std::map<GridKey, std::vector<End>> ends_grid;
                    for (size_t i = 0; i < open.size(); ++i)
                    {
                        ends_grid[gridOf(nodes[open[i].front()], SECTION_CLOSE_M)].push_back({ static_cast<S32>(i), true });
                        ends_grid[gridOf(nodes[open[i].back()], SECTION_CLOSE_M)].push_back({ static_cast<S32>(i), false });
                    }
                    for (size_t i = 0; i < open.size(); ++i)
                    {
                        const S32 si = static_cast<S32>(i);
                        for (bool head : { true, false })
                        {
                            const GridKey g = gridOf(nodes[head ? open[i].front() : open[i].back()], SECTION_CLOSE_M);
                            for (S64 dx = -1; dx <= 1; ++dx)
                            {
                                for (S64 dy = -1; dy <= 1; ++dy)
                                {
                                    const auto it = ends_grid.find({ g.first + dx, g.second + dy });
                                    if (it == ends_grid.end())
                                    {
                                        continue;
                                    }
                                    for (const End& e : it->second)
                                    {
                                        // Later paths, and this path's head seen from its tail.
                                        if (e.mPath < si || (e.mPath == si && (head || !e.mHead)))
                                        {
                                            continue;
                                        }
                                        pair(si, head, e.mPath, e.mHead);
                                    }
                                }
                            }
                        }
                    }
                }
                if (!found)
                {
                    break;
                }
                if (best_how == 0)
                {
                    std::vector<S32> p = std::move(open[best_i]);
                    open.erase(open.begin() + best_i);
                    p.push_back(p.front());
                    closed.push_back(std::move(p));
                    continue;
                }
                const std::vector<S32>& pi = open[best_i];
                const std::vector<S32>& pj = open[best_j];
                std::vector<S32> merged;
                switch (best_how)
                {
                    case 1:
                        merged = pi;
                        merged.insert(merged.end(), pj.begin(), pj.end());
                        break;
                    case 2:
                        merged = pj;
                        merged.insert(merged.end(), pi.begin(), pi.end());
                        break;
                    case 3:
                        merged = pi;
                        merged.insert(merged.end(), pj.rbegin(), pj.rend());
                        break;
                    default:
                        merged.assign(pi.rbegin(), pi.rend());
                        merged.insert(merged.end(), pj.begin(), pj.end());
                        break;
                }
                open.erase(open.begin() + best_j);
                open.erase(open.begin() + best_i);
                open.push_back(std::move(merged));
            }
        }

        /// Whether p lies within the merge distance of the chord a-b and turns less than the merge angle there.
        bool collinear(const LLVector2& a, const LLVector2& p, const LLVector2& b)
        {
            const F64 ax = p.mV[VX] - static_cast<F64>(a.mV[VX]), ay = p.mV[VY] - static_cast<F64>(a.mV[VY]);
            const F64 bx = b.mV[VX] - static_cast<F64>(p.mV[VX]), by = b.mV[VY] - static_cast<F64>(p.mV[VY]);
            const F64 la = std::sqrt(ax * ax + ay * ay), lb = std::sqrt(bx * bx + by * by);
            if (la < 1e-9 || lb < 1e-9)
            {
                return true;
            }
            const F64 turn = std::atan2(std::fabs(ax * by - ay * bx), ax * bx + ay * by);
            if (turn >= SECTION_MERGE_DEG * DEG_TO_RAD)
            {
                return false;
            }
            // Distance from p to the chord a-b.
            const F64 cx = b.mV[VX] - static_cast<F64>(a.mV[VX]), cy = b.mV[VY] - static_cast<F64>(a.mV[VY]);
            const F64 lc2 = cx * cx + cy * cy;
            const F64 t = lc2 > 0.0 ? llclamp((ax * cx + ay * cy) / lc2, 0.0, 1.0) : 0.0;
            const F64 dx = a.mV[VX] + t * cx - p.mV[VX], dy = a.mV[VY] + t * cy - p.mV[VY];
            return std::sqrt(dx * dx + dy * dy) <= SECTION_MERGE_M;
        }

        void simplifyLine(std::vector<LLVector2>& pts)
        {
            SK_SECTION_STAGE(SIMPLIFY);
            if (pts.size() <= 2)
            {
                return;
            }
            std::vector<LLVector2> out;
            out.push_back(pts.front());
            for (size_t i = 1; i + 1 < pts.size(); ++i)
            {
                if (!collinear(out.back(), pts[i], pts[i + 1]))
                {
                    out.push_back(pts[i]);
                }
            }
            out.push_back(pts.back());
            pts.swap(out);
        }

        void simplifyLoop(std::vector<LLVector2>& pts)
        {
            SK_SECTION_STAGE(SIMPLIFY);
            const size_t n = pts.size();
            if (n <= 3)
            {
                return;
            }
            std::vector<LLVector2> out;
            size_t removed = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const LLVector2& prev = out.empty() ? pts[n - 1] : out.back();
                if (n - removed > 3 && collinear(prev, pts[i], pts[(i + 1) % n]))
                {
                    ++removed;
                    continue;
                }
                out.push_back(pts[i]);
            }
            // The seam: the first and last kept points against their kept neighbours.
            bool changed = true;
            while (changed && out.size() > 3)
            {
                changed = false;
                if (collinear(out.back(), out.front(), out[1]))
                {
                    out.erase(out.begin());
                    changed = true;
                }
                else if (collinear(out[out.size() - 2], out.back(), out.front()))
                {
                    out.pop_back();
                    changed = true;
                }
            }
            pts.swap(out);
        }

        F64 chordDistance(const LLVector2& a, const LLVector2& b, const LLVector2& p)
        {
            const F64 cx = b.mV[VX] - static_cast<F64>(a.mV[VX]), cy = b.mV[VY] - static_cast<F64>(a.mV[VY]);
            const F64 px = p.mV[VX] - static_cast<F64>(a.mV[VX]), py = p.mV[VY] - static_cast<F64>(a.mV[VY]);
            const F64 l2 = cx * cx + cy * cy;
            const F64 t = l2 > 0.0 ? llclamp((px * cx + py * cy) / l2, 0.0, 1.0) : 0.0;
            const F64 dx = px - t * cx, dy = py - t * cy;
            return std::sqrt(dx * dx + dy * dy);
        }

        /// Douglas-Peucker on a closed loop, split at point 0 and the point farthest from it.
        void douglasPeucker(std::vector<LLVector2>& pts)
        {
            const size_t n = pts.size();
            size_t apex = 0;
            F64 apex_d = -1.0;
            for (size_t i = 1; i < n; ++i)
            {
                const F64 d = dist(pts[0], pts[i]);
                if (d > apex_d)
                {
                    apex_d = d;
                    apex = i;
                }
            }
            std::vector<LLVector2> ring(pts);
            ring.push_back(pts[0]);
            std::vector<bool> keep(n + 1, false);
            keep[0] = keep[apex] = keep[n] = true;
            std::vector<std::pair<size_t, size_t>> stack = { { 0, apex }, { apex, n } };
            while (!stack.empty())
            {
                const auto range = stack.back();
                stack.pop_back();
                F64 best = -1.0;
                size_t at = 0;
                for (size_t k = range.first + 1; k < range.second; ++k)
                {
                    const F64 d = chordDistance(ring[range.first], ring[range.second], ring[k]);
                    if (d > best)
                    {
                        best = d;
                        at = k;
                    }
                }
                if (best > SECTION_DP_M)
                {
                    keep[at] = true;
                    stack.emplace_back(range.first, at);
                    stack.emplace_back(at, range.second);
                }
            }
            std::vector<LLVector2> out;
            for (size_t i = 0; i < n; ++i)
            {
                if (keep[i])
                {
                    out.push_back(pts[i]);
                }
            }
            if (out.size() >= 3)
            {
                pts.swap(out);
            }
        }

        F32 loopLength(const LLVector2* pts, U32 count)
        {
            F64 s = 0.0;
            for (U32 i = 0; i < count; ++i)
            {
                s += dist(pts[i], pts[(i + 1) % count]);
            }
            return static_cast<F32>(s);
        }

        /// One group per +1 loop with its direct -1 children.
        void buildGroups(SliceResult& s)
        {
            s.mGroups.clear();
            for (size_t i = 0; i < s.mLoops.size(); ++i)
            {
                const SliceLoop& outer = s.mLoops[i];
                if (outer.mWind != 1)
                {
                    continue;
                }
                SliceGroup g;
                g.mOuter = static_cast<U16>(i);
                const LLVector2* op = &s.mPts[outer.mFirst];
                F32 area = std::fabs(loopArea(op, outer.mCount));
                F32 perimeter = loopLength(op, outer.mCount);
                g.mMin.set(FLT_MAX, FLT_MAX);
                g.mMax.set(-FLT_MAX, -FLT_MAX);
                for (U32 k = 0; k < outer.mCount; ++k)
                {
                    g.mMin.set(llmin(g.mMin.mV[VX], op[k].mV[VX]), llmin(g.mMin.mV[VY], op[k].mV[VY]));
                    g.mMax.set(llmax(g.mMax.mV[VX], op[k].mV[VX]), llmax(g.mMax.mV[VY], op[k].mV[VY]));
                }
                for (size_t j = 0; j < s.mLoops.size(); ++j)
                {
                    const SliceLoop& hole = s.mLoops[j];
                    if (hole.mWind == -1 && hole.mParent == static_cast<S32>(i))
                    {
                        g.mHoles.push_back(static_cast<U16>(j));
                        area -= std::fabs(loopArea(&s.mPts[hole.mFirst], hole.mCount));
                        perimeter += loopLength(&s.mPts[hole.mFirst], hole.mCount);
                    }
                }
                g.mArea = area;
                g.mPerimeter = perimeter;
                g.mThickness = 2.f * area / llmax(perimeter, 1e-9f);
                s.mGroups.push_back(std::move(g));
            }
        }

        S32 binOf(F32 z, F32 z0)
        {
            return static_cast<S32>(std::floor((z - z0) / SECTION_ZBIN_M));
        }

        void sliceSoup(const sknav::Record& rec, F32 c, const ZBins* bins, SliceResult& s)
        {
            if (!rec.mTriangles)
            {
                return;
            }
            const std::vector<LLVector3>& tris = *rec.mTriangles;
            const U32 ntri = static_cast<U32>(tris.size() / 3);
            // With bins, only the plane's bin is visited, in ascending triangle id as unbinned.
            const U32* ids = nullptr;
            U32 nids = ntri;
            if (bins)
            {
                const S32 b = binOf(c, bins->mZ0);
                if (b < 0 || b + 1 >= static_cast<S32>(bins->mStart.size()))
                {
                    return;
                }
                ids = bins->mTris.data() + bins->mStart[b];
                nids = bins->mStart[b + 1] - bins->mStart[b];
            }
            auto triAt = [&](U32 k) { return ids ? ids[k] : k; };

            U32 total = 0;
            for (const S32 count : rec.mConvexTris)
            {
                total += static_cast<U32>(llmax(count, 0));
            }
            if (rec.mSolid && rec.mConvexTris.size() > 1 && total == ntri)
            {
                // A decomposition: each body is its own convex hull, never nested in another.
                std::vector<U32> starts;
                U32 at = 0;
                for (const S32 count : rec.mConvexTris)
                {
                    starts.push_back(at);
                    at += static_cast<U32>(llmax(count, 0));
                }
                std::vector<std::vector<D2>> bodies(rec.mConvexTris.size());
                SK_SECTION_STAGE(SEGMENTS);
                for (U32 k = 0; k < nids; ++k)
                {
                    const U32 t = triAt(k);
                    Seg seg;
                    if (!triangleSegment(&tris[static_cast<size_t>(t) * 3], c, seg))
                    {
                        continue;
                    }
                    const size_t body = std::upper_bound(starts.begin(), starts.end(), t) - starts.begin() - 1;
                    bodies[body].push_back({ seg.mA.mV[VX], seg.mA.mV[VY] });
                    bodies[body].push_back({ seg.mB.mV[VX], seg.mB.mV[VY] });
                }
                std::vector<LLVector2> hull;
                for (std::vector<D2>& pts : bodies)
                {
                    if (pts.size() >= 3 && hull2d(pts, hull))
                    {
                        appendLoop(s, hull);
                    }
                }
                buildGroups(s);
                return;
            }

            std::vector<Seg> segs;
            {
                SK_SECTION_STAGE(SEGMENTS);
                for (U32 k = 0; k < nids; ++k)
                {
                    Seg seg;
                    if (triangleSegment(&tris[static_cast<size_t>(triAt(k)) * 3], c, seg))
                    {
                        segs.push_back(seg);
                    }
                }
            }
            std::vector<LLVector2> nodes;
            std::vector<std::vector<S32>> closed, open;
            chain(segs, nodes, closed, open);
            std::vector<LLVector2> pts;
            for (const std::vector<S32>& path : closed)
            {
                pts.clear();
                if (path.size() >= 4)
                {
                    for (size_t k = 0; k + 1 < path.size(); ++k)
                    {
                        pts.push_back(nodes[path[k]]);
                    }
                    if (loopArea(pts.data(), static_cast<U32>(pts.size())) <= 0.f)
                    {
                        std::reverse(pts.begin(), pts.end());
                    }
                    simplifyLoop(pts);
                    if (pts.size() > SECTION_DP_MIN_POINTS)
                    {
                        douglasPeucker(pts);
                    }
                    appendLoop(s, pts);
                    continue;
                }
                // A closed path of fewer than three distinct points is a line.
                for (S32 id : path)
                {
                    pts.push_back(nodes[id]);
                }
                SliceLoop line;
                line.mFirst = static_cast<U32>(s.mPts.size());
                line.mCount = static_cast<U32>(pts.size());
                s.mPts.insert(s.mPts.end(), pts.begin(), pts.end());
                s.mLines.push_back(line);
            }
            for (const std::vector<S32>& path : open)
            {
                pts.clear();
                for (S32 id : path)
                {
                    pts.push_back(nodes[id]);
                }
                simplifyLine(pts);
                SliceLoop line;
                line.mFirst = static_cast<U32>(s.mPts.size());
                line.mCount = static_cast<U32>(pts.size());
                s.mPts.insert(s.mPts.end(), pts.begin(), pts.end());
                s.mLines.push_back(line);
            }
            SK_SECTION_STAGE(NEST);
            windByNesting(s);
        }
    }

    size_t SliceResult::bytes() const
    {
        size_t b = sizeof(SliceResult) + mPts.capacity() * sizeof(LLVector2) + (mLoops.capacity() + mLines.capacity()) * sizeof(SliceLoop)
                 + mGroups.capacity() * sizeof(SliceGroup);
        for (const SliceGroup& g : mGroups)
        {
            b += g.mHoles.capacity() * sizeof(U16);
        }
        return b;
    }

    S32 tessellationCount(F32 semi_major)
    {
        return tessellation(semi_major);
    }

    void ellipsePolygon(const LLVector2& centre, const LLVector2& v1, const LLVector2& v2, std::vector<LLVector2>& out)
    {
        ellipseD({ centre.mV[VX], centre.mV[VY] }, { v1.mV[VX], v1.mV[VY] }, { v2.mV[VX], v2.mV[VY] }, out);
    }

    F32 loopArea(const LLVector2* pts, U32 count)
    {
        F64 s = 0.0;
        for (U32 i = 0; i < count; ++i)
        {
            const LLVector2& a = pts[i];
            const LLVector2& b = pts[(i + 1) % count];
            s += static_cast<F64>(a.mV[VX]) * b.mV[VY] - static_cast<F64>(a.mV[VY]) * b.mV[VX];
        }
        return static_cast<F32>(0.5 * s);
    }

    bool loopContains(const LLVector2* pts, U32 count, const LLVector2& p)
    {
        bool inside = false;
        for (U32 i = 0, j = count - 1; i < count; j = i++)
        {
            const LLVector2& a = pts[i];
            const LLVector2& b = pts[j];
            if ((a.mV[VY] > p.mV[VY]) != (b.mV[VY] > p.mV[VY]))
            {
                const F64 x = a.mV[VX] + (static_cast<F64>(b.mV[VX]) - a.mV[VX]) * (static_cast<F64>(p.mV[VY]) - a.mV[VY])
                                             / (static_cast<F64>(b.mV[VY]) - a.mV[VY]);
                if (p.mV[VX] < x)
                {
                    inside = !inside;
                }
            }
        }
        return inside;
    }

    void windByNesting(SliceResult& s, const std::vector<bool>* active)
    {
        const size_t n = s.mLoops.size();
        std::vector<F32> area(n);
        std::vector<LLVector2> lo(n), hi(n);
        for (size_t i = 0; i < n; ++i)
        {
            const SliceLoop& l = s.mLoops[i];
            const LLVector2* p = &s.mPts[l.mFirst];
            area[i] = std::fabs(loopArea(p, l.mCount));
            lo[i].set(FLT_MAX, FLT_MAX);
            hi[i].set(-FLT_MAX, -FLT_MAX);
            for (U32 k = 0; k < l.mCount; ++k)
            {
                lo[i].set(llmin(lo[i].mV[VX], p[k].mV[VX]), llmin(lo[i].mV[VY], p[k].mV[VY]));
                hi[i].set(llmax(hi[i].mV[VX], p[k].mV[VX]), llmax(hi[i].mV[VY], p[k].mV[VY]));
            }
        }
        auto on = [&](size_t i) { return !active || (i < active->size() && (*active)[i]); };
        // Whether loop j wholly contains loop i: boxes nested, larger, every vertex inside.
        auto contains = [&](size_t j, size_t i) {
            if (!(lo[j].mV[VX] <= lo[i].mV[VX] && hi[i].mV[VX] <= hi[j].mV[VX] && lo[j].mV[VY] <= lo[i].mV[VY] && hi[i].mV[VY] <= hi[j].mV[VY]))
            {
                return false;
            }
            if (area[j] <= area[i])
            {
                return false;
            }
            const SliceLoop& outer = s.mLoops[j];
            const SliceLoop& inner = s.mLoops[i];
            for (U32 k = 0; k < inner.mCount; ++k)
            {
                if (!loopContains(&s.mPts[outer.mFirst], outer.mCount, s.mPts[inner.mFirst + k]))
                {
                    return false;
                }
            }
            return true;
        };
        for (size_t i = 0; i < n; ++i)
        {
            SliceLoop& l = s.mLoops[i];
            l.mParent = -1;
            if (!on(i))
            {
                l.mWind = 0;
                continue;
            }
            S32 depth = 0;
            for (size_t j = 0; j < n; ++j)
            {
                if (j == i || !on(j) || !contains(j, i))
                {
                    continue;
                }
                ++depth;
                if (l.mParent < 0 || area[j] < area[l.mParent])
                {
                    l.mParent = static_cast<S32>(j);
                }
            }
            l.mWind = depth % 2 == 0 ? 1 : -1;
        }
        buildGroups(s);
    }

    ZBins buildZBins(const std::vector<LLVector3>& tris)
    {
        ZBins bins;
        const size_t ntri = tris.size() / 3;
        if (ntri == 0)
        {
            return bins;
        }
        F32 zmin = FLT_MAX;
        for (const LLVector3& v : tris)
        {
            zmin = llmin(zmin, v.mV[VZ]);
        }
        bins.mZ0 = std::floor(zmin / SECTION_ZBIN_M) * SECTION_ZBIN_M;
        std::vector<std::pair<S32, S32>> span(ntri);
        S32 nb = 0;
        for (size_t t = 0; t < ntri; ++t)
        {
            const LLVector3* tri = &tris[t * 3];
            const F32 lo = llmin(tri[0].mV[VZ], llmin(tri[1].mV[VZ], tri[2].mV[VZ]));
            const F32 hi = llmax(tri[0].mV[VZ], llmax(tri[1].mV[VZ], tri[2].mV[VZ]));
            span[t] = { llmax(0, binOf(lo, bins.mZ0)), binOf(hi, bins.mZ0) };
            nb = llmax(nb, span[t].second + 1);
        }
        bins.mStart.assign(static_cast<size_t>(nb) + 1, 0);
        for (const auto& sp : span)
        {
            for (S32 b = sp.first; b <= sp.second; ++b)
            {
                bins.mStart[b + 1]++;
            }
        }
        for (S32 b = 0; b < nb; ++b)
        {
            bins.mStart[b + 1] += bins.mStart[b];
        }
        bins.mTris.resize(bins.mStart[nb]);
        std::vector<U32> fill(bins.mStart.begin(), bins.mStart.end() - 1);
        for (size_t t = 0; t < ntri; ++t)
        {
            for (S32 b = span[t].first; b <= span[t].second; ++b)
            {
                bins.mTris[fill[b]++] = static_cast<U32>(t);
            }
        }
        return bins;
    }

    SliceResult sliceRecord(const sknav::Record& rec, F32 c, const ZBins* bins)
    {
        SliceResult s;
        if (rec.mShape == sknav::Shape::TRIANGLES)
        {
            s.mSheet = !rec.mSolid;
            sliceSoup(rec, c, bins, s);
            return s;
        }
        SK_SECTION_STAGE(SHAPE);
        thread_local std::vector<LLVector2> loop;
        loop.clear();
        bool made = false;
        switch (rec.mShape)
        {
            case sknav::Shape::BOX: made = sliceBox(rec, c, loop); break;
            case sknav::Shape::SPHERE: made = sliceEllipsoid(rec, c, loop); break;
            case sknav::Shape::CYLINDER: made = sliceCylinder(rec, c, loop); break;
            case sknav::Shape::TRIANGLES: break;
        }
        if (made && std::fabs(loopArea(loop.data(), static_cast<U32>(loop.size()))) > 1e-12f)
        {
            appendLoop(s, loop);
            buildGroups(s);
        }
        return s;
    }

    namespace
    {
        /// Every subsample a segment passes through, by grid traversal, clipped to the mask first.
        void stampSegment(F64 ax, F64 ay, F64 bx, F64 by, F64 x0, F64 y0, F64 sub, S32 w, S32 h, std::vector<U8>& mask)
        {
            F64 gx0 = (ax - x0) / sub, gy0 = (ay - y0) / sub, gx1 = (bx - x0) / sub, gy1 = (by - y0) / sub;
            // Liang-Barsky against the mask grown by a subsample.
            F64 t0 = 0.0, t1 = 1.0;
            const F64 dx = gx1 - gx0, dy = gy1 - gy0;
            const F64 p[4] = { -dx, dx, -dy, dy };
            const F64 q[4] = { gx0 + 1.0, w + 1.0 - gx0, gy0 + 1.0, h + 1.0 - gy0 };
            for (S32 k = 0; k < 4; ++k)
            {
                if (p[k] == 0.0)
                {
                    if (q[k] < 0.0)
                    {
                        return;
                    }
                    continue;
                }
                const F64 r = q[k] / p[k];
                if (p[k] < 0.0)
                {
                    t0 = llmax(t0, r);
                }
                else
                {
                    t1 = llmin(t1, r);
                }
            }
            if (t0 > t1)
            {
                return;
            }
            const F64 sx = gx0 + dx * t0, sy = gy0 + dy * t0, ex = gx0 + dx * t1, ey = gy0 + dy * t1;
            S64 i = static_cast<S64>(std::floor(sx)), j = static_cast<S64>(std::floor(sy));
            const S64 iend = static_cast<S64>(std::floor(ex)), jend = static_cast<S64>(std::floor(ey));
            const S64 step_x = ex > sx ? 1 : ex < sx ? -1 : 0, step_y = ey > sy ? 1 : ey < sy ? -1 : 0;
            const F64 ddx = std::fabs(ex - sx), ddy = std::fabs(ey - sy);
            const F64 delta_x = step_x ? 1.0 / ddx : DBL_MAX, delta_y = step_y ? 1.0 / ddy : DBL_MAX;
            F64 max_x = step_x > 0 ? (i + 1 - sx) / ddx : step_x < 0 ? (sx - i) / ddx : DBL_MAX;
            F64 max_y = step_y > 0 ? (j + 1 - sy) / ddy : step_y < 0 ? (sy - j) / ddy : DBL_MAX;
            auto set = [&](S64 x, S64 y) {
                if (x >= 0 && x < w && y >= 0 && y < h)
                {
                    mask[static_cast<size_t>(y) * w + x] = 1;
                }
            };
            S64 budget = std::llabs(iend - i) + std::llabs(jend - j) + 2;
            set(i, j);
            while ((i != iend || j != jend) && budget-- > 0)
            {
                if (max_x < max_y)
                {
                    i += step_x;
                    max_x += delta_x;
                }
                else
                {
                    j += step_y;
                    max_y += delta_y;
                }
                set(i, j);
            }
            set(iend, jend);
        }

        void stampPolyline(const LLVector2* pts, U32 count, bool closed, const LLVector2& shift, F64 x0, F64 y0, F64 sub, S32 w, S32 h,
                           std::vector<U8>& mask)
        {
            if (count == 0)
            {
                return;
            }
            const U32 edges = closed ? count : count - 1;
            for (U32 k = 0; k < edges; ++k)
            {
                const LLVector2& a = pts[k];
                const LLVector2& b = pts[(k + 1) % count];
                stampSegment(static_cast<F64>(a.mV[VX]) + shift.mV[VX], static_cast<F64>(a.mV[VY]) + shift.mV[VY],
                             static_cast<F64>(b.mV[VX]) + shift.mV[VX], static_cast<F64>(b.mV[VY]) + shift.mV[VY], x0, y0, sub, w, h, mask);
            }
            if (count == 1)
            {
                const F64 x = pts[0].mV[VX] + static_cast<F64>(shift.mV[VX]), y = pts[0].mV[VY] + static_cast<F64>(shift.mV[VY]);
                stampSegment(x, y, x, y, x0, y0, sub, w, h, mask);
            }
        }

        size_t binsBytes(const ZBins& b)
        {
            return sizeof(ZBins) + (b.mStart.capacity() + b.mTris.capacity()) * sizeof(U32);
        }
    }

    void fillCoverage(const SliceResult& s, const std::vector<U16>& loops, const std::vector<S8>& winds, bool lines, const LLVector2& shift,
                      F32 x0, F32 y0, F32 sub, S32 w, S32 h, std::vector<U8>& mask)
    {
        if (w <= 0 || h <= 0 || sub <= 0.f)
        {
            return;
        }
        if (mask.size() < static_cast<size_t>(w) * h)
        {
            mask.resize(static_cast<size_t>(w) * h, 0);
        }
        const F64 sx = shift.mV[VX], sy = shift.mV[VY];
        // Scanline over the loops' rows: the winding sum at each subsample centre, half-open rows y0 <= y < y1.
        F64 ymin = DBL_MAX, ymax = -DBL_MAX;
        for (U16 id : loops)
        {
            const SliceLoop& l = s.mLoops[id];
            for (U32 k = 0; k < l.mCount; ++k)
            {
                const F64 y = s.mPts[l.mFirst + k].mV[VY] + sy;
                ymin = llmin(ymin, y);
                ymax = llmax(ymax, y);
            }
        }
        if (ymin <= ymax)
        {
            const S32 j0 = llmax(0, static_cast<S32>(std::ceil((ymin - y0) / sub - 0.5)));
            const S32 j1 = llmin(h - 1, static_cast<S32>(std::ceil((ymax - y0) / sub - 0.5)) - 1);
            if (j0 <= j1)
            {
                struct Crossing
                {
                    F64 mX;
                    S32 mW;
                    bool operator<(const Crossing& o) const { return mX < o.mX; }
                };
                std::vector<std::vector<Crossing>> rows(static_cast<size_t>(j1 - j0 + 1));
                for (size_t n = 0; n < loops.size(); ++n)
                {
                    const SliceLoop& l = s.mLoops[loops[n]];
                    const S32 wind = n < winds.size() ? winds[n] : 1;
                    if (!wind)
                    {
                        continue;
                    }
                    for (U32 k = 0; k < l.mCount; ++k)
                    {
                        const LLVector2& pa = s.mPts[l.mFirst + k];
                        const LLVector2& pb = s.mPts[l.mFirst + (k + 1) % l.mCount];
                        const F64 ax = pa.mV[VX] + sx, ay = pa.mV[VY] + sy, bx = pb.mV[VX] + sx, by = pb.mV[VY] + sy;
                        if (ay == by)
                        {
                            continue;
                        }
                        const bool up = by > ay;
                        const F64 lo = up ? ay : by, hi = up ? by : ay;
                        // Rows whose centre lies in [lo, hi).
                        const S32 ja = llmax(j0, static_cast<S32>(std::ceil((lo - y0) / sub - 0.5)));
                        const S32 jb = llmin(j1, static_cast<S32>(std::ceil((hi - y0) / sub - 0.5)) - 1);
                        for (S32 j = ja; j <= jb; ++j)
                        {
                            const F64 yc = y0 + (j + 0.5) * sub;
                            if (yc < lo || yc >= hi)
                            {
                                continue;
                            }
                            const F64 x = ax + (yc - ay) * (bx - ax) / (by - ay);
                            rows[j - j0].push_back({ x, up ? -wind : wind });   // swept from the left: a downward edge enters a CCW loop
                        }
                    }
                }
                for (S32 j = j0; j <= j1; ++j)
                {
                    std::vector<Crossing>& row = rows[j - j0];
                    if (row.size() < 2)
                    {
                        continue;
                    }
                    std::sort(row.begin(), row.end());
                    S32 sum = 0;
                    for (size_t k = 0; k + 1 < row.size(); ++k)
                    {
                        sum += row[k].mW;
                        if (sum <= 0)
                        {
                            continue;
                        }
                        // Subsamples whose centre lies in [x_k, x_k+1).
                        const S32 ia = llmax(0, static_cast<S32>(std::ceil((row[k].mX - x0) / sub - 0.5)));
                        const S32 ib = llmin(w - 1, static_cast<S32>(std::ceil((row[k + 1].mX - x0) / sub - 0.5)) - 1);
                        for (S32 i = ia; i <= ib; ++i)
                        {
                            mask[static_cast<size_t>(j) * w + i] = 1;
                        }
                    }
                }
            }
        }
        // Conservative edges: a sheet thinner than a subsample never vanishes between samples.
        for (U16 id : loops)
        {
            const SliceLoop& l = s.mLoops[id];
            stampPolyline(&s.mPts[l.mFirst], l.mCount, true, shift, x0, y0, sub, w, h, mask);
        }
        if (lines)
        {
            for (const SliceLoop& l : s.mLines)
            {
                stampPolyline(&s.mPts[l.mFirst], l.mCount, false, shift, x0, y0, sub, w, h, mask);
            }
        }
    }

    void mergeLevels(std::vector<std::pair<S32, U32>>& samples, std::vector<S32>& out)
    {
        out.clear();
        if (samples.empty())
        {
            return;
        }
        std::sort(samples.begin(), samples.end());
        if (samples.front().first == samples.back().first)
        {
            out.push_back(samples.front().first);   // one level, the common case
            return;
        }
        struct Run
        {
            S32 mLowest, mBest;
            U32 mBestCount, mTotal;
        };
        // Called once per level cell, so the runs live in scratch kept per thread.
        thread_local std::vector<Run> scratch;
        std::vector<Run>& runs = scratch;
        runs.clear();
        // Equal levels first add up, then join the current run.
        for (size_t i = 0; i < samples.size();)
        {
            const S32 level = samples[i].first;
            U32 count = 0;
            for (; i < samples.size() && samples[i].first == level; ++i)
            {
                count += samples[i].second;
            }
            if (!runs.empty() && level - runs.back().mLowest <= LEVEL_MERGE_CELLS)
            {
                Run& r = runs.back();
                r.mTotal += count;
                if (count > r.mBestCount)
                {
                    r.mBest = level;
                    r.mBestCount = count;
                }
                continue;
            }
            runs.push_back({ level, level, count, count });
        }
        // No two runs share a best level, so the order is total and needs no stable sort.
        const size_t keep = llmin(runs.size(), static_cast<size_t>(LEVEL_MAX));
        std::partial_sort(runs.begin(), runs.begin() + keep, runs.end(), [](const Run& a, const Run& b) {
            return a.mTotal != b.mTotal ? a.mTotal > b.mTotal : a.mBest < b.mBest;
        });
        for (size_t i = 0; i < keep; ++i)
        {
            out.push_back(runs[i].mBest);
        }
        std::sort(out.begin(), out.end());
    }

    bool isIsland(const LLVector2* outer, U32 count, F32 thickness, const std::function<bool(const LLVector2&, F32&)>& floor_at)
    {
        if (thickness >= ISLAND_MAX_THICKNESS_M || count < 3)
        {
            return false;
        }
        // A point inside: the centroid, or the middle of the longest inside run across the box's middle.
        F64 a2 = 0.0, cx = 0.0, cy = 0.0;
        LLVector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
        for (U32 i = 0; i < count; ++i)
        {
            const LLVector2& p = outer[i];
            const LLVector2& q = outer[(i + 1) % count];
            const F64 t = static_cast<F64>(p.mV[VX]) * q.mV[VY] - static_cast<F64>(q.mV[VX]) * p.mV[VY];
            a2 += t;
            cx += (static_cast<F64>(p.mV[VX]) + q.mV[VX]) * t;
            cy += (static_cast<F64>(p.mV[VY]) + q.mV[VY]) * t;
            lo.set(llmin(lo.mV[VX], p.mV[VX]), llmin(lo.mV[VY], p.mV[VY]));
            hi.set(llmax(hi.mV[VX], p.mV[VX]), llmax(hi.mV[VY], p.mV[VY]));
        }
        if (std::fabs(a2) < 1e-12)
        {
            return false;
        }
        LLVector2 rep(static_cast<F32>(cx / (3.0 * a2)), static_cast<F32>(cy / (3.0 * a2)));
        if (!loopContains(outer, count, rep))
        {
            const F64 y = 0.5 * (static_cast<F64>(lo.mV[VY]) + hi.mV[VY]);
            std::vector<F64> xs;
            for (U32 i = 0; i < count; ++i)
            {
                const LLVector2& p = outer[i];
                const LLVector2& q = outer[(i + 1) % count];
                if ((p.mV[VY] > y) != (q.mV[VY] > y))
                {
                    xs.push_back(p.mV[VX] + (y - p.mV[VY]) * (static_cast<F64>(q.mV[VX]) - p.mV[VX]) / (static_cast<F64>(q.mV[VY]) - p.mV[VY]));
                }
            }
            std::sort(xs.begin(), xs.end());
            F64 best = -1.0;
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                if (xs[k + 1] - xs[k] > best)
                {
                    best = xs[k + 1] - xs[k];
                    rep.set(static_cast<F32>(0.5 * (xs[k] + xs[k + 1])), static_cast<F32>(y));
                }
            }
            if (best < 0.0)
            {
                return false;
            }
        }
        F32 top = 0.f;
        if (!floor_at(rep, top))
        {
            return false;
        }

        // The ring: samples along each edge pushed out along its normal, and one out along each convex corner's
        // bisector; at least eight in all.
        const bool ccw = a2 > 0.0;
        F64 perimeter = 0.0;
        S32 convex = 0;
        for (U32 i = 0; i < count; ++i)
        {
            perimeter += (outer[(i + 1) % count] - outer[i]).length();
            const LLVector2 e0 = outer[i] - outer[(i + count - 1) % count];
            const LLVector2 e1 = outer[(i + 1) % count] - outer[i];
            const F32 turn = e0.mV[VX] * e1.mV[VY] - e0.mV[VY] * e1.mV[VX];
            convex += (ccw ? turn > 0.f : turn < 0.f) ? 1 : 0;
        }
        F64 step = ISLAND_SAMPLE_M;
        S32 total = convex;
        for (U32 i = 0; i < count; ++i)
        {
            total += llmax(1, static_cast<S32>(std::ceil((outer[(i + 1) % count] - outer[i]).length() / step)));
        }
        if (total < 8)
        {
            step = perimeter / 8.0;
        }
        auto outward = [&](const LLVector2& e) {
            LLVector2 n = ccw ? LLVector2(e.mV[VY], -e.mV[VX]) : LLVector2(-e.mV[VY], e.mV[VX]);
            n.normVec();
            return n;
        };
        S32 found = 0, lower = 0;
        auto sample = [&](const LLVector2& p) {
            F32 z = 0.f;
            if (!floor_at(p, z))
            {
                return true;
            }
            if (z > top + ISLAND_HIGHER_M)
            {
                return false;   // a higher floor beside it: a tread, or a crate against a taller one
            }
            ++found;
            lower += z <= top - ISLAND_STEP_M ? 1 : 0;
            return true;
        };
        for (U32 i = 0; i < count; ++i)
        {
            const LLVector2& a = outer[i];
            const LLVector2& b = outer[(i + 1) % count];
            const LLVector2 e = b - a;
            const F32 len = e.length();
            if (len < 1e-6f)
            {
                continue;
            }
            const LLVector2 dir = outward(e);
            const LLVector2 n = dir * ISLAND_OUT_M;
            const S32 k = llmax(1, static_cast<S32>(std::ceil(len / step)));
            for (S32 s = 0; s < k; ++s)
            {
                const LLVector2 on = a + e * ((s + 0.5f) / k);
                if (!sample(on + n))
                {
                    return false;
                }
                // A floor that runs on across the outline is a slope, not an obstacle's top.
                F32 z_in = top, z_out = 0.f;
                floor_at(on - dir * llmin(ISLAND_EDGE_M, 0.5f * thickness), z_in);
                if (floor_at(on + dir * ISLAND_EDGE_M, z_out) && z_out > z_in - ISLAND_STEP_M)
                {
                    return false;
                }
            }
            const LLVector2 e_next = outer[(i + 2) % count] - b;
            const F32 turn = e.mV[VX] * e_next.mV[VY] - e.mV[VY] * e_next.mV[VX];
            if (ccw ? turn > 0.f : turn < 0.f)
            {
                LLVector2 bis = outward(e) + outward(e_next);
                if (bis.normVec() > 1e-6f && !sample(b + bis * ISLAND_OUT_M))
                {
                    return false;
                }
            }
        }
        return found >= 3 && lower >= ISLAND_SHARE * found;
    }

    SectionCache::SectionCache(size_t budget)
        : mBudget(budget)
    {
    }

    void SectionCache::touch(Entry& e)
    {
        mLru.splice(mLru.begin(), mLru, e.mLru);
    }

    void SectionCache::insert(const Key& key, Entry e)
    {
        mLru.push_front(key);
        e.mLru = mLru.begin();
        mBytes += e.mBytes;
        if (e.mSlice)
        {
            ++mSlices;
        }
        mEntries[key] = std::move(e);
    }

    void SectionCache::evict(const Key& keep_a, const Key& keep_b)
    {
        auto it = mLru.end();
        while (mBytes > mBudget && it != mLru.begin())
        {
            --it;
            if (*it == keep_a || *it == keep_b)
            {
                continue;
            }
            const auto entry = mEntries.find(*it);
            mBytes -= entry->second.mBytes;
            if (entry->second.mSlice)
            {
                --mSlices;
            }
            mEntries.erase(entry);
            it = mLru.erase(it);
        }
    }

    SlicePtr SectionCache::get(const sknav::Record& rec, S32 q, bool* hit)
    {
        const Key key = { rec.mSignature, static_cast<U8>(rec.mShape), q };
        const auto found = mEntries.find(key);
        if (found != mEntries.end())
        {
            touch(found->second);
            if (hit)
            {
                *hit = true;
            }
            return found->second.mSlice;
        }
        if (hit)
        {
            *hit = false;
        }
        Key bins_key = key;
        bins_key.mPlane = INT_MIN;
        std::shared_ptr<const ZBins> bins;
        if (rec.mShape == sknav::Shape::TRIANGLES && rec.mTriangles && rec.mTriangles->size() / 3 > SECTION_ZBIN_MIN_TRIS)
        {
            const auto b = mEntries.find(bins_key);
            if (b != mEntries.end())
            {
                touch(b->second);
                bins = b->second.mBins;
            }
            else
            {
                SK_SECTION_STAGE(ZBINS);
                bins = std::make_shared<const ZBins>(buildZBins(*rec.mTriangles));
                Entry e;
                e.mBins = bins;
                e.mBytes = binsBytes(*bins);
                insert(bins_key, std::move(e));
            }
        }
        const F32 c = sectionPlaneZ(q) - static_cast<F32>(rec.mOrigin.mdV[VZ]);
        auto slice = std::make_shared<SliceResult>(sliceRecord(rec, c, bins.get()));
        Entry e;
        e.mBytes = slice->bytes();
        e.mSlice = slice;
        insert(key, std::move(e));
        evict(key, bins_key);
        return slice;
    }
}
