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
#include <cstring>
#include <deque>
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

        F64 signedArea(const std::vector<D2>& p)
        {
            F64 s = 0.0;
            for (size_t i = 0; i < p.size(); ++i)
            {
                const D2& a = p[i];
                const D2& b = p[(i + 1) % p.size()];
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

        /// Monotone chain, counter-clockwise; false when fewer than three distinct points remain.
        bool hull2d(std::vector<D2> pts, std::vector<LLVector2>& out)
        {
            out.clear();
            std::sort(pts.begin(), pts.end());
            pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
            if (pts.size() < 3)
            {
                return false;
            }
            std::vector<D2> lower, upper;
            for (const D2& p : pts)
            {
                while (lower.size() >= 2 && cross(lower[lower.size() - 2], lower.back(), p) <= 0.0)
                {
                    lower.pop_back();
                }
                lower.push_back(p);
            }
            for (auto it = pts.rbegin(); it != pts.rend(); ++it)
            {
                while (upper.size() >= 2 && cross(upper[upper.size() - 2], upper.back(), *it) <= 0.0)
                {
                    upper.pop_back();
                }
                upper.push_back(*it);
            }
            std::vector<D2> h(lower.begin(), lower.end() - 1);
            h.insert(h.end(), upper.begin(), upper.end() - 1);
            if (h.size() < 3 || std::fabs(signedArea(h)) < 1e-12)
            {
                return false;
            }
            for (const D2& p : h)
            {
                out.emplace_back(static_cast<F32>(p.x), static_cast<F32>(p.y));
            }
            return true;
        }

        /// A convex polytope's section: each straddling edge's crossing, taken from its lower end, then the hull.
        bool sliceEdges(const std::vector<D3>& v, const std::vector<std::pair<S32, S32>>& edges, F64 c, std::vector<LLVector2>& out)
        {
            std::vector<D2> pts;
            for (const auto& e : edges)
            {
                const D3& p = v[e.first];
                const D3& q = v[e.second];
                if ((p.z >= c) != (q.z >= c))
                {
                    const D3& lo = p.z < q.z ? p : q;
                    const D3& hi = p.z < q.z ? q : p;
                    const F64 t = (c - lo.z) / (hi.z - lo.z);
                    pts.push_back({ lo.x + t * (hi.x - lo.x), lo.y + t * (hi.y - lo.y) });
                }
            }
            return pts.size() >= 3 && hull2d(pts, out);
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
            static std::vector<std::pair<S32, S32>> edges;
            if (edges.empty())
            {
                for (S32 i = 0; i < 8; ++i)
                {
                    for (S32 b = 0; b < 3; ++b)
                    {
                        const S32 j = i ^ (1 << b);
                        if (i < j)
                        {
                            edges.emplace_back(i, j);
                        }
                    }
                }
            }
            std::vector<D3> v(8);
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
            return sliceEdges(v, edges, c, out);
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
                std::vector<D2> pts;
                for (S32 sign : { 1, -1 })
                {
                    const F64 qu = bx + sign * px, qv = by + sign * py;
                    for (S32 sw : { -1, 1 })
                    {
                        pts.push_back({ centre.x + qu * c0.x + qv * c1.x + sw * c2.x, centre.y + qu * c0.y + qv * c1.y + sw * c2.y });
                    }
                }
                return hull2d(pts, out);
            }
            // Tilted: the inscribed prism sliced like a box; the section stretches by 1 / |a2.z| and so does the bound.
            const S32 n = tessellation(llmax(r.mHalf.mV[0], r.mHalf.mV[1]) / llmax(std::fabs(static_cast<F64>(r.mAxes[2].mV[VZ])), 0.2));
            std::vector<D3> v(2 * n);
            for (S32 i = 0; i < n; ++i)
            {
                const F64 t = 2.0 * PI_64 * i / n;
                const F64 ct = std::cos(t), st = std::sin(t);
                const D3 ring = { centre.x + ct * c0.x + st * c1.x, centre.y + ct * c0.y + st * c1.y, centre.z + ct * c0.z + st * c1.z };
                v[i] = { ring.x - c2.x, ring.y - c2.y, ring.z - c2.z };
                v[n + i] = { ring.x + c2.x, ring.y + c2.y, ring.z + c2.z };
            }
            std::vector<std::pair<S32, S32>> edges;
            for (S32 i = 0; i < n; ++i)
            {
                edges.emplace_back(i, (i + 1) % n);
            }
            for (S32 i = 0; i < n; ++i)
            {
                edges.emplace_back(n + i, n + (i + 1) % n);
            }
            for (S32 i = 0; i < n; ++i)
            {
                edges.emplace_back(i, n + i);
            }
            return sliceEdges(v, edges, c, out);
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

        /// Segments into closed paths (front == back) and open ones: exact ends, a 1 mm snap, then a 2 cm close nearest first.
        void chain(const std::vector<Seg>& segs, std::vector<LLVector2>& nodes, std::vector<std::vector<S32>>& closed,
                   std::vector<std::vector<S32>>& open)
        {
            std::unordered_map<U64, S32> keys;
            auto node = [&](const LLVector2& p) {
                const auto it = keys.emplace(pointKey(p), static_cast<S32>(nodes.size()));
                if (it.second)
                {
                    nodes.push_back(p);
                }
                return it.first->second;
            };
            std::vector<std::pair<S32, S32>> ends;
            ends.reserve(segs.size());
            for (const Seg& s : segs)
            {
                const S32 a = node(s.mA);
                const S32 b = node(s.mB);
                ends.emplace_back(a, b);
            }
            // Nodes in neighbouring 1 mm cells within the tolerance join the lowest id.
            std::vector<S32> parent(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                parent[i] = static_cast<S32>(i);
            }
            std::map<GridKey, std::vector<S32>> grid;
            for (size_t i = 0; i < nodes.size(); ++i)
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
                            if (dist(nodes[j], nodes[i]) <= SECTION_SNAP_M)
                            {
                                const S32 a = findRoot(parent, static_cast<S32>(i)), b = findRoot(parent, j);
                                if (a != b)
                                {
                                    parent[llmax(a, b)] = llmin(a, b);
                                }
                            }
                        }
                    }
                }
                grid[g].push_back(static_cast<S32>(i));
            }
            std::vector<std::pair<S32, S32>> joined;
            joined.reserve(ends.size());
            for (const auto& e : ends)
            {
                const S32 a = findRoot(parent, e.first), b = findRoot(parent, e.second);
                if (a != b)
                {
                    joined.emplace_back(a, b);
                }
            }
            std::vector<std::vector<S32>> incident(nodes.size());
            for (size_t s = 0; s < joined.size(); ++s)
            {
                incident[joined[s].first].push_back(static_cast<S32>(s));
                incident[joined[s].second].push_back(static_cast<S32>(s));
            }
            std::vector<size_t> next_free(nodes.size(), 0);
            std::vector<bool> used(joined.size(), false);
            auto nextAt = [&](S32 tip) -> S32 {
                std::vector<S32>& list = incident[tip];
                size_t& k = next_free[tip];
                while (k < list.size() && used[list[k]])
                {
                    ++k;
                }
                return k < list.size() ? list[k] : -1;
            };
            for (size_t s = 0; s < joined.size(); ++s)
            {
                if (used[s])
                {
                    continue;
                }
                used[s] = true;
                std::deque<S32> path = { joined[s].first, joined[s].second };
                bool is_closed = false;
                for (S32 direction : { 1, -1 })
                {
                    while (!is_closed)
                    {
                        const S32 tip = direction == 1 ? path.back() : path.front();
                        if (direction == 1 && path.size() > 2 && path.back() == path.front())
                        {
                            is_closed = true;
                            break;
                        }
                        const S32 nxt = nextAt(tip);
                        if (nxt < 0)
                        {
                            break;
                        }
                        used[nxt] = true;
                        const S32 other = joined[nxt].first == tip ? joined[nxt].second : joined[nxt].first;
                        if (direction == 1)
                        {
                            path.push_back(other);
                        }
                        else
                        {
                            path.push_front(other);
                        }
                        if (path.front() == path.back())
                        {
                            is_closed = true;
                        }
                    }
                    if (is_closed)
                    {
                        break;
                    }
                }
                (is_closed ? closed : open).emplace_back(path.begin(), path.end());
            }

            // Close what is left within 2 cm, nearest first; ties by the lower polyline index, then in the order the
            // ends are tried: tail to head, head to tail, tail to tail, head to head.
            while (!open.empty())
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
                bool found = false;
                F64 best_d = 0.0;
                S32 best_i = 0, best_j = 0, best_how = 0;     // how: 0 self, 1 ij, 2 ji, 3 ijr, 4 rij
                auto consider = [&](F64 d, S32 i, S32 j, S32 how) {
                    if (d > SECTION_CLOSE_M)
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
                for (size_t i = 0; i < open.size(); ++i)
                {
                    const S32 si = static_cast<S32>(i);
                    for (bool head : { true, false })
                    {
                        const LLVector2& p = nodes[head ? open[i].front() : open[i].back()];
                        const GridKey g = gridOf(p, SECTION_CLOSE_M);
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
                                    if (e.mPath < si || (e.mPath == si && e.mHead == head))
                                    {
                                        continue;
                                    }
                                    const LLVector2& q = nodes[e.mHead ? open[e.mPath].front() : open[e.mPath].back()];
                                    const F64 d = dist(p, q);
                                    if (e.mPath == si)
                                    {
                                        if (!head)
                                        {
                                            consider(d, si, si, 0);
                                        }
                                        continue;
                                    }
                                    // This path's end against a later path's end.
                                    const S32 how = !head && e.mHead ? 1 : head && !e.mHead ? 2 : !head ? 3 : 4;
                                    consider(d, si, e.mPath, how);
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
                std::vector<S32> pi = open[best_i], pj = open[best_j];
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
                for (const std::vector<D2>& pts : bodies)
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
            for (U32 k = 0; k < nids; ++k)
            {
                Seg seg;
                if (triangleSegment(&tris[static_cast<size_t>(triAt(k)) * 3], c, seg))
                {
                    segs.push_back(seg);
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
        std::vector<LLVector2> loop;
        bool made = false;
        switch (rec.mShape)
        {
            case sknav::Shape::BOX: made = sliceBox(rec, c, loop); break;
            case sknav::Shape::SPHERE: made = sliceEllipsoid(rec, c, loop); break;
            case sknav::Shape::CYLINDER: made = sliceCylinder(rec, c, loop); break;
            case sknav::Shape::TRIANGLES:
                s.mSheet = !rec.mSolid;
                sliceSoup(rec, c, bins, s);
                return s;
        }
        if (made && std::fabs(loopArea(loop.data(), static_cast<U32>(loop.size()))) > 1e-12f)
        {
            appendLoop(s, loop);
            buildGroups(s);
        }
        return s;
    }
}
