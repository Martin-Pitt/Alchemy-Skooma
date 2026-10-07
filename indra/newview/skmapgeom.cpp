/**
 * @file skmapgeom.cpp
 * @brief The mini-map's scene: navmesh polygons stitched into surfaces, components and layers.
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

#include "skmapgeom.h"

#include "sknavbuild.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>

namespace skmap
{
    namespace
    {
        typedef std::pair<S32, S32> Cell;

        struct UnionFind
        {
            std::vector<S32> mParent;
            explicit UnionFind(size_t n) : mParent(n)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    mParent[i] = static_cast<S32>(i);
                }
            }
            S32 find(S32 a)
            {
                while (mParent[a] != a)
                {
                    mParent[a] = mParent[mParent[a]];
                    a = mParent[a];
                }
                return a;
            }
            // The smaller root wins, so the result does not depend on the order of unions.
            void unite(S32 a, S32 b)
            {
                a = find(a);
                b = find(b);
                if (a == b)
                {
                    return;
                }
                if (a < b)
                {
                    mParent[b] = a;
                }
                else
                {
                    mParent[a] = b;
                }
            }
        };

        F32 cross2(const LLVector2& o, const LLVector2& a, const LLVector2& b)
        {
            return (a.mV[VX] - o.mV[VX]) * (b.mV[VY] - o.mV[VY]) - (a.mV[VY] - o.mV[VY]) * (b.mV[VX] - o.mV[VX]);
        }

        LLVector2 plan(const LLVector3& v)
        {
            return LLVector2(v.mV[VX], v.mV[VY]);
        }

        /// Area-weighted median of (value, weight) pairs; ties by value.
        F32 weightedMedian(std::vector<std::pair<F32, F32>> samples)
        {
            if (samples.empty())
            {
                return 0.f;
            }
            std::sort(samples.begin(), samples.end());
            F32 total = 0.f;
            for (const auto& s : samples)
            {
                total += llmax(s.second, 1e-6f);
            }
            F32 run = 0.f;
            for (const auto& s : samples)
            {
                run += llmax(s.second, 1e-6f);
                if (run >= total * 0.5f)
                {
                    return s.first;
                }
            }
            return samples.back().first;
        }

        /// Separating axes: a convex polygon against an axis-aligned rectangle, in plan.
        bool polyMeetsRect(const ScenePoly& poly, F32 x0, F32 y0, F32 x1, F32 y1)
        {
            if (poly.mMaxX < x0 || poly.mMinX > x1 || poly.mMaxY < y0 || poly.mMinY > y1)
            {
                return false;
            }
            const LLVector2 corners[4] = { LLVector2(x0, y0), LLVector2(x1, y0), LLVector2(x1, y1), LLVector2(x0, y1) };
            const S32 n = poly.mVertCount;
            for (S32 i = 0; i < n; ++i)
            {
                const LLVector2 a = plan(poly.mVerts[i]);
                const LLVector2 b = plan(poly.mVerts[(i + 1) % n]);
                const LLVector2 edge = b - a;
                const LLVector2 normal(-edge.mV[VY], edge.mV[VX]);
                F32 pmin = FLT_MAX, pmax = -FLT_MAX;
                for (S32 k = 0; k < n; ++k)
                {
                    const F32 d = normal * plan(poly.mVerts[k]);
                    pmin = llmin(pmin, d);
                    pmax = llmax(pmax, d);
                }
                F32 rmin = FLT_MAX, rmax = -FLT_MAX;
                for (const LLVector2& c : corners)
                {
                    const F32 d = normal * c;
                    rmin = llmin(rmin, d);
                    rmax = llmax(rmax, d);
                }
                if (rmax < pmin || rmin > pmax)
                {
                    return false;
                }
            }
            return true;
        }

        /// The plan distance from a point to a convex polygon, 0 inside.
        F32 planDistance(const Scene& scene, const ScenePoly& poly, const LLVector2& p)
        {
            if (polyContains(scene, poly, p))
            {
                return 0.f;
            }
            F32 best = FLT_MAX;
            for (S32 i = 0; i < poly.mVertCount; ++i)
            {
                const LLVector2 a = plan(poly.mVerts[i]);
                const LLVector2 b = plan(poly.mVerts[(i + 1) % poly.mVertCount]);
                const LLVector2 ab = b - a;
                const F32 len2 = ab * ab;
                const F32 t = len2 > 0.f ? llclamp(((p - a) * ab) / len2, 0.f, 1.f) : 0.f;
                best = llmin(best, (a + ab * t - p).length());
            }
            return best;
        }

        Cell cellOfLocal(const Scene& scene, const LLVector2& p)
        {
            return indexCell(scene.mOrigin.mdV[VX] + p.mV[VX], scene.mOrigin.mdV[VY] + p.mV[VY]);
        }

        /// The cells within reach of a scene-local point, row by row.
        void cellsAround(const Scene& scene, const LLVector2& p, F32 reach, std::vector<Cell>& out)
        {
            out.clear();
            const Cell lo = cellOfLocal(scene, p - LLVector2(reach, reach));
            const Cell hi = cellOfLocal(scene, p + LLVector2(reach, reach));
            for (S32 y = lo.second; y <= hi.second; ++y)
            {
                for (S32 x = lo.first; x <= hi.first; ++x)
                {
                    out.emplace_back(x, y);
                }
            }
        }

        /// The cells a polygon meets.
        void polyCells(const Scene& scene, const ScenePoly& poly, std::vector<Cell>& out)
        {
            out.clear();
            const Cell lo = cellOfLocal(scene, LLVector2(poly.mMinX, poly.mMinY));
            const Cell hi = cellOfLocal(scene, LLVector2(poly.mMaxX, poly.mMaxY));
            for (S32 y = lo.second; y <= hi.second; ++y)
            {
                for (S32 x = lo.first; x <= hi.first; ++x)
                {
                    const F32 x0 = static_cast<F32>(x * INDEX_CELL_M - scene.mOrigin.mdV[VX]);
                    const F32 y0 = static_cast<F32>(y * INDEX_CELL_M - scene.mOrigin.mdV[VY]);
                    if (polyMeetsRect(poly, x0, y0, x0 + INDEX_CELL_M, y0 + INDEX_CELL_M))
                    {
                        out.emplace_back(x, y);
                    }
                }
            }
        }

        /// The polygon nearest a global point within reach in plan, closest in height.
        S32 polyNear(const Scene& scene, const LLVector3d& pos, F32 reach)
        {
            const LLVector2 p = scene.toLocal(pos);
            const F32 z = static_cast<F32>(pos.mdV[VZ]);
            std::vector<Cell> cells;
            cellsAround(scene, p, reach, cells);
            S32 best = -1;
            F32 best_dz = FLT_MAX;
            for (const Cell& c : cells)
            {
                const auto it = scene.mIndex.find(c);
                if (it == scene.mIndex.end())
                {
                    continue;
                }
                for (U32 id : it->second)
                {
                    const ScenePoly& poly = scene.mPolys[id];
                    if (planDistance(scene, poly, p) > reach)
                    {
                        continue;
                    }
                    const F32 dz = std::fabs(poly.mZMean - z);
                    if (dz < best_dz || (dz == best_dz && static_cast<S32>(id) < best))
                    {
                        best_dz = dz;
                        best = static_cast<S32>(id);
                    }
                }
            }
            return best;
        }

        struct BorderEdge
        {
            U32 mPoly = 0;
            F32 mLo = 0.f, mHi = 0.f;      ///< along the line
            F32 mZLo = 0.f, mZHi = 0.f;    ///< heights at those ends
            S8 mSide = 0;                  ///< which side of the line the polygon lies on
            bool operator<(const BorderEdge& o) const { return mLo != o.mLo ? mLo < o.mLo : mPoly < o.mPoly; }
            F32 zAt(F32 t) const
            {
                return mHi > mLo ? mZLo + (mZHi - mZLo) * (t - mLo) / (mHi - mLo) : mZLo;
            }
        };

        F32 segmentDistance(const LLVector2& p, const LLVector2& a, const LLVector2& b, F32* t_out = nullptr)
        {
            const LLVector2 ab = b - a;
            const F32 len2 = ab * ab;
            const F32 t = len2 > 0.f ? llclamp(((p - a) * ab) / len2, 0.f, 1.f) : 0.f;
            if (t_out)
            {
                *t_out = t;
            }
            return (a + ab * t - p).length();
        }

        /// Any polygon of the scene within reach of a scene-local point, at any height.
        bool anyPolyNear(const Scene& scene, const LLVector2& p, F32 reach)
        {
            std::vector<Cell> cells;
            cellsAround(scene, p, reach, cells);
            for (const Cell& c : cells)
            {
                const auto it = scene.mIndex.find(c);
                if (it == scene.mIndex.end())
                {
                    continue;
                }
                for (U32 id : it->second)
                {
                    if (planDistance(scene, scene.mPolys[id], p) <= reach)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// The polygon whose open edge a floor segment lies on, or -1.
        S32 segmentOwner(const Scene& scene, const LLVector2& a, const LLVector2& b, F32 za, F32 zb)
        {
            const LLVector2 mid = (a + b) * 0.5f;
            const F32 zmid = (za + zb) * 0.5f;
            std::vector<Cell> cells;
            cellsAround(scene, mid, MARK_PLAN_M, cells);
            S32 best = -1;
            F32 best_d = FLT_MAX;
            for (const Cell& c : cells)
            {
                const auto it = scene.mIndex.find(c);
                if (it == scene.mIndex.end())
                {
                    continue;
                }
                for (U32 id : it->second)
                {
                    const ScenePoly& poly = scene.mPolys[id];
                    if (zmid < poly.mZMin - MARK_Z_M || zmid > poly.mZMax + MARK_Z_M)
                    {
                        continue;
                    }
                    for (S32 v = 0; v < poly.mVertCount; ++v)
                    {
                        if (!(poly.mOpenEdges & (1 << v)))
                        {
                            continue;
                        }
                        const LLVector3& ea = poly.mVerts[v];
                        const LLVector3& eb = poly.mVerts[(v + 1) % poly.mVertCount];
                        F32 t = 0.f;
                        const F32 d = segmentDistance(mid, plan(ea), plan(eb), &t);
                        const F32 z = ea.mV[VZ] + (eb.mV[VZ] - ea.mV[VZ]) * t;
                        if (d <= MARK_PLAN_M && std::fabs(z - zmid) <= MARK_Z_M && (d < best_d || (d == best_d && static_cast<S32>(id) < best)))
                        {
                            best_d = d;
                            best = static_cast<S32>(id);
                        }
                    }
                }
            }
            return best;
        }

        S32 layerNear(const Scene& scene, const LLVector2& p, F32 z, F32 reach)
        {
            const S32 id = polyNear(scene, scene.mOrigin + LLVector3d(p.mV[VX], p.mV[VY], z), reach);
            return id >= 0 ? scene.mPolys[id].mLayer : -1;
        }

        S8 floorSide(const LLVector2& a, const LLVector2& b, const LLVector2& centroid)
        {
            return cross2(a, b, centroid) > 0.f ? 1 : -1;
        }

        /// A door's marks from its leaves cut 1 m over the sill: one shut bar, or the threshold with each leaf shut or open,
        /// an open hinged leaf with its swing. False when a leaf cuts to nothing, so the door keeps its plain marks.
        bool doorState(Scene& s, const DoorInput& door, const std::vector<sknav::RecordPtr>& leaves, const LineMark& base)
        {
            if (leaves.empty())
            {
                return false;
            }
            const LLVector2 p = s.toLocal(door.mPlanePoint);
            LLVector2 n(door.mNormal.mV[VX], door.mNormal.mV[VY]);
            if (n.normVec() < 1e-3f)
            {
                n.set(1.f, 0.f);
            }
            const LLVector2 t(-n.mV[VY], n.mV[VX]);
            const F32 half = door.mWidth * 0.5f;
            const LLVector2 ja = p - t * half, jb = p + t * half;
            struct Leaf
            {
                LLVector2 mA, mB;
                bool mShut = false;
            };
            std::vector<Leaf> found;
            for (const sknav::RecordPtr& rec : leaves)
            {
                const SliceResult sl = sliceRecord(*rec, static_cast<F32>(door.mSill + DOOR_SLICE_M - rec->mOrigin.mdV[VZ]));
                if (sl.mPts.empty())
                {
                    return false;
                }
                const LLVector2 shift(static_cast<F32>(rec->mOrigin.mdV[VX] - s.mOrigin.mdV[VX]),
                                      static_cast<F32>(rec->mOrigin.mdV[VY] - s.mOrigin.mdV[VY]));
                // The leaf's line: the principal axis of its cut, its ends the extreme projections on it.
                LLVector2 mean;
                for (const LLVector2& q : sl.mPts)
                {
                    mean += q + shift;
                }
                mean /= static_cast<F32>(sl.mPts.size());
                F32 xx = 0.f, xy = 0.f, yy = 0.f;
                for (const LLVector2& q : sl.mPts)
                {
                    const LLVector2 d = q + shift - mean;
                    xx += d.mV[VX] * d.mV[VX];
                    xy += d.mV[VX] * d.mV[VY];
                    yy += d.mV[VY] * d.mV[VY];
                }
                const F32 angle = 0.5f * std::atan2(2.f * xy, xx - yy);
                const LLVector2 axis(std::cos(angle), std::sin(angle));
                F32 lo = FLT_MAX, hi = -FLT_MAX;
                for (const LLVector2& q : sl.mPts)
                {
                    const F32 u = (q + shift - mean) * axis;
                    lo = llmin(lo, u);
                    hi = llmax(hi, u);
                }
                Leaf leaf;
                leaf.mA = mean + axis * lo;
                leaf.mB = mean + axis * hi;
                auto onShutLine = [&](const LLVector2& e) {
                    return std::fabs((e - p) * n) <= DOOR_SHUT_M && std::fabs((e - p) * t) <= half + DOOR_SHUT_M;
                };
                leaf.mShut = onShutLine(leaf.mA) && onShutLine(leaf.mB);
                found.push_back(leaf);
            }
            LineMark mark = base;
            bool all_shut = true;
            for (const Leaf& leaf : found)
            {
                all_shut = all_shut && leaf.mShut;
            }
            if (all_shut)
            {
                mark.mKind = MarkKind::DOOR_SHUT;
                mark.mA = ja;
                mark.mB = jb;
                s.mMarks.push_back(mark);
                return true;
            }
            mark.mKind = MarkKind::DOOR_THRESHOLD;
            mark.mA = ja;
            mark.mB = jb;
            s.mMarks.push_back(mark);
            std::vector<LLVector2> hinges = { ja, jb };
            if (door.mDouble)
            {
                hinges.push_back(p);
            }
            for (const Leaf& leaf : found)
            {
                if (leaf.mShut)
                {
                    mark.mKind = MarkKind::DOOR_SHUT;
                    mark.mA = leaf.mA;
                    mark.mB = leaf.mB;
                    s.mMarks.push_back(mark);
                    continue;
                }
                // The hinge is the end at a jamb (or the middle of a double door); with none the leaf slides.
                F32 best = DOOR_HINGE_M;
                S32 hinge_end = -1;
                size_t hinge_at = 0;
                for (S32 e = 0; e < 2; ++e)
                {
                    const LLVector2& end = e == 0 ? leaf.mA : leaf.mB;
                    for (size_t h = 0; h < hinges.size(); ++h)
                    {
                        const F32 d = (end - hinges[h]).length();
                        if (d <= best)
                        {
                            best = d;
                            hinge_end = e;
                            hinge_at = h;
                        }
                    }
                }
                const LLVector2 hinge = hinge_end == 1 ? leaf.mB : leaf.mA;
                const LLVector2 free_end = hinge_end == 1 ? leaf.mA : leaf.mB;
                mark.mKind = MarkKind::DOOR_LEAF;
                mark.mA = hinge;
                mark.mB = free_end;
                s.mMarks.push_back(mark);
                if (hinge_end < 0)
                {
                    continue;
                }
                // Where the free end rests shut: the far jamb, or the middle of a double door.
                const LLVector2 shut_end = door.mDouble ? (hinge_at == 2 ? (((free_end - ja).length() < (free_end - jb).length()) ? ja : jb) : p)
                                                        : (hinge_at == 0 ? jb : ja);
                LLVector2 toward = shut_end - hinge;
                if (toward.normVec() < 1e-4f)
                {
                    continue;
                }
                const LLVector2 rest = hinge + toward * (free_end - hinge).length();
                const LLVector2 u = rest - hinge, v = free_end - hinge;
                mark.mKind = MarkKind::DOOR_SWING;
                mark.mA = hinge;
                mark.mB = rest;
                mark.mSweep = std::atan2(u.mV[VX] * v.mV[VY] - u.mV[VY] * v.mV[VX], u * v);
                s.mMarks.push_back(mark);
                mark.mSweep = 0.f;
            }
            return true;
        }

        /// The plan distance from a scene-local point to a section piece: 0 inside its fill, else to its nearest edge or line.
        F32 pieceDistance(const SectionPiece& piece, const LLVector2& p)
        {
            const SliceResult& sl = *piece.mSlice;
            const LLVector2 q = p - piece.mShift;
            std::vector<std::pair<const SliceLoop*, bool>> edges;     // (run, closed)
            if (piece.mFlags & PIECE_LINES)
            {
                const size_t nlines = sl.mLines.size();
                for (U16 l : piece.mLines)
                {
                    edges.emplace_back(l < nlines ? &sl.mLines[l] : &sl.mLoops[l - nlines], l >= nlines);
                }
            }
            else
            {
                S32 wind = 0;
                for (size_t k = 0; k < piece.mLoops.size(); ++k)
                {
                    const SliceLoop& loop = sl.mLoops[piece.mLoops[k]];
                    if (loopContains(&sl.mPts[loop.mFirst], loop.mCount, q))
                    {
                        wind += piece.mWind[k];
                    }
                    edges.emplace_back(&loop, true);
                }
                if (wind > 0)
                {
                    return 0.f;
                }
            }
            F32 best = FLT_MAX;
            for (const auto& e : edges)
            {
                const SliceLoop& run = *e.first;
                if (run.mCount == 1)
                {
                    best = llmin(best, (q - sl.mPts[run.mFirst]).length());
                    continue;
                }
                const U32 n = e.second ? run.mCount : run.mCount - 1;
                for (U32 k = 0; k < n; ++k)
                {
                    best = llmin(best, segmentDistance(q, sl.mPts[run.mFirst + k], sl.mPts[run.mFirst + (k + 1) % run.mCount]));
                }
            }
            return best;
        }

        /// Step 8: walls, ledges and overhangs on their floors, drop edges and points, doors,
        /// ladder and elevator glyphs, and the region edges where floors meet them.
        void classifyMarks(Scene& s, const SceneInput& input)
        {
            struct Seg
            {
                LLVector2 mA, mB;
                F32 mZA = 0.f, mZB = 0.f;
                sknav::EdgeKind mKind = sknav::EdgeKind::OPEN;
                F32 mValue = 0.f;
                S32 mOwner = -1;
                bool mDrop = false;
            };
            std::vector<Seg> segs;
            std::map<Cell, std::vector<U32>> seg_index;
            for (const TileInput& tile : input.mTiles)
            {
                if (!tile.mSegments)
                {
                    continue;
                }
                const LLVector3d offset = tile.mOrigin - s.mOrigin;
                const LLVector3 o(static_cast<F32>(offset.mdV[VX]), static_cast<F32>(offset.mdV[VY]), static_cast<F32>(offset.mdV[VZ]));
                for (const sknav::WallSegment& w : *tile.mSegments)
                {
                    Seg seg;
                    seg.mA = plan(w.mA + o);
                    seg.mB = plan(w.mB + o);
                    seg.mZA = w.mA.mV[VZ] + o.mV[VZ];
                    seg.mZB = w.mB.mV[VZ] + o.mV[VZ];
                    seg.mKind = w.mKind;
                    seg.mValue = w.mValue;
                    seg.mOwner = segmentOwner(s, seg.mA, seg.mB, seg.mZA, seg.mZB);
                    const U32 id = static_cast<U32>(segs.size());
                    segs.push_back(seg);
                    const Cell lo = cellOfLocal(s, LLVector2(llmin(seg.mA.mV[VX], seg.mB.mV[VX]), llmin(seg.mA.mV[VY], seg.mB.mV[VY])));
                    const Cell hi = cellOfLocal(s, LLVector2(llmax(seg.mA.mV[VX], seg.mB.mV[VX]), llmax(seg.mA.mV[VY], seg.mB.mV[VY])));
                    for (S32 y = lo.second; y <= hi.second; ++y)
                    {
                        for (S32 x = lo.first; x <= hi.first; ++x)
                        {
                            seg_index[{ x, y }].push_back(id);
                        }
                    }
                }
            }

            // Drops: a link's ledge is the nearest segment of any kind under its start.
            for (const TileInput& tile : input.mTiles)
            {
                if (!tile.mLinks)
                {
                    continue;
                }
                const LLVector3d offset = tile.mOrigin - s.mOrigin;
                const LLVector3 o(static_cast<F32>(offset.mdV[VX]), static_cast<F32>(offset.mdV[VY]), static_cast<F32>(offset.mdV[VZ]));
                for (const sknav::DropLink& link : *tile.mLinks)
                {
                    if (link.mKind != sknav::LinkKind::DROP)
                    {
                        continue;
                    }
                    const LLVector2 from = plan(link.mFrom + o);
                    const F32 fz = link.mFrom.mV[VZ] + o.mV[VZ];
                    std::vector<Cell> cells;
                    cellsAround(s, from, DROP_MATCH_M, cells);
                    S32 best = -1;
                    F32 best_d = FLT_MAX;
                    for (const Cell& c : cells)
                    {
                        const auto it = seg_index.find(c);
                        if (it == seg_index.end())
                        {
                            continue;
                        }
                        for (U32 id : it->second)
                        {
                            const Seg& seg = segs[id];
                            F32 t = 0.f;
                            const F32 d = segmentDistance(from, seg.mA, seg.mB, &t);
                            const F32 z = seg.mZA + (seg.mZB - seg.mZA) * t;
                            if (d <= DROP_MATCH_M && std::fabs(z - fz) <= DROP_MATCH_Z_M && (d < best_d || (d == best_d && static_cast<S32>(id) < best)))
                            {
                                best_d = d;
                                best = static_cast<S32>(id);
                            }
                        }
                    }
                    if (best >= 0)
                    {
                        segs[best].mDrop = true;
                        continue;
                    }
                    LineMark mark;
                    mark.mKind = MarkKind::DROP_POINT;
                    mark.mA = from;
                    mark.mB = plan(link.mTo + o);
                    mark.mZ = fz;
                    mark.mHeight = link.mHeight;
                    mark.mLayer = layerNear(s, from, fz, 0.5f);
                    if (mark.mLayer >= 0)
                    {
                        s.mMarks.push_back(mark);
                    }
                }
            }

            // Orphans: with the section, a wall line stays only where no piece of its layer stands behind it.
            std::vector<std::map<Cell, std::vector<U32>>> piece_cells;
            if (s.mHasSection)
            {
                piece_cells.resize(s.mLayers.size());
                for (size_t l = 0; l < s.mLayers.size(); ++l)
                {
                    for (U32 id : s.mLayers[l].mPieces)
                    {
                        const SectionPiece& piece = s.mPieces[id];
                        if (piece.mFlags & PIECE_BLOB)
                        {
                            continue;   // clutter covers nothing
                        }
                        const LLVector2 grow(ORPHAN_REACH_M, ORPHAN_REACH_M);
                        const Cell lo = cellOfLocal(s, piece.mMin - grow), hi = cellOfLocal(s, piece.mMax + grow);
                        for (S32 y = lo.second; y <= hi.second; ++y)
                        {
                            for (S32 x = lo.first; x <= hi.first; ++x)
                            {
                                piece_cells[l][{ x, y }].push_back(id);
                            }
                        }
                    }
                }
            }
            auto covered = [&](S32 layer, const LLVector2& p) {
                const auto it = piece_cells[layer].find(cellOfLocal(s, p));
                if (it == piece_cells[layer].end())
                {
                    return false;
                }
                for (U32 id : it->second)
                {
                    const SectionPiece& piece = s.mPieces[id];
                    if (p.mV[VX] < piece.mMin.mV[VX] - ORPHAN_REACH_M || p.mV[VX] > piece.mMax.mV[VX] + ORPHAN_REACH_M
                        || p.mV[VY] < piece.mMin.mV[VY] - ORPHAN_REACH_M || p.mV[VY] > piece.mMax.mV[VY] + ORPHAN_REACH_M)
                    {
                        continue;
                    }
                    if (pieceDistance(piece, p) <= ORPHAN_REACH_M)
                    {
                        return true;
                    }
                }
                return false;
            };

            for (const Seg& seg : segs)
            {
                if (!seg.mDrop && seg.mKind == sknav::EdgeKind::OPEN)
                {
                    continue;   // erosion and tile borders: never a line
                }
                LineMark mark;
                mark.mKind = seg.mDrop ? MarkKind::DROP_EDGE
                           : seg.mKind == sknav::EdgeKind::WALL ? MarkKind::WALL
                           : seg.mKind == sknav::EdgeKind::LEDGE ? MarkKind::LEDGE
                                                                 : MarkKind::OVERHANG;
                mark.mA = seg.mA;
                mark.mB = seg.mB;
                mark.mZ = (seg.mZA + seg.mZB) * 0.5f;
                mark.mHeight = seg.mValue;
                if (seg.mOwner >= 0)
                {
                    const ScenePoly& poly = s.mPolys[seg.mOwner];
                    mark.mLayer = poly.mLayer;
                    mark.mFloorSide = floorSide(seg.mA, seg.mB, poly.mCentroid);
                }
                else if (seg.mDrop)
                {
                    mark.mLayer = layerNear(s, (seg.mA + seg.mB) * 0.5f, mark.mZ, 0.5f);
                }
                if (mark.mLayer >= 0 && mark.mKind == MarkKind::WALL && s.mHasSection && mark.mFloorSide != 0)
                {
                    // Probe every 0.5 m part 0.4 m off the floor; keep each run of parts nothing stands behind.
                    const LLVector2 d = mark.mB - mark.mA;
                    const F32 len = d.length();
                    const S32 n = llmax(2, static_cast<S32>(std::ceil(len / ORPHAN_STEP_M)));
                    const LLVector2 away = len > 0.f ? LLVector2(-d.mV[VY], d.mV[VX]) * (-mark.mFloorSide / len) : LLVector2();
                    S32 start = -1;
                    for (S32 k = 0; k <= n; ++k)
                    {
                        const bool cover = k == n || covered(mark.mLayer, mark.mA + d * ((k + 0.5f) / n) + away * ORPHAN_PROBE_M);
                        if (!cover && start < 0)
                        {
                            start = k;
                        }
                        if (cover && start >= 0)
                        {
                            LineMark orphan = mark;
                            orphan.mA = start > 0 ? mark.mA + d * (static_cast<F32>(start) / n) : mark.mA;
                            orphan.mB = k < n ? mark.mA + d * (static_cast<F32>(k) / n) : mark.mB;
                            s.mMarks.push_back(orphan);
                            start = -1;
                        }
                    }
                    continue;
                }
                if (mark.mLayer >= 0)
                {
                    s.mMarks.push_back(mark);
                }
            }

            // Doors: on the floor nearest the sill, by their leaves' live state, or jambs and a leaf.
            for (size_t di = 0; di < input.mDoors.size(); ++di)
            {
                const DoorInput& door = input.mDoors[di];
                const LLVector2 p = s.toLocal(door.mPlanePoint);
                const F32 sill = static_cast<F32>(door.mSill - s.mOrigin.mdV[VZ]);
                std::vector<Cell> cells;
                cellsAround(s, p, 2.f, cells);
                S32 layer = -1;
                F32 best_dz = FLT_MAX;
                S32 best_id = -1;
                for (const Cell& c : cells)
                {
                    const auto it = s.mIndex.find(c);
                    if (it == s.mIndex.end())
                    {
                        continue;
                    }
                    for (U32 id : it->second)
                    {
                        const ScenePoly& poly = s.mPolys[id];
                        if (planDistance(s, poly, p) > 2.f)
                        {
                            continue;
                        }
                        const F32 dz = sill < poly.mZMin ? poly.mZMin - sill : sill > poly.mZMax ? sill - poly.mZMax : 0.f;
                        if (dz < best_dz || (dz == best_dz && static_cast<S32>(id) < best_id))
                        {
                            best_dz = dz;
                            best_id = static_cast<S32>(id);
                            layer = poly.mLayer;
                        }
                    }
                }
                if (layer < 0)
                {
                    continue;
                }
                LineMark mark;
                mark.mZ = sill;
                mark.mLayer = layer;
                if (door.mPlaneKnown && di < s.mDoorLeaves.size() && doorState(s, door, s.mDoorLeaves[di], mark))
                {
                    continue;
                }
                if (door.mPlaneKnown)
                {
                    LLVector2 n(door.mNormal.mV[VX], door.mNormal.mV[VY]);
                    if (n.normVec() < 1e-3f)
                    {
                        n.set(1.f, 0.f);
                    }
                    const LLVector2 t(-n.mV[VY], n.mV[VX]);
                    const F32 half = door.mWidth * 0.5f;
                    const LLVector2 ja = p - t * half, jb = p + t * half;
                    for (const LLVector2& j : { ja, jb })
                    {
                        mark.mKind = MarkKind::DOOR_JAMB;
                        mark.mA = j - n * (DOOR_JAMB_M * 0.5f);
                        mark.mB = j + n * (DOOR_JAMB_M * 0.5f);
                        s.mMarks.push_back(mark);
                    }
                    mark.mKind = MarkKind::DOOR_LEAF;
                    if (door.mDouble)
                    {
                        mark.mA = ja;
                        mark.mB = p;
                        s.mMarks.push_back(mark);
                        mark.mA = p;
                        mark.mB = jb;
                        s.mMarks.push_back(mark);
                    }
                    else
                    {
                        mark.mA = ja;
                        mark.mB = jb;
                        s.mMarks.push_back(mark);
                    }
                }
                else
                {
                    const LLVector2 lo = s.toLocal(door.mMin), hi = s.toLocal(door.mMax);
                    const LLVector2 c = (lo + hi) * 0.5f;
                    mark.mKind = MarkKind::DOOR_LEAF;
                    if (hi.mV[VX] - lo.mV[VX] >= hi.mV[VY] - lo.mV[VY])
                    {
                        mark.mA.set(lo.mV[VX], c.mV[VY]);
                        mark.mB.set(hi.mV[VX], c.mV[VY]);
                    }
                    else
                    {
                        mark.mA.set(c.mV[VX], lo.mV[VY]);
                        mark.mB.set(c.mV[VX], hi.mV[VY]);
                    }
                    s.mMarks.push_back(mark);
                }
            }

            // Ladders and elevators: a glyph at each lower exit, on the floor under it.
            std::set<std::pair<U8, std::pair<S32, std::pair<S32, S32>>>> seen;
            for (const FeatureLink& link : input.mFeatureLinks)
            {
                const LLVector3d& lower = link.mFrom.mdV[VZ] <= link.mTo.mdV[VZ] ? link.mFrom : link.mTo;
                const auto key = std::make_pair(static_cast<U8>(link.mKind),
                                                std::make_pair(static_cast<S32>(std::floor(lower.mdV[VX] * 10.0)),
                                                               std::make_pair(static_cast<S32>(std::floor(lower.mdV[VY] * 10.0)),
                                                                              static_cast<S32>(std::floor(lower.mdV[VZ] * 10.0)))));
                if (!seen.insert(key).second)
                {
                    continue;
                }
                const FloorHit hit = floorAt(s, lower, 2.5f, 0.5f);
                LineMark mark;
                mark.mKind = link.mKind == sknav::LinkKind::ELEVATOR ? MarkKind::ELEVATOR : MarkKind::LADDER;
                mark.mA = s.toLocal(lower);
                mark.mB = mark.mA;
                mark.mZ = static_cast<F32>(lower.mdV[VZ] - s.mOrigin.mdV[VZ]);
                mark.mLayer = hit.mFound ? hit.mLayer : layerNear(s, mark.mA, mark.mZ, LINK_REACH_M);
                if (mark.mLayer >= 0)
                {
                    s.mMarks.push_back(mark);
                }
            }

            for (size_t i = 0; i < s.mMarks.size(); ++i)
            {
                s.mLayers[s.mMarks[i].mLayer].mMarks.push_back(static_cast<U32>(i));
            }

            // Region edges where floors come within reach of them. A shared edge is drawn once,
            // by the region on its south or west side.
            auto inRegion = [&](F64 x, F64 y) {
                for (const RegionSquare& r : input.mRegions)
                {
                    if (x >= r.mOrigin.mdV[VX] && x < r.mOrigin.mdV[VX] + r.mWidth && y >= r.mOrigin.mdV[VY] && y < r.mOrigin.mdV[VY] + r.mWidth)
                    {
                        return true;
                    }
                }
                return false;
            };
            for (const RegionSquare& r : input.mRegions)
            {
                const F64 x0 = r.mOrigin.mdV[VX], y0 = r.mOrigin.mdV[VY], w = r.mWidth;
                struct Side
                {
                    F64 mX, mY, mDX, mDY, mNX, mNY;
                };
                const Side sides[4] = {
                    { x0, y0, 1.0, 0.0, 0.0, -1.0 },        // south
                    { x0 + w, y0, 0.0, 1.0, 1.0, 0.0 },     // east
                    { x0, y0 + w, 1.0, 0.0, 0.0, 1.0 },     // north
                    { x0, y0, 0.0, 1.0, -1.0, 0.0 },        // west
                };
                for (const Side& side : sides)
                {
                    const bool owns_shared = side.mNX > 0.0 || side.mNY > 0.0;
                    S32 open_kind = -1;     // 0 void, 1 region, -1 none
                    F64 open_from = 0.0;
                    auto close = [&](F64 to) {
                        if (open_kind < 0)
                        {
                            return;
                        }
                        LineMark mark;
                        mark.mKind = open_kind == 0 ? MarkKind::VOID_BOUNDARY : MarkKind::REGION_BOUNDARY;
                        mark.mA = s.toLocal(LLVector3d(side.mX + side.mDX * open_from, side.mY + side.mDY * open_from, 0.0));
                        mark.mB = s.toLocal(LLVector3d(side.mX + side.mDX * to, side.mY + side.mDY * to, 0.0));
                        s.mBoundaries.push_back(mark);
                        open_kind = -1;
                    };
                    for (F64 t = 0.0; t < w - 1e-6; t += BOUNDARY_STEP_M)
                    {
                        const F64 mt = llmin(t + BOUNDARY_STEP_M * 0.5, w);
                        const F64 mx = side.mX + side.mDX * mt, my = side.mY + side.mDY * mt;
                        S32 kind = -1;
                        if (mx >= s.mMin.mdV[VX] && mx <= s.mMax.mdV[VX] && my >= s.mMin.mdV[VY] && my <= s.mMax.mdV[VY]
                            && anyPolyNear(s, s.toLocal(LLVector3d(mx, my, 0.0)), BOUNDARY_NEAR_M))
                        {
                            const bool across = inRegion(mx + side.mNX * BOUNDARY_NEAR_M, my + side.mNY * BOUNDARY_NEAR_M);
                            kind = across ? (owns_shared ? 1 : -1) : 0;
                        }
                        if (kind != open_kind)
                        {
                            close(t);
                            if (kind >= 0)
                            {
                                open_kind = kind;
                                open_from = t;
                            }
                        }
                    }
                    close(w);
                }
            }
        }

        /// Step 9: iso-height segments on terrain and ramps, every 0.5 m.
        void sampleContours(Scene& s)
        {
            for (size_t i = 0; i < s.mPolys.size(); ++i)
            {
                const ScenePoly& poly = s.mPolys[i];
                if (poly.mLayer < 0)
                {
                    continue;
                }
                const bool terrain = poly.mRun < 0 && poly.mSurface >= 0 && s.mSurfaces[poly.mSurface].mTerrain;
                const bool ramp = poly.mRun >= 0 && !s.mRuns[poly.mRun].mStair;
                if (!terrain && !ramp)
                {
                    continue;
                }
                if (!ramp && poly.mArea == sknav::AREA_OBJECT && poly.mZMax - poly.mZMin < FLAT_POLY_M)
                {
                    continue;
                }
                for (U32 t = 0; t < poly.mTriCount; ++t)
                {
                    const LLVector3* tri = &s.mTris[(poly.mFirstTri + t) * 3];
                    const F32 lo = llmin(tri[0].mV[VZ], llmin(tri[1].mV[VZ], tri[2].mV[VZ]));
                    const F32 hi = llmax(tri[0].mV[VZ], llmax(tri[1].mV[VZ], tri[2].mV[VZ]));
                    const S32 k0 = static_cast<S32>(std::floor(lo / CONTOUR_STEP_M));
                    const S32 k1 = static_cast<S32>(std::ceil(hi / CONTOUR_STEP_M));
                    for (S32 k = k0; k <= k1; ++k)
                    {
                        const F32 level = k * CONTOUR_STEP_M;
                        if (level <= lo || level >= hi)
                        {
                            continue;
                        }
                        LLVector2 pts[2];
                        S32 n = 0;
                        for (S32 e = 0; e < 3 && n < 2; ++e)
                        {
                            const LLVector3& a = tri[e];
                            const LLVector3& b = tri[(e + 1) % 3];
                            if ((a.mV[VZ] >= level) != (b.mV[VZ] >= level))
                            {
                                const F32 u = (level - a.mV[VZ]) / (b.mV[VZ] - a.mV[VZ]);
                                pts[n++] = plan(a) + (plan(b) - plan(a)) * u;
                            }
                        }
                        if (n == 2)
                        {
                            Contour c;
                            c.mA = pts[0];
                            c.mB = pts[1];
                            c.mLevel = k;
                            c.mLayer = poly.mLayer;
                            s.mLayers[poly.mLayer].mContours.push_back(static_cast<U32>(s.mContours.size()));
                            s.mContours.push_back(c);
                        }
                    }
                }
            }
        }

        S64 quantise(F64 v, F64 scale)
        {
            return static_cast<S64>(std::floor(v * scale + 0.5));
        }

        /// A node's hash starts from its cell, so two cells with the same content never share a bake.
        U64 cellSeed(const CellId& cell)
        {
            return sknav::mix(sknav::mix(sknav::mix(sknav::SEED, static_cast<U64>(cell.mTier)), static_cast<U64>(static_cast<S64>(cell.mX))),
                              static_cast<U64>(static_cast<S64>(cell.mY)));
        }

        /// Step 12: per layer, a quadtree of content hashes over 8 m leaves, and a hash of the
        /// layer's line marks.
        void hashCells(Scene& s, const SceneInput& input)
        {
            s.mCellHashes.assign(s.mLayers.size(), {});
            s.mMarkHashes.assign(s.mLayers.size(), 0);
            auto leavesOf = [&](F64 x0, F64 y0, F64 x1, F64 y1, std::vector<CellId>& out) {
                cellsInBox(TIER_MIN, LLVector3d(s.mOrigin.mdV[VX] + x0 - HASH_REACH_M, s.mOrigin.mdV[VY] + y0 - HASH_REACH_M, 0.0),
                           LLVector3d(s.mOrigin.mdV[VX] + x1 + HASH_REACH_M, s.mOrigin.mdV[VY] + y1 + HASH_REACH_M, 0.0), out);
            };
            std::vector<CellId> leaves;
            for (size_t l = 0; l < s.mLayers.size(); ++l)
            {
                const SceneLayer& layer = s.mLayers[l];
                std::map<CellId, std::vector<U64>> records;
                for (U32 id : layer.mPolys)
                {
                    const ScenePoly& poly = s.mPolys[id];
                    const TileInput& tile = input.mTiles[poly.mTile];
                    U64 r = sknav::mix(sknav::SEED, tile.mMesh.getCRC32());
                    r = sknav::mix(r, static_cast<U32>(tile.mKey.mX));
                    r = sknav::mix(r, static_cast<U32>(tile.mKey.mY));
                    r = sknav::mix(r, tile.mContentKey);
                    r = sknav::mix(r, poly.mIndex);
                    r = sknav::mix(r, poly.mCoverage);
                    U64 run_kind = 0;
                    if (poly.mRun >= 0)
                    {
                        const SceneRun& run = s.mRuns[poly.mRun];
                        run_kind = run.mStair ? 2 + (static_cast<U64>(run.mTreads) << 8) : 1;
                        // Rungs and chevrons are drawn from the run's ends, global so a box step keeps them.
                        for (const LLVector3* end : { &run.mBottomA, &run.mBottomB, &run.mTopA, &run.mTopB })
                        {
                            r = sknav::mix(r, static_cast<U64>(quantise(s.mOrigin.mdV[VX] + end->mV[VX], 100.0)));
                            r = sknav::mix(r, static_cast<U64>(quantise(s.mOrigin.mdV[VY] + end->mV[VY], 100.0)));
                            r = sknav::mix(r, static_cast<U64>(quantise(s.mOrigin.mdV[VZ] + end->mV[VZ], 100.0)));
                        }
                        if (!run.mStair)
                        {
                            // Only a ramp is shaded by height, against the layer's base.
                            r = sknav::mix(r, static_cast<U64>(quantise(layer.mBaseZ, 10.0)));
                        }
                    }
                    r = sknav::mix(r, run_kind);
                    r = sknav::mix(r, static_cast<U64>(quantise(poly.mWater, 10.0)));
                    leavesOf(poly.mMinX, poly.mMinY, poly.mMaxX, poly.mMaxY, leaves);
                    for (const CellId& c : leaves)
                    {
                        records[c].push_back(r);
                    }
                }
                std::map<CellId, std::vector<U64>> marks;
                std::vector<U64> mark_records;
                for (U32 m : layer.mMarks)
                {
                    const LineMark& mark = s.mMarks[m];
                    const S64 ax = quantise(s.mOrigin.mdV[VX] + mark.mA.mV[VX], 100.0), ay = quantise(s.mOrigin.mdV[VY] + mark.mA.mV[VY], 100.0);
                    const S64 bx = quantise(s.mOrigin.mdV[VX] + mark.mB.mV[VX], 100.0), by = quantise(s.mOrigin.mdV[VY] + mark.mB.mV[VY], 100.0);
                    U64 r = sknav::mix(sknav::SEED, static_cast<U64>(mark.mKind));
                    r = sknav::mix(sknav::mix(sknav::mix(sknav::mix(r, ax), ay), bx), by);
                    U64 ribbon = sknav::mix(sknav::mix(r, static_cast<U64>(quantise(mark.mHeight, 100.0))),
                                            static_cast<U64>(static_cast<S64>(mark.mFloorSide)));
                    ribbon = sknav::mix(ribbon, static_cast<U64>(quantise(mark.mSweep, 100.0)));
                    mark_records.push_back(ribbon);
                    if (mark.mKind != MarkKind::LEDGE)
                    {
                        continue;   // the raster's halo reads only these
                    }
                    leavesOf(llmin(mark.mA.mV[VX], mark.mB.mV[VX]), llmin(mark.mA.mV[VY], mark.mB.mV[VY]),
                             llmax(mark.mA.mV[VX], mark.mB.mV[VX]), llmax(mark.mA.mV[VY], mark.mB.mV[VY]), leaves);
                    for (const CellId& c : leaves)
                    {
                        marks[c].push_back(r);
                    }
                }
                // Section pieces: leaves are made from them too, so a piece far from any floor still bakes.
                for (U32 id : layer.mPieces)
                {
                    const SectionPiece& piece = s.mPieces[id];
                    U64 r = sknav::mix(sknav::SEED, piece.mSource);
                    r = sknav::mix(r, static_cast<U64>(static_cast<S64>(piece.mPlane)));
                    r = sknav::mix(r, static_cast<U64>(piece.mStyle));
                    r = sknav::mix(r, piece.mFlags);
                    r = sknav::mix(r, piece.mGroup);
                    r = sknav::mix(r, static_cast<U64>(quantise(s.mOrigin.mdV[VX] + piece.mShift.mV[VX], 100.0)));
                    r = sknav::mix(r, static_cast<U64>(quantise(s.mOrigin.mdV[VY] + piece.mShift.mV[VY], 100.0)));
                    for (size_t k = 0; k < piece.mLoops.size(); ++k)
                    {
                        r = sknav::mix(sknav::mix(r, piece.mLoops[k]), static_cast<U64>(static_cast<S64>(piece.mWind[k])));
                    }
                    for (U16 line : piece.mLines)
                    {
                        r = sknav::mix(r, line);
                    }
                    leavesOf(piece.mMin.mV[VX], piece.mMin.mV[VY], piece.mMax.mV[VX], piece.mMax.mV[VY], leaves);
                    for (const CellId& c : leaves)
                    {
                        records[c].push_back(r);
                    }
                }
                // The floor levels each leaf's own 1 m cells list; a node mixes its 3 x 3 leaves, which covers the levels' reach.
                const LevelGrid& grid = layer.mLevels;
                std::map<std::pair<S32, S32>, U64> leaf_levels;
                auto levelsOfLeaf = [&](S32 lx, S32 ly) {
                    const auto it = leaf_levels.find({ lx, ly });
                    if (it != leaf_levels.end())
                    {
                        return it->second;
                    }
                    U64 h = sknav::SEED;
                    const S32 side = static_cast<S32>(tierCellSize(TIER_MIN) / LEVEL_CELL_M);
                    for (S32 gy = ly * side; gy < (ly + 1) * side; ++gy)
                    {
                        for (S32 gx = lx * side; gx < (lx + 1) * side; ++gx)
                        {
                            const S32 x = gx - grid.mX0, y = gy - grid.mY0;
                            if (x < 0 || x >= grid.mW || y < 0 || y >= grid.mH)
                            {
                                continue;
                            }
                            const U32 c = grid.mCell[static_cast<size_t>(y) * grid.mW + x];
                            if (!c)
                            {
                                continue;
                            }
                            h = sknav::mix(sknav::mix(h, static_cast<U64>(static_cast<S64>(gx))), static_cast<U64>(static_cast<S64>(gy)));
                            for (S32 k = 0; k <= grid.mLists[c - 1]; ++k)
                            {
                                h = sknav::mix(h, static_cast<U64>(static_cast<S64>(grid.mLists[c - 1 + k])));
                            }
                        }
                    }
                    leaf_levels.emplace(std::make_pair(lx, ly), h);
                    return h;
                };

                std::sort(mark_records.begin(), mark_records.end());
                U64 mark_hash = sknav::SEED;
                for (U64 r : mark_records)
                {
                    mark_hash = sknav::mix(mark_hash, r);
                }
                s.mMarkHashes[l] = mark_hash;

                std::map<CellId, U64>& nodes = s.mCellHashes[l];
                for (auto& entry : records)
                {
                    std::vector<U64>& rs = entry.second;
                    std::sort(rs.begin(), rs.end());
                    U64 h = cellSeed(entry.first);
                    for (U64 r : rs)
                    {
                        h = sknav::mix(h, r);
                    }
                    const auto mit = marks.find(entry.first);
                    if (mit != marks.end())
                    {
                        std::sort(mit->second.begin(), mit->second.end());
                        for (U64 r : mit->second)
                        {
                            h = sknav::mix(h, r);
                        }
                    }
                    if (grid.mW > 0)
                    {
                        for (S32 dy = -1; dy <= 1; ++dy)
                        {
                            for (S32 dx = -1; dx <= 1; ++dx)
                            {
                                h = sknav::mix(h, levelsOfLeaf(entry.first.mX + dx, entry.first.mY + dy));
                            }
                        }
                    }
                    nodes[entry.first] = h ? h : 1;
                }
                // Parents mix their four children in a fixed order.
                for (S32 tier = TIER_MIN + 1; tier <= TIER_MAX; ++tier)
                {
                    std::set<CellId> parents;
                    for (const auto& entry : nodes)
                    {
                        if (entry.first.mTier == tier - 1)
                        {
                            parents.insert(entry.first.ancestor(tier));
                        }
                    }
                    for (const CellId& p : parents)
                    {
                        U64 h = cellSeed(p);
                        for (S32 k = 0; k < 4; ++k)
                        {
                            const CellId child = { tier - 1, p.mX * 2 + (k & 1), p.mY * 2 + (k >> 1) };
                            const auto it = nodes.find(child);
                            h = sknav::mix(h, it != nodes.end() ? it->second : 0);
                        }
                        nodes[p] = h ? h : 1;
                    }
                }
            }
        }

        S32 floorDivInt(S32 a, S32 b)
        {
            return a >= 0 ? a / b : -((-a + b - 1) / b);
        }

        S32 cellIndex(F64 v, F64 size)
        {
            return static_cast<S32>(std::floor(v / size));
        }

        /// Whether any global 1 m cell of a scene-local box lists the level.
        bool anyLevelNear(const Scene& s, const LevelGrid& grid, const LLVector2& lo, const LLVector2& hi, S32 k)
        {
            const S32 x0 = cellIndex(s.mOrigin.mdV[VX] + lo.mV[VX], LEVEL_CELL_M), x1 = cellIndex(s.mOrigin.mdV[VX] + hi.mV[VX], LEVEL_CELL_M);
            const S32 y0 = cellIndex(s.mOrigin.mdV[VY] + lo.mV[VY], LEVEL_CELL_M), y1 = cellIndex(s.mOrigin.mdV[VY] + hi.mV[VY], LEVEL_CELL_M);
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    if (levelListed(grid, x, y, k))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// The topmost floor of one layer at a scene-local point.
        bool layerFloorAt(const Scene& s, S32 layer, const LLVector2& p, F32& z)
        {
            const auto it = s.mIndex.find(cellOfLocal(s, p));
            if (it == s.mIndex.end())
            {
                return false;
            }
            bool found = false;
            for (U32 id : it->second)
            {
                const ScenePoly& poly = s.mPolys[id];
                if (poly.mLayer != layer || !polyContains(s, poly, p))
                {
                    continue;
                }
                const F32 h = polyHeightAt(s, poly, p);
                if (!found || h > z)
                {
                    z = h;
                    found = true;
                }
            }
            return found;
        }

        /// The layer's floor levels per global 1 m cell, spread six cells off the floor by a multi-source search.
        void buildLevels(const Scene& s, const SceneLayer& layer, LevelGrid& grid)
        {
            grid = LevelGrid();
            if (layer.mPolys.empty())
            {
                return;
            }
            const F64 ox = s.mOrigin.mdV[VX], oy = s.mOrigin.mdV[VY], oz = s.mOrigin.mdV[VZ];
            F64 lo_x = DBL_MAX, lo_y = DBL_MAX, hi_x = -DBL_MAX, hi_y = -DBL_MAX;
            for (U32 id : layer.mPolys)
            {
                const ScenePoly& poly = s.mPolys[id];
                lo_x = llmin(lo_x, ox + poly.mMinX);
                lo_y = llmin(lo_y, oy + poly.mMinY);
                hi_x = llmax(hi_x, ox + poly.mMaxX);
                hi_y = llmax(hi_y, oy + poly.mMaxY);
            }
            grid.mX0 = cellIndex(lo_x, LEVEL_CELL_M) - SECTION_REACH_CELLS;
            grid.mY0 = cellIndex(lo_y, LEVEL_CELL_M) - SECTION_REACH_CELLS;
            grid.mW = cellIndex(hi_x, LEVEL_CELL_M) + SECTION_REACH_CELLS - grid.mX0 + 1;
            grid.mH = cellIndex(hi_y, LEVEL_CELL_M) + SECTION_REACH_CELLS - grid.mY0 + 1;
            const S32 w = grid.mW, h = grid.mH;
            const size_t n = static_cast<size_t>(w) * h;

            // Samples: each polygon's height at every cell centre it holds, or at its centroid when it holds none.
            // Cells keep linked lists of samples, so a terrain-sized grid allocates nothing per cell.
            struct Link
            {
                S32 mLevel;
                S32 mNext;
            };
            std::vector<Link> links;
            std::vector<S32> head(n, -1);
            std::vector<size_t> sampled;
            auto add = [&](size_t i, S32 k) {
                if (head[i] < 0)
                {
                    sampled.push_back(i);
                }
                links.push_back({ k, head[i] });
                head[i] = static_cast<S32>(links.size()) - 1;
            };
            for (U32 id : layer.mPolys)
            {
                const ScenePoly& poly = s.mPolys[id];
                const S32 cx0 = static_cast<S32>(std::ceil(ox + poly.mMinX - 0.5)), cx1 = static_cast<S32>(std::floor(ox + poly.mMaxX - 0.5));
                const S32 cy0 = static_cast<S32>(std::ceil(oy + poly.mMinY - 0.5)), cy1 = static_cast<S32>(std::floor(oy + poly.mMaxY - 0.5));
                bool any = false;
                for (S32 cy = cy0; cy <= cy1; ++cy)
                {
                    for (S32 cx = cx0; cx <= cx1; ++cx)
                    {
                        const LLVector2 c(static_cast<F32>(cx + 0.5 - ox), static_cast<F32>(cy + 0.5 - oy));
                        if (!polyContains(s, poly, c))
                        {
                            continue;
                        }
                        add(static_cast<size_t>(cy - grid.mY0) * w + (cx - grid.mX0), sectionLevel(static_cast<F32>(polyHeightAt(s, poly, c) + oz)));
                        any = true;
                    }
                }
                if (!any)
                {
                    const S32 cx = cellIndex(ox + poly.mCentroid.mV[VX], LEVEL_CELL_M), cy = cellIndex(oy + poly.mCentroid.mV[VY], LEVEL_CELL_M);
                    add(static_cast<size_t>(cy - grid.mY0) * w + (cx - grid.mX0),
                        sectionLevel(static_cast<F32>(polyHeightAt(s, poly, poly.mCentroid) + oz)));
                }
            }
            // Up to LEVEL_MAX levels per cell, in fixed slots.
            std::vector<S32> slots(n * LEVEL_MAX, 0);
            std::vector<U8> counts(n, 0);
            std::vector<U8> seen(n, 0);
            std::vector<std::pair<S32, U32>> merge_in;
            std::vector<S32> merge_out;
            auto store = [&](size_t i) {
                mergeLevels(merge_in, merge_out);
                counts[i] = static_cast<U8>(merge_out.size());
                std::copy(merge_out.begin(), merge_out.end(), slots.begin() + i * LEVEL_MAX);
                seen[i] = 1;
            };
            std::sort(sampled.begin(), sampled.end());
            for (size_t i : sampled)
            {
                merge_in.clear();
                for (S32 k = head[i]; k >= 0; k = links[k].mNext)
                {
                    merge_in.emplace_back(links[k].mLevel, 1u);
                }
                store(i);
            }
            std::vector<size_t> frontier = sampled;
            for (S32 ring = 0; ring < SECTION_REACH_CELLS && !frontier.empty(); ++ring)
            {
                // Each new cell takes the union of its frontier neighbours' levels, each counted once.
                links.clear();
                std::vector<size_t> reached;
                for (size_t f : frontier)
                {
                    const S32 fx = static_cast<S32>(f % w), fy = static_cast<S32>(f / w);
                    for (S32 dy = -1; dy <= 1; ++dy)
                    {
                        for (S32 dx = -1; dx <= 1; ++dx)
                        {
                            const S32 x = fx + dx, y = fy + dy;
                            if (x < 0 || x >= w || y < 0 || y >= h)
                            {
                                continue;
                            }
                            const size_t i = static_cast<size_t>(y) * w + x;
                            if (seen[i])
                            {
                                continue;
                            }
                            if (head[i] < 0)
                            {
                                reached.push_back(i);
                            }
                            for (S32 k = 0; k < counts[f]; ++k)
                            {
                                links.push_back({ slots[f * LEVEL_MAX + k], head[i] });
                                head[i] = static_cast<S32>(links.size()) - 1;
                            }
                        }
                    }
                }
                std::sort(reached.begin(), reached.end());
                for (size_t i : reached)
                {
                    merge_in.clear();
                    for (S32 k = head[i]; k >= 0; k = links[k].mNext)
                    {
                        merge_in.emplace_back(links[k].mLevel, 1u);
                    }
                    head[i] = -1;
                    std::sort(merge_in.begin(), merge_in.end());
                    merge_in.erase(std::unique(merge_in.begin(), merge_in.end()), merge_in.end());
                    store(i);
                }
                frontier.swap(reached);
            }
            grid.mCell.assign(n, 0);
            for (size_t i = 0; i < n; ++i)
            {
                if (!counts[i])
                {
                    continue;
                }
                grid.mCell[i] = static_cast<U32>(grid.mLists.size()) + 1;
                grid.mLists.push_back(counts[i]);
                grid.mLists.insert(grid.mLists.end(), slots.begin() + i * LEVEL_MAX, slots.begin() + i * LEVEL_MAX + counts[i]);
            }
        }

        /// The layers' section: records filtered, bucketed, sliced at the planes their cells' floor levels ask for, and
        /// cut into pieces; door leaves set aside for the door marks.
        void buildSection(Scene& s, const SceneInput& input, SectionCache* cache)
        {
            LL_PROFILE_ZONE_SCOPED;
            const auto started = std::chrono::steady_clock::now();
            SectionStats& stats = s.mSectionStats;
            const std::vector<sknav::RecordPtr>& recs = *input.mRecords;

            // 1. Door leaves first: a swinging leaf is dynamic and must still reach its door.
            s.mDoorLeaves.assign(input.mDoors.size(), {});
            std::map<LLUUID, size_t> leaf_door;
            for (size_t i = 0; i < input.mDoors.size(); ++i)
            {
                for (const LLUUID& id : input.mDoors[i].mLeaves)
                {
                    leaf_door.emplace(id, i);
                }
            }
            // 2. What stands still and blocks avatars, near the box.
            const F64 reach = SECTION_REACH_CELLS * LEVEL_CELL_M;
            std::vector<U32> kept;
            for (size_t i = 0; i < recs.size(); ++i)
            {
                const sknav::Record* r = recs[i].get();
                if (!r)
                {
                    continue;
                }
                if (r->mFeature == sknav::FeatureKind::DOOR)
                {
                    const auto it = leaf_door.find(r->mPart);
                    if (it != leaf_door.end())
                    {
                        s.mDoorLeaves[it->second].push_back(recs[i]);
                        continue;
                    }
                }
                if (r->mRole != sknav::Role::NAV || r->mDynamic || (r->mExclude & sknav::EXCLUDE_ALL) == sknav::EXCLUDE_ALL
                    || r->mNavRole == sknav::NavRole::MATERIAL_VOLUME || r->mNavRole == sknav::NavRole::EXCLUSION_VOLUME
                    || r->mNavRole == sknav::NavRole::DYNAMIC)
                {
                    continue;
                }
                if (r->mMax.mdV[VX] < input.mMin.mdV[VX] - reach || r->mMin.mdV[VX] > input.mMax.mdV[VX] + reach
                    || r->mMax.mdV[VY] < input.mMin.mdV[VY] - reach || r->mMin.mdV[VY] > input.mMax.mdV[VY] + reach)
                {
                    continue;
                }
                kept.push_back(static_cast<U32>(i));
            }
            stats.mRecords = static_cast<U32>(kept.size());

            // 3. Global 8 m buckets; a record over more than 8 x 8 of them goes on a list every bucket sees.
            std::map<Cell, std::vector<U32>> buckets;
            std::vector<U32> large;
            for (U32 i : kept)
            {
                const sknav::Record& r = *recs[i];
                const S32 bx0 = cellIndex(r.mMin.mdV[VX], RECORD_BUCKET_M), bx1 = cellIndex(r.mMax.mdV[VX], RECORD_BUCKET_M);
                const S32 by0 = cellIndex(r.mMin.mdV[VY], RECORD_BUCKET_M), by1 = cellIndex(r.mMax.mdV[VY], RECORD_BUCKET_M);
                if (static_cast<S64>(bx1 - bx0 + 1) * (by1 - by0 + 1) > RECORD_LARGE_BUCKETS)
                {
                    large.push_back(i);
                    continue;
                }
                for (S32 by = by0; by <= by1; ++by)
                {
                    for (S32 bx = bx0; bx <= bx1; ++bx)
                    {
                        buckets[{ bx, by }].push_back(i);
                    }
                }
            }

            // One slice per (record, plane) for the whole scene, from the cache when there is one.
            std::map<std::pair<U32, S32>, SlicePtr> slices;
            std::set<S32> planes_used;
            auto sliceOf = [&](U32 ri, S32 q) {
                const auto key = std::make_pair(ri, q);
                const auto it = slices.find(key);
                if (it != slices.end())
                {
                    return it->second;
                }
                const sknav::Record& r = *recs[ri];
                SlicePtr sl;
                if (cache)
                {
                    bool hit = false;
                    sl = cache->get(r, q, &hit);
                    (hit ? stats.mCacheHits : stats.mSlices)++;
                }
                else
                {
                    sl = std::make_shared<SliceResult>(sliceRecord(r, sectionPlaneZ(q) - static_cast<F32>(r.mOrigin.mdV[VZ])));
                    stats.mSlices++;
                }
                slices.emplace(key, sl);
                return sl;
            };

            const S32 per_bucket = static_cast<S32>(RECORD_BUCKET_M / LEVEL_CELL_M);
            std::vector<U32> piece_record;      // per piece, its record's index
            for (size_t l = 0; l < s.mLayers.size(); ++l)
            {
                SceneLayer& layer = s.mLayers[l];
                // 4. The layer's local floor levels.
                buildLevels(s, layer, layer.mLevels);
                const LevelGrid& grid = layer.mLevels;
                if (grid.mW == 0)
                {
                    continue;
                }
                // 5. Per bucket, the planes its cells (and a ring around them) ask for, against each record's height.
                std::vector<std::pair<U32, S32>> pairs;
                const S32 bx0 = floorDivInt(grid.mX0, per_bucket), bx1 = floorDivInt(grid.mX0 + grid.mW - 1, per_bucket);
                const S32 by0 = floorDivInt(grid.mY0, per_bucket), by1 = floorDivInt(grid.mY0 + grid.mH - 1, per_bucket);
                std::set<S32> planes;
                for (S32 by = by0; by <= by1; ++by)
                {
                    for (S32 bx = bx0; bx <= bx1; ++bx)
                    {
                        const auto bucket = buckets.find({ bx, by });
                        if (bucket == buckets.end() && large.empty())
                        {
                            continue;
                        }
                        planes.clear();
                        const S32 x0 = llmax(grid.mX0, bx * per_bucket - 1), x1 = llmin(grid.mX0 + grid.mW - 1, bx * per_bucket + per_bucket);
                        const S32 y0 = llmax(grid.mY0, by * per_bucket - 1), y1 = llmin(grid.mY0 + grid.mH - 1, by * per_bucket + per_bucket);
                        for (S32 y = y0; y <= y1; ++y)
                        {
                            for (S32 x = x0; x <= x1; ++x)
                            {
                                const U32 c = grid.mCell[static_cast<size_t>(y - grid.mY0) * grid.mW + (x - grid.mX0)];
                                if (!c)
                                {
                                    continue;
                                }
                                const S32 count = grid.mLists[c - 1];
                                for (S32 k = 0; k < count; ++k)
                                {
                                    planes.insert(grid.mLists[c + k] + SECTION_LOW_Q);
                                    planes.insert(grid.mLists[c + k] + SECTION_HIGH_Q);
                                }
                            }
                        }
                        if (planes.empty())
                        {
                            continue;
                        }
                        auto take = [&](U32 ri) {
                            const sknav::Record& r = *recs[ri];
                            for (S32 q : planes)
                            {
                                const F64 z = sectionPlaneZ(q);
                                if (r.mMin.mdV[VZ] < z && z <= r.mMax.mdV[VZ])
                                {
                                    pairs.emplace_back(ri, q);
                                }
                            }
                        };
                        if (bucket != buckets.end())
                        {
                            for (U32 ri : bucket->second)
                            {
                                take(ri);
                            }
                        }
                        for (U32 ri : large)
                        {
                            take(ri);
                        }
                    }
                }
                std::sort(pairs.begin(), pairs.end());
                pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

                // 6. Pieces, in (record, plane, group) order.
                const S32 li = static_cast<S32>(l);
                auto floor_at = [&s, li](const LLVector2& p, F32& z) { return layerFloorAt(s, li, p, z); };
                for (const auto& pq : pairs)
                {
                    const sknav::Record& rec = *recs[pq.first];
                    const S32 q = pq.second;
                    planes_used.insert(q);
                    const SlicePtr sl = sliceOf(pq.first, q);
                    if (sl->mLoops.empty() && sl->mLines.empty())
                    {
                        continue;
                    }
                    const LLVector2 shift(static_cast<F32>(rec.mOrigin.mdV[VX] - s.mOrigin.mdV[VX]),
                                          static_cast<F32>(rec.mOrigin.mdV[VY] - s.mOrigin.mdV[VY]));
                    const F32 plane = static_cast<F32>(sectionPlaneZ(q) - s.mOrigin.mdV[VZ]);

                    // Sheet rooms: a closed sheet around walkable floor with headroom through the plane is a wall line.
                    std::vector<bool> active(sl->mLoops.size(), true);
                    bool demoted = false;
                    if (sl->mSheet)
                    {
                        // Outer loops before the loops nested in them, so each sees its ancestors' verdicts.
                        std::vector<std::pair<S32, size_t>> order;
                        for (size_t i = 0; i < sl->mLoops.size(); ++i)
                        {
                            S32 depth = 0;
                            for (S32 up = sl->mLoops[i].mParent; up >= 0; up = sl->mLoops[up].mParent)
                            {
                                ++depth;
                            }
                            order.emplace_back(depth, i);
                        }
                        std::sort(order.begin(), order.end());
                        for (const auto& entry : order)
                        {
                            const size_t i = entry.second;
                            // Inside an odd number of kept loops it bounds a hole, such as a thick wall's inner face: floor in it is not filled.
                            S32 kept_above = 0;
                            for (S32 up = sl->mLoops[i].mParent; up >= 0; up = sl->mLoops[up].mParent)
                            {
                                kept_above += active[up] ? 1 : 0;
                            }
                            if (kept_above % 2)
                            {
                                continue;
                            }
                            const SliceLoop& loop = sl->mLoops[i];
                            const LLVector2* pts = &sl->mPts[loop.mFirst];
                            LLVector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
                            for (U32 k = 0; k < loop.mCount; ++k)
                            {
                                lo.set(llmin(lo.mV[VX], pts[k].mV[VX]), llmin(lo.mV[VY], pts[k].mV[VY]));
                                hi.set(llmax(hi.mV[VX], pts[k].mV[VX]), llmax(hi.mV[VY], pts[k].mV[VY]));
                            }
                            lo += shift;
                            hi += shift;
                            const Cell c0 = cellOfLocal(s, lo), c1 = cellOfLocal(s, hi);
                            std::set<U32> seen_polys;
                            bool walkable = false;
                            for (S32 cy = c0.second; cy <= c1.second && !walkable; ++cy)
                            {
                                for (S32 cx = c0.first; cx <= c1.first && !walkable; ++cx)
                                {
                                    const auto it = s.mIndex.find({ cx, cy });
                                    if (it == s.mIndex.end())
                                    {
                                        continue;
                                    }
                                    for (U32 id : it->second)
                                    {
                                        const ScenePoly& poly = s.mPolys[id];
                                        if (poly.mLayer != li || !seen_polys.insert(id).second)
                                        {
                                            continue;
                                        }
                                        std::vector<LLVector2> points = { poly.mCentroid };
                                        for (S32 v = 0; v < poly.mVertCount; ++v)
                                        {
                                            points.push_back(plan(poly.mVerts[v]));
                                        }
                                        for (const LLVector2& p : points)
                                        {
                                            if (p.mV[VX] < lo.mV[VX] || p.mV[VX] > hi.mV[VX] || p.mV[VY] < lo.mV[VY] || p.mV[VY] > hi.mV[VY]
                                                || polyHeightAt(s, poly, p) > plane - SECTION_HEADROOM_M)
                                            {
                                                continue;
                                            }
                                            const LLVector2 local = p - shift;
                                            if (!loopContains(pts, loop.mCount, local))
                                            {
                                                continue;
                                            }
                                            // Outside every loop nested in this one.
                                            bool in_child = false;
                                            for (size_t j = 0; j < sl->mLoops.size() && !in_child; ++j)
                                            {
                                                S32 up = sl->mLoops[j].mParent;
                                                while (up >= 0 && up != static_cast<S32>(i))
                                                {
                                                    up = sl->mLoops[up].mParent;
                                                }
                                                if (j != i && up == static_cast<S32>(i))
                                                {
                                                    in_child = loopContains(&sl->mPts[sl->mLoops[j].mFirst], sl->mLoops[j].mCount, local);
                                                }
                                            }
                                            if (!in_child)
                                            {
                                                walkable = true;
                                                break;
                                            }
                                        }
                                        if (walkable)
                                        {
                                            break;
                                        }
                                    }
                                }
                            }
                            if (walkable)
                            {
                                active[i] = false;
                                demoted = true;
                            }
                        }
                    }
                    const SliceResult* src = sl.get();
                    SliceResult rewound;
                    if (demoted)
                    {
                        rewound = *sl;
                        windByNesting(rewound, &active);
                        src = &rewound;
                    }

                    SectionPiece base;
                    base.mSlice = sl;
                    base.mShift = shift;
                    base.mPlane = q;
                    base.mStyle = rec.mProvenance == sknav::Provenance::BOX ? SectionStyle::PROVISIONAL : SectionStyle::SOLID;
                    base.mSource = rec.mSignature;
                    base.mPart = rec.mPart;
                    base.mRoot = rec.mRoot;
                    for (size_t gi = 0; gi < src->mGroups.size(); ++gi)
                    {
                        const SliceGroup& g = src->mGroups[gi];
                        SectionPiece piece = base;
                        piece.mGroup = static_cast<U16>(gi);
                        piece.mLoops.push_back(g.mOuter);
                        piece.mLoops.insert(piece.mLoops.end(), g.mHoles.begin(), g.mHoles.end());
                        for (U16 id : piece.mLoops)
                        {
                            piece.mWind.push_back(src->mLoops[id].mWind);
                        }
                        piece.mThickness = g.mThickness;
                        piece.mMin = g.mMin + shift;
                        piece.mMax = g.mMax + shift;
                        const LLVector2 extent = g.mMax - g.mMin;
                        if (g.mArea < CLUTTER_AREA_M2 && llmax(extent.mV[VX], extent.mV[VY]) < CLUTTER_EXTENT_M)
                        {
                            piece.mFlags |= PIECE_BLOB;
                        }
                        const SliceLoop& outer = src->mLoops[g.mOuter];
                        std::vector<LLVector2> ring(src->mPts.begin() + outer.mFirst, src->mPts.begin() + outer.mFirst + outer.mCount);
                        for (LLVector2& p : ring)
                        {
                            p += shift;
                        }
                        if (isIsland(ring.data(), static_cast<U32>(ring.size()), g.mThickness, floor_at))
                        {
                            piece.mFlags |= PIECE_ISLAND;
                        }
                        layer.mPieces.push_back(static_cast<U32>(s.mPieces.size()));
                        s.mPieces.push_back(std::move(piece));
                        piece_record.push_back(pq.first);
                    }
                    if (!src->mLines.empty() || demoted)
                    {
                        SectionPiece piece = base;
                        piece.mGroup = 0xffff;
                        piece.mFlags = PIECE_LINES;
                        LLVector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
                        auto grow = [&](const SliceLoop& run) {
                            for (U32 k = 0; k < run.mCount; ++k)
                            {
                                const LLVector2& p = src->mPts[run.mFirst + k];
                                lo.set(llmin(lo.mV[VX], p.mV[VX]), llmin(lo.mV[VY], p.mV[VY]));
                                hi.set(llmax(hi.mV[VX], p.mV[VX]), llmax(hi.mV[VY], p.mV[VY]));
                            }
                        };
                        const size_t nlines = src->mLines.size();
                        for (size_t i = 0; i < nlines; ++i)
                        {
                            piece.mLines.push_back(static_cast<U16>(i));
                            grow(src->mLines[i]);
                        }
                        for (size_t i = 0; i < active.size(); ++i)
                        {
                            if (!active[i])
                            {
                                piece.mLines.push_back(static_cast<U16>(nlines + i));
                                grow(src->mLoops[i]);
                            }
                        }
                        piece.mMin = lo + shift;
                        piece.mMax = hi + shift;
                        layer.mPieces.push_back(static_cast<U32>(s.mPieces.size()));
                        s.mPieces.push_back(std::move(piece));
                        piece_record.push_back(pq.first);
                    }
                }

                // The cover each record gives on this layer, from its pieces' planes and the levels near them.
                for (size_t a = 0; a < layer.mPieces.size();)
                {
                    const U32 ri = piece_record[layer.mPieces[a]];
                    size_t b = a;
                    S32 full = -1, half = -1;
                    bool island = false;
                    for (; b < layer.mPieces.size() && piece_record[layer.mPieces[b]] == ri; ++b)
                    {
                        const SectionPiece& piece = s.mPieces[layer.mPieces[b]];
                        island = island || (piece.mFlags & PIECE_ISLAND);
                        if ((piece.mFlags & (PIECE_BLOB | PIECE_LINES)) || piece.mStyle != SectionStyle::SOLID)
                        {
                            continue;
                        }
                        const LLVector2 grow(LEVEL_CELL_M, LEVEL_CELL_M);
                        if (full < 0 && anyLevelNear(s, grid, piece.mMin - grow, piece.mMax + grow, piece.mPlane - SECTION_HIGH_Q))
                        {
                            full = static_cast<S32>(layer.mPieces[b]);
                        }
                        if (half < 0 && anyLevelNear(s, grid, piece.mMin - grow, piece.mMax + grow, piece.mPlane - SECTION_LOW_Q))
                        {
                            half = static_cast<S32>(layer.mPieces[b]);
                        }
                    }
                    a = b;
                    stats.mIslands += island ? 1 : 0;
                    if (full < 0 && half < 0)
                    {
                        continue;
                    }
                    const bool is_full = full >= 0;
                    (is_full ? stats.mFullRecords : stats.mHalfRecords)++;
                    if (is_full && !input.mSectionReport)
                    {
                        continue;
                    }
                    const SectionPiece& piece = s.mPieces[is_full ? full : half];
                    const sknav::Record& rec = *recs[ri];
                    SectionReport report;
                    report.mFull = is_full;
                    report.mPart = rec.mPart;
                    report.mRoot = rec.mRoot;
                    report.mMin = piece.mMin;
                    report.mMax = piece.mMax;
                    const S32 k = piece.mPlane - (is_full ? SECTION_HIGH_Q : SECTION_LOW_Q);
                    report.mFloor = k * SECTION_CELL_M;
                    report.mLow = sectionPlaneZ(k + SECTION_LOW_Q);
                    report.mHigh = sectionPlaneZ(k + SECTION_HIGH_Q);
                    report.mRecordTop = static_cast<F32>(rec.mMax.mdV[VZ]);
                    report.mThickness = piece.mThickness;
                    report.mLength = llmax(piece.mMax.mV[VX] - piece.mMin.mV[VX], piece.mMax.mV[VY] - piece.mMin.mV[VY]);
                    report.mShape = static_cast<U8>(rec.mShape);
                    report.mProvenance = static_cast<U8>(rec.mProvenance);
                    report.mSolid = rec.mSolid;
                    report.mIsland = (piece.mFlags & PIECE_ISLAND) != 0;
                    // A lintel: the lowest other record over the piece starting above the high plane and within 3 m of the floor.
                    const F64 x0 = s.mOrigin.mdV[VX] + piece.mMin.mV[VX] - 0.05, x1 = s.mOrigin.mdV[VX] + piece.mMax.mV[VX] + 0.05;
                    const F64 y0 = s.mOrigin.mdV[VY] + piece.mMin.mV[VY] - 0.05, y1 = s.mOrigin.mdV[VY] + piece.mMax.mV[VY] + 0.05;
                    auto consider = [&](U32 other) {
                        if (other == ri)
                        {
                            return;
                        }
                        const sknav::Record& o = *recs[other];
                        const F64 bottom = o.mMin.mdV[VZ];
                        if (o.mMax.mdV[VX] < x0 || o.mMin.mdV[VX] > x1 || o.mMax.mdV[VY] < y0 || o.mMin.mdV[VY] > y1 || bottom <= report.mHigh
                            || bottom > report.mFloor + 3.0)
                        {
                            return;
                        }
                        if (report.mLintel < 0.f || bottom < report.mLintel)
                        {
                            report.mLintel = static_cast<F32>(bottom);
                        }
                    };
                    for (S32 by = cellIndex(y0, RECORD_BUCKET_M); by <= cellIndex(y1, RECORD_BUCKET_M); ++by)
                    {
                        for (S32 bx = cellIndex(x0, RECORD_BUCKET_M); bx <= cellIndex(x1, RECORD_BUCKET_M); ++bx)
                        {
                            const auto bucket = buckets.find({ bx, by });
                            if (bucket != buckets.end())
                            {
                                for (U32 other : bucket->second)
                                {
                                    consider(other);
                                }
                            }
                        }
                    }
                    for (U32 other : large)
                    {
                        consider(other);
                    }
                    stats.mHalfOpenings += !is_full && report.mLintel >= 0.f ? 1 : 0;
                    if (input.mSectionReport)
                    {
                        s.mSectionReport.push_back(report);
                    }
                }
            }
            stats.mPlanes = static_cast<U32>(planes_used.size());
            stats.mPieces = static_cast<U32>(s.mPieces.size());
            s.mHasSection = true;
            stats.mMs = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
        }

        /// Index every polygon by the 4 m cells it meets.
        void buildIndex(Scene& scene, std::vector<std::vector<Cell>>& poly_cells)
        {
            poly_cells.assign(scene.mPolys.size(), {});
            for (size_t i = 0; i < scene.mPolys.size(); ++i)
            {
                polyCells(scene, scene.mPolys[i], poly_cells[i]);
                for (const Cell& c : poly_cells[i])
                {
                    scene.mIndex[c].push_back(static_cast<U32>(i));
                }
            }
        }
    }

    std::pair<S32, S32> indexCell(F64 x, F64 y)
    {
        return { static_cast<S32>(std::floor(x / INDEX_CELL_M)), static_cast<S32>(std::floor(y / INDEX_CELL_M)) };
    }

    bool levelListed(const LevelGrid& grid, S32 gx, S32 gy, S32 k)
    {
        const S32 x = gx - grid.mX0, y = gy - grid.mY0;
        if (x < 0 || x >= grid.mW || y < 0 || y >= grid.mH)
        {
            return false;
        }
        const U32 c = grid.mCell[static_cast<size_t>(y) * grid.mW + x];
        if (!c)
        {
            return false;
        }
        const S32 count = grid.mLists[c - 1];
        for (S32 i = 0; i < count; ++i)
        {
            if (grid.mLists[c + i] == k)
            {
                return true;
            }
        }
        return false;
    }

    bool polyContains(const Scene& scene, const ScenePoly& poly, const LLVector2& p)
    {
        if (p.mV[VX] < poly.mMinX - 1e-4f || p.mV[VX] > poly.mMaxX + 1e-4f || p.mV[VY] < poly.mMinY - 1e-4f || p.mV[VY] > poly.mMaxY + 1e-4f)
        {
            return false;
        }
        bool pos = false, neg = false;
        for (S32 i = 0; i < poly.mVertCount; ++i)
        {
            const F32 c = cross2(plan(poly.mVerts[i]), plan(poly.mVerts[(i + 1) % poly.mVertCount]), p);
            pos = pos || c > 1e-5f;
            neg = neg || c < -1e-5f;
        }
        return !(pos && neg);
    }

    F32 polyHeightAt(const Scene& scene, const ScenePoly& poly, const LLVector2& p)
    {
        for (U32 t = 0; t < poly.mTriCount; ++t)
        {
            const LLVector3* tri = &scene.mTris[(poly.mFirstTri + t) * 3];
            const LLVector2 a = plan(tri[0]), b = plan(tri[1]), c = plan(tri[2]);
            const F32 area = cross2(a, b, c);
            if (std::fabs(area) < 1e-8f)
            {
                continue;
            }
            const F32 wa = cross2(b, c, p) / area;
            const F32 wb = cross2(c, a, p) / area;
            const F32 wc = 1.f - wa - wb;
            if (wa >= -1e-4f && wb >= -1e-4f && wc >= -1e-4f)
            {
                return wa * tri[0].mV[VZ] + wb * tri[1].mV[VZ] + wc * tri[2].mV[VZ];
            }
        }
        return poly.mZMean;
    }

    ScenePtr buildScene(const SceneInput& input, SectionCache* cache)
    {
        auto scene = std::make_shared<Scene>();
        Scene& s = *scene;
        s.mOrigin = input.mOrigin;
        s.mMin = input.mMin;
        s.mMax = input.mMax;
        s.mClimb = input.mClimb;

        // Runs first: a polygon in a run is kept however tall it is.
        std::map<std::pair<LLUUID, U64>, S32> run_ids;
        std::vector<bool> run_home;
        std::vector<std::vector<std::vector<S32>>> tile_runs(input.mTiles.size());
        for (size_t ti = 0; ti < input.mTiles.size(); ++ti)
        {
            const TileInput& tile = input.mTiles[ti];
            const size_t count = tile.mPolys ? tile.mPolys->mPolys.size() : 0;
            tile_runs[ti].assign(count, {});
            if (!tile.mFlights)
            {
                continue;
            }
            const LLVector3d offset = tile.mOrigin - s.mOrigin;
            for (const sknav::Flight& f : *tile.mFlights)
            {
                LLVector2 mid;
                for (const LLVector2& p : f.mFootprint)
                {
                    mid += p / static_cast<F32>(llmax(static_cast<size_t>(1), f.mFootprint.size()));
                }
                const F64 gx = tile.mOrigin.mdV[VX] + mid.mV[VX];
                const F64 gy = tile.mOrigin.mdV[VY] + mid.mV[VY];
                const bool home = gx >= tile.mMin.mdV[VX] && gx < tile.mMax.mdV[VX] && gy >= tile.mMin.mdV[VY] && gy < tile.mMax.mdV[VY];
                const auto key = std::make_pair(tile.mMesh, f.mKey);
                auto it = run_ids.find(key);
                S32 id;
                if (it == run_ids.end())
                {
                    id = static_cast<S32>(s.mRuns.size());
                    run_ids.emplace(key, id);
                    s.mRuns.emplace_back();
                    run_home.push_back(false);
                    s.mRuns.back().mMesh = tile.mMesh;
                    s.mRuns.back().mKey = f.mKey;
                }
                else
                {
                    id = it->second;
                }
                SceneRun& run = s.mRuns[id];
                if (it == run_ids.end() || (home && !run_home[id]))
                {
                    const LLVector3 o(static_cast<F32>(offset.mdV[VX]), static_cast<F32>(offset.mdV[VY]), static_cast<F32>(offset.mdV[VZ]));
                    run.mStair = f.mKind == sknav::ConnectorKind::STAIR && f.mTreads >= 2;
                    run.mTreads = f.mTreads;
                    run.mBottomA = f.mBottomA + o;
                    run.mBottomB = f.mBottomB + o;
                    run.mTopA = f.mTopA + o;
                    run.mTopB = f.mTopB + o;
                    run.mRun = f.mRun;
                    run.mRise = f.mRise;
                    run_home[id] = home;
                }
                for (U16 p : f.mPolys)
                {
                    if (p < count)
                    {
                        tile_runs[ti][p].push_back(id);
                    }
                }
            }
        }

        // Polygons, scene-local.
        std::vector<std::vector<S32>> ids(input.mTiles.size());
        for (size_t ti = 0; ti < input.mTiles.size(); ++ti)
        {
            const TileInput& tile = input.mTiles[ti];
            if (!tile.mPolys)
            {
                continue;
            }
            const sknav::TilePolys& tp = *tile.mPolys;
            ids[ti].assign(tp.mPolys.size(), -1);
            const LLVector3d offset = tile.mOrigin - s.mOrigin;
            const LLVector3 o(static_cast<F32>(offset.mdV[VX]), static_cast<F32>(offset.mdV[VY]), static_cast<F32>(offset.mdV[VZ]));
            for (size_t pi = 0; pi < tp.mPolys.size(); ++pi)
            {
                const sknav::TilePolys::Poly& src = tp.mPolys[pi];
                const bool in_run = !tile_runs[ti][pi].empty();
                if (!in_run && src.mZMax - src.mZMin > RUNLESS_Z_SPAN_M)
                {
                    continue;
                }
                if (src.mVertCount < 3)
                {
                    continue;
                }
                ScenePoly poly;
                poly.mTile = static_cast<U32>(ti);
                poly.mIndex = static_cast<U16>(pi);
                poly.mArea = src.mArea;
                poly.mVertCount = src.mVertCount;
                poly.mTerrainTile = tile.mTerrain;
                poly.mWater = tile.mWater - static_cast<F32>(s.mOrigin.mdV[VZ]);
                poly.mMinX = poly.mMinY = FLT_MAX;
                poly.mMaxX = poly.mMaxY = -FLT_MAX;
                F32 zsum = 0.f;
                for (S32 v = 0; v < src.mVertCount; ++v)
                {
                    if (src.mNeighbour[v] < 0)
                    {
                        poly.mOpenEdges |= static_cast<U8>(1 << v);
                    }
                    poly.mVerts[v] = tp.mVerts[src.mVerts[v]] + o;
                    zsum += poly.mVerts[v].mV[VZ];
                    poly.mMinX = llmin(poly.mMinX, poly.mVerts[v].mV[VX]);
                    poly.mMinY = llmin(poly.mMinY, poly.mVerts[v].mV[VY]);
                    poly.mMaxX = llmax(poly.mMaxX, poly.mVerts[v].mV[VX]);
                    poly.mMaxY = llmax(poly.mMaxY, poly.mVerts[v].mV[VY]);
                }
                poly.mZMean = zsum / src.mVertCount;
                poly.mZMin = src.mZMin + o.mV[VZ];
                poly.mZMax = src.mZMax + o.mV[VZ];
                // Shoelace area and centroid.
                F32 a2 = 0.f;
                LLVector2 c;
                const LLVector2 p0 = plan(poly.mVerts[0]);
                for (S32 v = 1; v + 1 < src.mVertCount; ++v)
                {
                    const LLVector2 p1 = plan(poly.mVerts[v]), p2 = plan(poly.mVerts[v + 1]);
                    const F32 t = cross2(p0, p1, p2);
                    a2 += t;
                    c += (p0 + p1 + p2) * (t / 3.f);
                }
                if (std::fabs(a2) > 1e-8f)
                {
                    poly.mCentroid = c / a2;
                }
                else
                {
                    for (S32 v = 0; v < src.mVertCount; ++v)
                    {
                        poly.mCentroid += plan(poly.mVerts[v]) / static_cast<F32>(src.mVertCount);
                    }
                }
                poly.mPlanArea = std::fabs(a2) * 0.5f;
                poly.mFirstTri = static_cast<U32>(s.mTris.size() / 3);
                const size_t first = static_cast<size_t>(src.mFirstTri) * 3;
                for (size_t k = 0; k < static_cast<size_t>(src.mTriCount) * 3 && first + k < tp.mTris.size(); ++k)
                {
                    s.mTris.push_back(tp.mTris[first + k] + o);
                }
                poly.mTriCount = static_cast<U16>((s.mTris.size() / 3) - poly.mFirstTri);
                if (in_run)
                {
                    poly.mRun = tile_runs[ti][pi].front();
                }
                ids[ti][pi] = static_cast<S32>(s.mPolys.size());
                s.mPolys.push_back(poly);
            }
        }
        for (size_t ti = 0; ti < input.mTiles.size(); ++ti)
        {
            for (size_t pi = 0; pi < tile_runs[ti].size(); ++pi)
            {
                if (ids[ti].empty() || ids[ti][pi] < 0)
                {
                    continue;
                }
                for (S32 r : tile_runs[ti][pi])
                {
                    s.mRuns[r].mPolys.push_back(static_cast<U32>(ids[ti][pi]));
                }
            }
        }
        for (SceneRun& run : s.mRuns)
        {
            std::sort(run.mPolys.begin(), run.mPolys.end());
            run.mPolys.erase(std::unique(run.mPolys.begin(), run.mPolys.end()), run.mPolys.end());
        }

        // Stitches: Detour's own neighbours inside a tile, then border edges across lattice lines.
        std::map<std::pair<S32, S32>, std::vector<BorderEdge>> lines;
        for (size_t i = 0; i < s.mPolys.size(); ++i)
        {
            const ScenePoly& poly = s.mPolys[i];
            const sknav::TilePolys::Poly& src = input.mTiles[poly.mTile].mPolys->mPolys[poly.mIndex];
            for (S32 v = 0; v < poly.mVertCount; ++v)
            {
                const LLVector3& a = poly.mVerts[v];
                const LLVector3& b = poly.mVerts[(v + 1) % poly.mVertCount];
                const S16 n = src.mNeighbour[v];
                if (n >= 0)
                {
                    const S32 other = static_cast<size_t>(n) < ids[poly.mTile].size() ? ids[poly.mTile][n] : -1;
                    if (other > static_cast<S32>(i))
                    {
                        SceneStitch st;
                        st.mA = static_cast<U32>(i);
                        st.mB = static_cast<U32>(other);
                        st.mZ = (a.mV[VZ] + b.mV[VZ]) * 0.5f;
                        s.mStitches.push_back(st);
                    }
                    continue;
                }
                if (n != sknav::TilePolys::EDGE_BORDER)
                {
                    continue;
                }
                for (S32 axis = 0; axis < 2; ++axis)
                {
                    if (std::fabs(a.mV[axis] - b.mV[axis]) > LATTICE_TOLERANCE_M)
                    {
                        continue;
                    }
                    const F32 at = (a.mV[axis] + b.mV[axis]) * 0.5f;
                    const F64 global = s.mOrigin.mdV[axis] + at;
                    const S32 line = static_cast<S32>(std::floor(global / LATTICE_M + 0.5));
                    if (std::fabs(global - line * static_cast<F64>(LATTICE_M)) > LATTICE_TOLERANCE_M)
                    {
                        continue;
                    }
                    const S32 along = 1 - axis;
                    BorderEdge e;
                    e.mPoly = static_cast<U32>(i);
                    const bool forward = a.mV[along] <= b.mV[along];
                    e.mLo = forward ? a.mV[along] : b.mV[along];
                    e.mHi = forward ? b.mV[along] : a.mV[along];
                    e.mZLo = forward ? a.mV[VZ] : b.mV[VZ];
                    e.mZHi = forward ? b.mV[VZ] : a.mV[VZ];
                    e.mSide = poly.mCentroid.mV[axis] < at ? -1 : 1;
                    lines[{ axis, line }].push_back(e);
                    break;
                }
            }
        }
        for (auto& entry : lines)
        {
            std::vector<BorderEdge>& edges = entry.second;
            std::sort(edges.begin(), edges.end());
            for (size_t i = 0; i < edges.size(); ++i)
            {
                for (size_t j = i + 1; j < edges.size() && edges[j].mLo < edges[i].mHi - STITCH_OVERLAP_M; ++j)
                {
                    if (edges[i].mSide == edges[j].mSide || edges[i].mPoly == edges[j].mPoly)
                    {
                        continue;
                    }
                    const F32 lo = llmax(edges[i].mLo, edges[j].mLo);
                    const F32 hi = llmin(edges[i].mHi, edges[j].mHi);
                    if (hi - lo <= STITCH_OVERLAP_M)
                    {
                        continue;
                    }
                    const F32 dlo = std::fabs(edges[i].zAt(lo) - edges[j].zAt(lo));
                    const F32 dhi = std::fabs(edges[i].zAt(hi) - edges[j].zAt(hi));
                    SceneStitch st;
                    st.mA = llmin(edges[i].mPoly, edges[j].mPoly);
                    st.mB = llmax(edges[i].mPoly, edges[j].mPoly);
                    st.mStep = dlo > s.mClimb || dhi > s.mClimb;
                    st.mZ = (edges[i].zAt(lo) + edges[i].zAt(hi) + edges[j].zAt(lo) + edges[j].zAt(hi)) * 0.25f;
                    s.mStitches.push_back(st);
                }
            }
        }
        std::sort(s.mStitches.begin(), s.mStitches.end(), [](const SceneStitch& x, const SceneStitch& y) {
            return x.mA != y.mA ? x.mA < y.mA : x.mB != y.mB ? x.mB < y.mB : x.mZ < y.mZ;
        });

        // Surfaces: polygons outside runs joined by edges that do not step.
        const S32 npolys = static_cast<S32>(s.mPolys.size());
        UnionFind polys_uf(npolys);
        for (const SceneStitch& st : s.mStitches)
        {
            if (!st.mStep && s.mPolys[st.mA].mRun < 0 && s.mPolys[st.mB].mRun < 0)
            {
                polys_uf.unite(st.mA, st.mB);
            }
        }
        std::map<S32, S32> surface_of_root;
        for (S32 i = 0; i < npolys; ++i)
        {
            ScenePoly& poly = s.mPolys[i];
            if (poly.mRun >= 0)
            {
                continue;
            }
            const S32 root = polys_uf.find(i);
            auto it = surface_of_root.find(root);
            if (it == surface_of_root.end())
            {
                it = surface_of_root.emplace(root, static_cast<S32>(s.mSurfaces.size())).first;
                s.mSurfaces.emplace_back();
            }
            poly.mSurface = it->second;
            SceneSurface& surface = s.mSurfaces[it->second];
            surface.mPolys.push_back(static_cast<U32>(i));
            surface.mTerrain = surface.mTerrain || (poly.mTerrainTile && poly.mArea == sknav::AREA_GROUND);
        }

        // A run's ends: the surfaces it touches nearest its bottom and its top.
        std::vector<std::vector<std::pair<S32, F32>>> run_contacts(s.mRuns.size());
        for (const SceneStitch& st : s.mStitches)
        {
            if (st.mStep)
            {
                continue;
            }
            const ScenePoly& a = s.mPolys[st.mA];
            const ScenePoly& b = s.mPolys[st.mB];
            if (a.mRun >= 0 && b.mRun < 0)
            {
                run_contacts[a.mRun].emplace_back(b.mSurface, st.mZ);
            }
            else if (b.mRun >= 0 && a.mRun < 0)
            {
                run_contacts[b.mRun].emplace_back(a.mSurface, st.mZ);
            }
        }
        for (size_t r = 0; r < s.mRuns.size(); ++r)
        {
            SceneRun& run = s.mRuns[r];
            const F32 zb = (run.mBottomA.mV[VZ] + run.mBottomB.mV[VZ]) * 0.5f;
            const F32 zt = (run.mTopA.mV[VZ] + run.mTopB.mV[VZ]) * 0.5f;
            std::map<S32, F32> contacts;    // surface -> the joint height nearest an end
            for (const auto& contact : run_contacts[r])
            {
                auto it = contacts.find(contact.first);
                const F32 near_end = llmin(std::fabs(contact.second - zb), std::fabs(contact.second - zt));
                if (it == contacts.end() || near_end < llmin(std::fabs(it->second - zb), std::fabs(it->second - zt)))
                {
                    contacts[contact.first] = contact.second;
                }
            }
            S32 bottom = -1, top = -1;
            F32 best_b = FLT_MAX, best_t = FLT_MAX;
            for (const auto& c : contacts)
            {
                const F32 db = std::fabs(c.second - zb);
                const F32 dt = std::fabs(c.second - zt);
                if (db < best_b)
                {
                    best_b = db;
                    bottom = c.first;
                }
                if (dt < best_t)
                {
                    best_t = dt;
                    top = c.first;
                }
            }
            if (bottom >= 0 && bottom == top)
            {
                // One surface for both ends: it is the end it is nearer, and the other end
                // takes the best of the rest, if any.
                const bool is_bottom = best_b <= best_t;
                S32 other = -1;
                F32 best_o = FLT_MAX;
                for (const auto& c : contacts)
                {
                    if (c.first == bottom)
                    {
                        continue;
                    }
                    const F32 d = std::fabs(c.second - (is_bottom ? zt : zb));
                    if (d < best_o)
                    {
                        best_o = d;
                        other = c.first;
                    }
                }
                if (is_bottom)
                {
                    top = other;
                }
                else
                {
                    top = bottom;
                    bottom = other;
                }
            }
            run.mBottomSurface = bottom;
            run.mTopSurface = top;
            if (bottom < 0 && top < 0 && !run.mPolys.empty())
            {
                // A run touching no floor is a surface of its own, so it still draws.
                const S32 id = static_cast<S32>(s.mSurfaces.size());
                s.mSurfaces.emplace_back();
                s.mSurfaces.back().mPolys = run.mPolys;
                run.mBottomSurface = id;
            }
        }

        for (SceneSurface& surface : s.mSurfaces)
        {
            std::vector<std::pair<F32, F32>> samples;
            for (U32 id : surface.mPolys)
            {
                samples.emplace_back(s.mPolys[id].mZMean, s.mPolys[id].mPlanArea);
                surface.mArea += s.mPolys[id].mPlanArea;
            }
            surface.mMedianZ = weightedMedian(samples);
        }

        // The index, needed from here on for picks; layers are filled in below.
        std::vector<std::vector<Cell>> poly_cells;
        buildIndex(s, poly_cells);

        // Components: surfaces joined by runs and by ladders and elevators, never by drops.
        const S32 nsurfaces = static_cast<S32>(s.mSurfaces.size());
        UnionFind surf_uf(nsurfaces);
        for (const SceneRun& run : s.mRuns)
        {
            if (run.mBottomSurface >= 0 && run.mTopSurface >= 0)
            {
                surf_uf.unite(run.mBottomSurface, run.mTopSurface);
            }
        }
        auto surfaceOfPoly = [&](S32 id) -> S32 {
            if (id < 0)
            {
                return -1;
            }
            const ScenePoly& poly = s.mPolys[id];
            if (poly.mSurface >= 0)
            {
                return poly.mSurface;
            }
            if (poly.mRun >= 0)
            {
                const SceneRun& run = s.mRuns[poly.mRun];
                return run.mBottomSurface >= 0 ? run.mBottomSurface : run.mTopSurface;
            }
            return -1;
        };
        std::vector<std::pair<S32, S32>> link_polys;
        for (const FeatureLink& link : input.mFeatureLinks)
        {
            const S32 a = polyNear(s, link.mFrom, LINK_REACH_M);
            const S32 b = polyNear(s, link.mTo, LINK_REACH_M);
            const S32 sa = surfaceOfPoly(a), sb = surfaceOfPoly(b);
            if (sa >= 0 && sb >= 0)
            {
                surf_uf.unite(sa, sb);
                link_polys.emplace_back(a, b);
            }
        }
        std::map<S32, S32> component_of_root;
        for (S32 i = 0; i < nsurfaces; ++i)
        {
            const S32 root = surf_uf.find(i);
            auto it = component_of_root.find(root);
            if (it == component_of_root.end())
            {
                it = component_of_root.emplace(root, static_cast<S32>(component_of_root.size())).first;
            }
            s.mSurfaces[i].mComponent = it->second;
        }
        s.mComponents = static_cast<S32>(component_of_root.size());

        // Surface footprints, for the touch test.
        std::vector<std::set<Cell>> surface_cells(nsurfaces);
        for (S32 i = 0; i < nsurfaces; ++i)
        {
            for (U32 id : s.mSurfaces[i].mPolys)
            {
                surface_cells[i].insert(poly_cells[id].begin(), poly_cells[id].end());
            }
        }

        // Layers: per component, the terrain surfaces as one ground layer; every other surface
        // grouped greedily by median height where footprints touch, whatever joins it, so a
        // table top a step too high to climb still reads as part of its floor.
        struct Building
        {
            S32 mLayer;
            std::vector<std::pair<F32, F32>> mSamples;
            F32 mMedian;
            std::set<Cell> mCells;
        };
        std::map<S32, S32> ground_of_component;
        std::vector<S32> members;
        for (S32 i = 0; i < nsurfaces; ++i)
        {
            SceneSurface& surface = s.mSurfaces[i];
            if (!surface.mTerrain)
            {
                members.push_back(i);
                continue;
            }
            auto it = ground_of_component.find(surface.mComponent);
            if (it == ground_of_component.end())
            {
                it = ground_of_component.emplace(surface.mComponent, static_cast<S32>(s.mLayers.size())).first;
                s.mLayers.emplace_back();
                s.mLayers.back().mGround = true;
                s.mLayers.back().mComponent = surface.mComponent;
            }
            surface.mLayer = it->second;
        }
        std::stable_sort(members.begin(), members.end(), [&](S32 a, S32 b) { return s.mSurfaces[a].mMedianZ < s.mSurfaces[b].mMedianZ; });
        std::vector<Building> open;
        std::map<Cell, std::vector<S32>> open_cells;   // cell -> the buildings meeting it
        for (S32 i : members)
        {
            const SceneSurface& surface = s.mSurfaces[i];
            std::set<S32> nearby;
            for (const Cell& c : surface_cells[i])
            {
                for (S32 dy = -1; dy <= 1; ++dy)
                {
                    for (S32 dx = -1; dx <= 1; ++dx)
                    {
                        const auto it = open_cells.find({ c.first + dx, c.second + dy });
                        if (it != open_cells.end())
                        {
                            nearby.insert(it->second.begin(), it->second.end());
                        }
                    }
                }
            }
            S32 best = -1;
            F32 best_dz = FLT_MAX;
            for (S32 k : nearby)
            {
                const F32 dz = std::fabs(surface.mMedianZ - open[k].mMedian);
                if (dz < LAYER_GAP_M && dz < best_dz)
                {
                    best_dz = dz;
                    best = k;
                }
            }
            if (best < 0)
            {
                Building b;
                b.mLayer = static_cast<S32>(s.mLayers.size());
                b.mMedian = surface.mMedianZ;
                s.mLayers.emplace_back();
                s.mLayers.back().mComponent = surface.mComponent;
                open.push_back(b);
                best = static_cast<S32>(open.size()) - 1;
            }
            Building& b = open[best];
            b.mSamples.emplace_back(surface.mMedianZ, surface.mArea);
            b.mMedian = weightedMedian(b.mSamples);
            for (const Cell& c : surface_cells[i])
            {
                if (b.mCells.insert(c).second)
                {
                    open_cells[c].push_back(best);
                }
            }
            s.mSurfaces[i].mLayer = b.mLayer;
        }
        for (S32 i = 0; i < nsurfaces; ++i)
        {
            const S32 layer = s.mSurfaces[i].mLayer;
            for (U32 id : s.mSurfaces[i].mPolys)
            {
                if (s.mPolys[id].mRun < 0)
                {
                    s.mPolys[id].mLayer = layer;
                }
            }
        }
        for (size_t r = 0; r < s.mRuns.size(); ++r)
        {
            SceneRun& run = s.mRuns[r];
            const S32 bottom = run.mBottomSurface >= 0 ? s.mSurfaces[run.mBottomSurface].mLayer : -1;
            const S32 top = run.mTopSurface >= 0 ? s.mSurfaces[run.mTopSurface].mLayer : -1;
            run.mLayer = bottom >= 0 ? bottom : top;
            run.mTopLayer = top;
            for (U32 id : run.mPolys)
            {
                if (s.mPolys[id].mRun == static_cast<S32>(r))
                {
                    s.mPolys[id].mLayer = run.mLayer;
                }
            }
            if (run.mLayer >= 0)
            {
                s.mLayers[run.mLayer].mRuns.push_back(static_cast<S32>(r));
                if (top >= 0 && top != run.mLayer)
                {
                    s.mLayerLinks.emplace_back(llmin(run.mLayer, top), llmax(run.mLayer, top));
                }
            }
        }
        for (const auto& lp : link_polys)
        {
            const S32 la = s.mPolys[lp.first].mLayer, lb = s.mPolys[lp.second].mLayer;
            if (la >= 0 && lb >= 0 && la != lb)
            {
                s.mLayerLinks.emplace_back(llmin(la, lb), llmax(la, lb));
            }
        }
        std::sort(s.mLayerLinks.begin(), s.mLayerLinks.end());
        s.mLayerLinks.erase(std::unique(s.mLayerLinks.begin(), s.mLayerLinks.end()), s.mLayerLinks.end());

        // Layer polygons, base heights and footprints.
        std::vector<std::vector<std::pair<F32, F32>>> layer_samples(s.mLayers.size());
        for (size_t i = 0; i < s.mPolys.size(); ++i)
        {
            const ScenePoly& poly = s.mPolys[i];
            if (poly.mLayer < 0)
            {
                continue;
            }
            SceneLayer& layer = s.mLayers[poly.mLayer];
            layer.mPolys.push_back(static_cast<U32>(i));
            layer.mArea += poly.mPlanArea;
            layer.mFootprint.insert(poly_cells[i].begin(), poly_cells[i].end());
            layer_samples[poly.mLayer].emplace_back(poly.mZMean, poly.mPlanArea);
        }
        for (size_t l = 0; l < s.mLayers.size(); ++l)
        {
            s.mLayers[l].mBaseZ = weightedMedian(layer_samples[l]);
        }

        // Draw order: ground first, then ascending base, the larger footprint later on a tie.
        for (size_t l = 0; l < s.mLayers.size(); ++l)
        {
            if (!s.mLayers[l].mPolys.empty())
            {
                s.mOrder.push_back(static_cast<S32>(l));
            }
        }
        std::sort(s.mOrder.begin(), s.mOrder.end(), [&](S32 a, S32 b) {
            const SceneLayer& la = s.mLayers[a];
            const SceneLayer& lb = s.mLayers[b];
            if (la.mGround != lb.mGround)
            {
                return la.mGround;
            }
            if (la.mBaseZ != lb.mBaseZ)
            {
                return la.mBaseZ < lb.mBaseZ;
            }
            if (la.mArea != lb.mArea)
            {
                return la.mArea < lb.mArea;
            }
            return a < b;
        });

        if (input.mRecords)
        {
            buildSection(s, input, cache);
        }
        classifyMarks(s, input);
        sampleContours(s);
        for (ScenePoly& poly : s.mPolys)
        {
            if (input.mCoverage)
            {
                const LLVector3d at = s.mOrigin + LLVector3d(poly.mCentroid.mV[VX], poly.mCentroid.mV[VY], poly.mZMean + 1.f);
                poly.mCoverage = static_cast<U8>(input.mCoverage(at));
            }
        }
        hashCells(s, input);
        for (const TileInput& tile : input.mTiles)
        {
            s.mBuilt.insert({ static_cast<S32>(std::floor((tile.mMin.mdV[VX] + 1.0) / LATTICE_M)),
                              static_cast<S32>(std::floor((tile.mMin.mdV[VY] + 1.0) / LATTICE_M)) });
        }
        return scene;
    }

    U64 cellKey(const Scene& scene, S32 layer, const CellId& cell)
    {
        if (layer < 0 || layer >= static_cast<S32>(scene.mCellHashes.size()))
        {
            return 0;
        }
        const auto it = scene.mCellHashes[layer].find(cell);
        return it != scene.mCellHashes[layer].end() ? it->second : 0;
    }

    FloorHit floorAt(const Scene& scene, const LLVector3d& pos, F32 below, F32 above)
    {
        FloorHit hit;
        const auto it = scene.mIndex.find(indexCell(pos.mdV[VX], pos.mdV[VY]));
        if (it == scene.mIndex.end())
        {
            return hit;
        }
        const LLVector2 p = scene.toLocal(pos);
        const F32 z = static_cast<F32>(pos.mdV[VZ]);
        for (U32 id : it->second)
        {
            const ScenePoly& poly = scene.mPolys[id];
            if (poly.mLayer < 0 || poly.mZMax < z - below || poly.mZMin > z + above || !polyContains(scene, poly, p))
            {
                continue;
            }
            const F32 h = polyHeightAt(scene, poly, p);
            if (h < z - below || h > z + above)
            {
                continue;
            }
            if (!hit.mFound || h > hit.mZ)
            {
                hit.mFound = true;
                hit.mZ = h;
                hit.mPoly = static_cast<S32>(id);
                hit.mLayer = poly.mLayer;
            }
        }
        return hit;
    }

    FloorRelation floorRelation(const Scene& scene, const AgentFloor& agent, const LLVector3d& other, bool z_known)
    {
        if (!z_known)
        {
            return FloorRelation::UNKNOWN;
        }
        const FloorHit hit = floorAt(scene, other, 2.5f, 0.5f);
        if (hit.mFound && agent.mFound)
        {
            const F32 dz = hit.mZ - agent.mZ;
            if ((agent.mLayer >= 0 && hit.mLayer == agent.mLayer) || std::fabs(dz) < FLOOR_SAME_M)
            {
                return FloorRelation::SAME;
            }
            return dz > 0.f ? FloorRelation::ABOVE : FloorRelation::BELOW;
        }
        const F64 dz = other.mdV[VZ] - agent.mPos.mdV[VZ];
        if (dz >= FLOOR_RAW_M)
        {
            return FloorRelation::ABOVE;
        }
        if (dz <= -FLOOR_RAW_M)
        {
            return FloorRelation::BELOW;
        }
        return FloorRelation::SAME;
    }

    LayerSelection selectLayers(const Scene& scene, const AgentFloor& agent, sknav::EnclosureBucket bucket, F32 fade)
    {
        LayerSelection out;
        const F32 za = agent.mFound ? agent.mZ : static_cast<F32>(agent.mPos.mdV[VZ]);
        const Cell start = indexCell(agent.mPos.mdV[VX], agent.mPos.mdV[VY]);
        const LLVector2 p = scene.toLocal(agent.mPos);

        if (bucket == sknav::EnclosureBucket::INDOORS || bucket == sknav::EnclosureBucket::SHELTERED)
        {
            out.mInside = true;
        }
        else if (bucket == sknav::EnclosureBucket::UNKNOWN)
        {
            const auto it = scene.mIndex.find(start);
            if (it != scene.mIndex.end())
            {
                for (U32 id : it->second)
                {
                    const ScenePoly& poly = scene.mPolys[id];
                    if (poly.mLayer >= 0 && scene.mLayers[poly.mLayer].mBaseZ > za + INSIDE_ABOVE_M && polyContains(scene, poly, p))
                    {
                        out.mInside = true;
                        break;
                    }
                }
            }
        }

        if (out.mInside && agent.mLayer >= 0)
        {
            // The storey: the agent's layer and what runs, ladders and elevators reach from it
            // within reach above.
            std::deque<S32> queue;
            out.mStorey.insert(agent.mLayer);
            queue.push_back(agent.mLayer);
            while (!queue.empty())
            {
                const S32 l = queue.front();
                queue.pop_front();
                for (const auto& link : scene.mLayerLinks)
                {
                    const S32 other = link.first == l ? link.second : link.second == l ? link.first : -1;
                    if (other >= 0 && !out.mStorey.count(other) && scene.mLayers[other].mBaseZ <= za + STOREY_REACH_M)
                    {
                        out.mStorey.insert(other);
                        queue.push_back(other);
                    }
                }
            }
            std::vector<S32> above;
            for (size_t l = 0; l < scene.mLayers.size(); ++l)
            {
                if (scene.mLayers[l].mBaseZ > za + INSIDE_ABOVE_M && !out.mStorey.count(static_cast<S32>(l)))
                {
                    above.push_back(static_cast<S32>(l));
                }
            }
            auto covered = [&](const Cell& c) {
                for (S32 l : above)
                {
                    if (scene.mLayers[l].mFootprint.count(c))
                    {
                        return true;
                    }
                }
                return false;
            };
            auto holdsStorey = [&](const Cell& c) {
                const auto it = scene.mIndex.find(c);
                if (it == scene.mIndex.end())
                {
                    return false;
                }
                for (U32 id : it->second)
                {
                    if (out.mStorey.count(scene.mPolys[id].mLayer))
                    {
                        return true;
                    }
                }
                return false;
            };
            // The interior: covered cells of the storey, flood-filled from the agent's.
            std::set<Cell> interior;
            if (!above.empty() && covered(start) && holdsStorey(start))
            {
                std::deque<Cell> cells;
                interior.insert(start);
                cells.push_back(start);
                static const S32 DX[4] = { 1, -1, 0, 0 };
                static const S32 DY[4] = { 0, 0, 1, -1 };
                while (!cells.empty())
                {
                    const Cell c = cells.front();
                    cells.pop_front();
                    for (S32 k = 0; k < 4; ++k)
                    {
                        const Cell n(c.first + DX[k], c.second + DY[k]);
                        if (std::abs(n.first - start.first) > INTERIOR_CAP_CELLS || std::abs(n.second - start.second) > INTERIOR_CAP_CELLS
                            || interior.count(n) || !covered(n) || !holdsStorey(n))
                        {
                            continue;
                        }
                        interior.insert(n);
                        cells.push_back(n);
                    }
                }
            }
            for (S32 l : above)
            {
                std::set<Cell> cut;
                for (const Cell& c : scene.mLayers[l].mFootprint)
                {
                    if (interior.count(c))
                    {
                        cut.insert(c);
                    }
                }
                if (!cut.empty())
                {
                    out.mCut[l] = std::move(cut);
                }
            }
        }

        for (S32 l : scene.mOrder)
        {
            const SceneLayer& layer = scene.mLayers[l];
            if (layer.mGround || std::fabs(layer.mBaseZ - za) <= fade)
            {
                out.mDrawn.push_back(l);
            }
        }
        return out;
    }
}
