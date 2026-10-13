/**
 * @file sknavwallruns.cpp
 * @brief Wall runs: what solid stands beside each walkable floor's boundary, measured from that floor.
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

#include "sknavwallruns.h"

#include "sknavbuild.h"
#include "sknavenclosure.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sknav
{
    namespace
    {
        constexpr F32 OPEN_CEILING_M = 1e9f;
        constexpr F32 ABOVE_EPS_M = 0.02f;      ///< a solid's top must clear the floor by this to stand above it
        constexpr F32 ACROSS_LOW_M = 0.25f;     ///< the far side's space is read this high over the floor, then a metre higher
        constexpr F32 SAME_FLOOR_M = 0.3f;      ///< a gap this close to the floor in the floor's own space is that floor still

        bool airAt(const std::vector<WallColumnGap>& gaps, F32 z)
        {
            for (const WallColumnGap& g : gaps)
            {
                if (g.mFloor <= z && z < g.mCeiling)
                {
                    return true;
                }
            }
            return false;
        }

        /// The top over floor of the solid that rises from about the floor, stacked over gaps too short to pass; 0 for none.
        F32 solidTop(const std::vector<WallColumnGap>& gaps, F32 floor, F32 height)
        {
            // Solid intervals are what the gaps leave: below the first, between each pair, above the last unless open.
            F32 top = -1.f;
            for (size_t i = 0; i <= gaps.size(); ++i)
            {
                const F32 bottom = i == 0 ? -OPEN_CEILING_M : gaps[i - 1].mCeiling;
                const F32 t = i < gaps.size() ? gaps[i].mFloor : OPEN_CEILING_M;
                if (bottom >= OPEN_CEILING_M)
                {
                    break;
                }
                if (top < 0.f)
                {
                    if (t > floor + ABOVE_EPS_M && bottom < floor + WALL_HIGH_PLANE_M)
                    {
                        top = t;
                    }
                    continue;
                }
                if (bottom - top >= height)
                {
                    break;
                }
                top = t;
            }
            return top < 0.f ? 0.f : std::min(top, OPEN_CEILING_M) - floor;
        }

        struct Sample
        {
            WallClass mClass = WallClass::NONE;
            F32 mTop = 0.f;
            F32 mFace = -1.f;
            F32 mFar = -1.f;
            F32 mFloor = 0.f;
            bool mThick = false;
            U32 mAcross = 0;
        };

        struct Edge
        {
            U16 mPoly = 0;
            U16 mA = 0, mB = 0;     ///< tile vertex indices, floor on the left of A to B
            std::vector<LLVector3> mPts;    ///< mA, the sub-segment ends, mB
            std::vector<Sample> mSamples;   ///< one per sub-segment
        };
    }

    WallAirReader::WallAirReader(std::vector<WallAir> tiles, std::function<U32(U16)> space_id)
        : mTiles(std::move(tiles)), mSpaceId(std::move(space_id))
    {
        for (const WallAir& t : mTiles)
        {
            if (t.mAir)
            {
                mCell = t.mAir->mCell;
                break;
            }
        }
    }

    const WallAir* WallAirReader::tileAt(F32 x, F32 y, S32& ix, S32& iy) const
    {
        for (const WallAir& t : mTiles)
        {
            if (!t.mAir)
            {
                continue;
            }
            const TileAir& a = *t.mAir;
            ix = static_cast<S32>(std::floor((x - a.mOrigin.mV[VX]) / a.mCell));
            iy = static_cast<S32>(std::floor((y - a.mOrigin.mV[VY]) / a.mCell));
            if (ix >= 0 && iy >= 0 && ix < a.mWidth && iy < a.mHeight)
            {
                return &t;
            }
        }
        return nullptr;
    }

    bool WallAirReader::column(F32 x, F32 y, std::vector<WallColumnGap>& out) const
    {
        out.clear();
        S32 ix, iy;
        const WallAir* t = tileAt(x, y, ix, iy);
        if (!t)
        {
            return false;
        }
        const TileAir& a = *t->mAir;
        const S32 leaf = findLeafByKey(a, ix, iy);
        if (leaf < 0)
        {
            return false;
        }
        AirColumn col;
        airColumn(a, leaf, col);
        const bool spaces = t->mEnclosure && t->mEnclosure->mAir == t->mAir && t->mEnclosure->mGaps.size() == a.mGaps.size();
        for (U32 k = 0; k < col.mCount; ++k)
        {
            const AirGap& g = a.mGaps[col.mGap[k]];
            WallColumnGap c;
            c.mFloor = a.mOrigin.mV[VZ] + col.floorAt(a, k, ix, iy) * a.mUnit;
            c.mCeiling = g.mCeiling == AIR_CEILING_OPEN ? OPEN_CEILING_M : a.mOrigin.mV[VZ] + g.mCeiling * a.mUnit;
            const U16 s = spaces ? t->mEnclosure->mGaps[col.mGap[k]].mSpace : 0;
            c.mSpace = s && mSpaceId ? mSpaceId(s) : 0;
            c.mWalkable = (g.mFlags & AirGap::AVATAR_FLOOR) != 0;
            out.push_back(c);
        }
        return !out.empty();
    }

    F32 WallAirReader::faceAt(F32 x, F32 y, S32 dir) const
    {
        S32 ix, iy;
        const WallAir* t = tileAt(x, y, ix, iy);
        if (!t)
        {
            return std::nanf("");
        }
        const TileAir& a = *t->mAir;
        const bool along_x = dir < 2;
        const F32 base = along_x ? a.mOrigin.mV[VX] + (ix + ((dir & 1) ? 1 : 0)) * a.mCell : a.mOrigin.mV[VY] + (iy + ((dir & 1) ? 1 : 0)) * a.mCell;
        const F32 sgn = (dir & 1) ? -1.f : 1.f;
        return base + sgn * faceOffset(a.mFaceOffsets, faceKey(ix, iy, a.mWidth, dir)) * a.mCell;
    }

    void buildWallRuns(const dtMeshTile& tile, const std::vector<U32>& poly_space, const WallAirReader& air, const WallRunParams& params,
                       std::vector<WallRun>& out, WallRunStats* stats)
    {
        out.clear();
        if (!tile.header)
        {
            return;
        }
        const F32 cell = air.cell();
        const F32 step = cell * 0.5f;
        auto vert = [&tile](U16 v) { return fromRecast(&tile.verts[v * 3]); };
        std::vector<Edge> edges;
        std::vector<WallColumnGap> gaps;
        for (S32 p = 0; p < tile.header->polyCount; ++p)
        {
            const dtPoly& poly = tile.polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount < 3)
            {
                continue;
            }
            F32 area2 = 0.f;
            for (S32 j = 0; j < poly.vertCount; ++j)
            {
                const LLVector3 a = vert(poly.verts[j]), b = vert(poly.verts[(j + 1) % poly.vertCount]);
                area2 += a.mV[VX] * b.mV[VY] - b.mV[VX] * a.mV[VY];
            }
            for (S32 j = 0; j < poly.vertCount; ++j)
            {
                if (poly.neis[j] != 0)
                {
                    continue;       // a neighbour, or the tile's border
                }
                Edge e;
                e.mPoly = static_cast<U16>(p);
                e.mA = poly.verts[j];
                e.mB = poly.verts[(j + 1) % poly.vertCount];
                if (area2 < 0.f)
                {
                    std::swap(e.mA, e.mB);  // counter-clockwise keeps the floor on the left
                }
                const LLVector3 a = vert(e.mA), b = vert(e.mB);
                const F32 dx = b.mV[VX] - a.mV[VX], dy = b.mV[VY] - a.mV[VY];
                const F32 len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f)
                {
                    continue;
                }
                const LLVector3 n(dy / len, -dx / len, 0.f);    // right of A to B: out of the floor
                const S32 subs = std::max(1, static_cast<S32>(std::ceil(len / WALL_RUN_STRIDE_M)));
                e.mPts.push_back(a);
                for (S32 k = 0; k < subs; ++k)
                {
                    const F32 t = (k + 0.5f) / subs;
                    const LLVector3 at = a + (b - a) * t;
                    Sample s;
                    // The floor measured from: the walkable gap just inside whose floor is nearest the poly's, else any gap.
                    s.mFloor = at.mV[VZ];
                    const LLVector3 in = at - n * step;
                    if (air.column(in.mV[VX], in.mV[VY], gaps))
                    {
                        F32 best = params.mClimb;
                        bool walkable = false;
                        for (const WallColumnGap& g : gaps)
                        {
                            const F32 dz = std::fabs(g.mFloor - at.mV[VZ]);
                            if (dz <= params.mClimb && ((g.mWalkable && !walkable) || (g.mWalkable == walkable && dz <= best)))
                            {
                                best = dz;
                                walkable = g.mWalkable;
                                s.mFloor = g.mFloor;
                            }
                        }
                    }
                    const F32 floor = s.mFloor;
                    // Out from the edge to the first column with solid at either plane.
                    F32 hit = -1.f;
                    for (F32 d = step * 0.5f; d <= params.mReach + 1e-4f; d += step)
                    {
                        const LLVector3 q = at + n * d;
                        if (!air.column(q.mV[VX], q.mV[VY], gaps))
                        {
                            continue;
                        }
                        const bool low = !airAt(gaps, floor + WALL_LOW_PLANE_M), high = !airAt(gaps, floor + WALL_HIGH_PLANE_M);
                        if (!low && !high)
                        {
                            continue;
                        }
                        s.mClass = high ? WallClass::SOLID : WallClass::HATCHED;
                        s.mTop = solidTop(gaps, floor, params.mHeight);
                        hit = d;
                        // The true face, along the axis nearer the normal.
                        const bool along_x = std::fabs(n.mV[VX]) >= std::fabs(n.mV[VY]);
                        const F32 nc = along_x ? n.mV[VX] : n.mV[VY];
                        const S32 dir = along_x ? (nc > 0.f ? 0 : 1) : (nc > 0.f ? 2 : 3);
                        const F32 face = air.faceAt(q.mV[VX], q.mV[VY], dir);
                        if (!std::isnan(face))
                        {
                            const F32 t_face = (face - at.mV[along_x ? VX : VY]) / nc;
                            s.mFace = std::clamp(t_face, 0.f, d + step);
                        }
                        // On through the solid to the first column with air back at the plane it was solid at.
                        const F32 plane = floor + (high ? WALL_HIGH_PLANE_M : WALL_LOW_PLANE_M);
                        F32 last = d;
                        bool ended = false;
                        for (F32 d2 = d + step; d2 <= d + WALL_THICK_M + 1e-4f; d2 += step)
                        {
                            const LLVector3 q2 = at + n * d2;
                            if (!air.column(q2.mV[VX], q2.mV[VY], gaps))
                            {
                                ended = true;   // off the tiles: unknown
                                break;
                            }
                            if (airAt(gaps, plane))
                            {
                                const LLVector3 ql = at + n * last;
                                const F32 back = air.faceAt(ql.mV[VX], ql.mV[VY], dir ^ 1);
                                const F32 t_far = std::isnan(back) ? (last + d2) * 0.5f : (back - at.mV[along_x ? VX : VY]) / nc;
                                s.mFar = std::clamp(t_far, std::max(s.mFace, 0.f), d2);
                                ended = true;
                                break;
                            }
                            last = d2;
                        }
                        s.mThick = !ended;
                        break;
                    }
                    // Beyond: past the solid, or past the floor's own edge, the first gap at the reading heights.
                    const F32 from = hit < 0.f ? step * 0.5f : hit;
                    const U32 own = p < static_cast<S32>(poly_space.size()) ? poly_space[p] : 0;
                    for (const F32 h : { ACROSS_LOW_M, ACROSS_LOW_M + 1.f })
                    {
                        bool found = false;
                        for (F32 d = from; d <= from + params.mAcrossReach + 1e-4f && !found; d += step)
                        {
                            const LLVector3 q = at + n * d;
                            if (!air.column(q.mV[VX], q.mV[VY], gaps))
                            {
                                continue;
                            }
                            for (const WallColumnGap& g : gaps)
                            {
                                const F32 z = floor + h;
                                if (g.mFloor <= z && z < g.mCeiling)
                                {
                                    if (hit < 0.f && g.mSpace == own && std::fabs(g.mFloor - floor) <= SAME_FLOOR_M)
                                    {
                                        break;      // still the floor's own gap
                                    }
                                    s.mAcross = g.mSpace;
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (found)
                        {
                            break;
                        }
                    }
                    e.mSamples.push_back(s);
                    e.mPts.push_back(k + 1 < subs ? a + (b - a) * ((k + 1.f) / subs) : b);
                }
                edges.push_back(std::move(e));
            }
        }

        // Chain the edges head to tail; a chain starts where no edge ends, else anywhere on a loop.
        std::multimap<U16, size_t> from_vert;
        std::vector<U32> ending(tile.header->vertCount, 0);
        for (size_t i = 0; i < edges.size(); ++i)
        {
            from_vert.emplace(edges[i].mA, i);
            ++ending[edges[i].mB];
        }
        std::vector<bool> used(edges.size(), false);
        std::vector<size_t> chain;
        auto emit = [&](const std::vector<size_t>& ch) {
            WallRun run;
            F64 face_sum = 0.0, floor_sum = 0.0, far_sum = 0.0, depth_sum = 0.0;
            U32 face_n = 0, n = 0, far_n = 0, depth_n = 0, thick_n = 0;
            std::map<U32, U32> across_n;
            auto close = [&]() {
                if (run.mLine.size() >= 2)
                {
                    // The far side most of its samples found, a space before none.
                    U32 best_n = 0;
                    for (const auto& [id, count] : across_n)
                    {
                        if (count > best_n || (count == best_n && run.mAcross == 0))
                        {
                            run.mAcross = id;
                            best_n = count;
                        }
                    }
                    run.mFace = face_n ? static_cast<F32>(face_sum / face_n) : -1.f;
                    run.mThick = thick_n > far_n;
                    // A thick run is drawn with no back face, so it has none.
                    run.mFar = far_n && !run.mThick ? static_cast<F32>(far_sum / far_n) : -1.f;
                    run.mDepth = depth_n && !run.mThick ? static_cast<F32>(depth_sum / depth_n) : -1.f;
                    run.mFloorZ = n ? static_cast<F32>(floor_sum / n) : 0.f;
                    std::sort(run.mPolys.begin(), run.mPolys.end());
                    run.mPolys.erase(std::unique(run.mPolys.begin(), run.mPolys.end()), run.mPolys.end());
                    run.mCover = coverClass(run.mTopMax, params.mEyeHeight, params.mClimb);
                    out.push_back(run);
                    if (stats)
                    {
                        ++stats->mRuns[static_cast<S32>(run.mClass)];
                        for (size_t i = 1; i < run.mLine.size(); ++i)
                        {
                            const LLVector3 d = run.mLine[i] - run.mLine[i - 1];
                            stats->mMetres[static_cast<S32>(run.mClass)] += std::sqrt(d.mV[VX] * d.mV[VX] + d.mV[VY] * d.mV[VY]);
                        }
                    }
                }
                run = WallRun();
                face_sum = floor_sum = far_sum = depth_sum = 0.0;
                face_n = n = far_n = depth_n = thick_n = 0;
                across_n.clear();
            };
            const size_t first_run = out.size();
            for (const size_t ei : ch)
            {
                const Edge& e = edges[ei];
                const U32 own = e.mPoly < poly_space.size() ? poly_space[e.mPoly] : 0;
                for (size_t k = 0; k < e.mSamples.size(); ++k)
                {
                    const Sample& s = e.mSamples[k];
                    if (n && (s.mClass != run.mClass || own != run.mSpace))
                    {
                        if (run.mLine.back() != e.mPts[k])
                        {
                            run.mLine.push_back(e.mPts[k]);
                        }
                        close();
                    }
                    if (run.mLine.empty())
                    {
                        run.mLine.push_back(e.mPts[k]);
                    }
                    if (k == 0 && stats)
                    {
                        ++stats->mEdges;
                        for (const Sample& o : e.mSamples)
                        {
                            if (o.mClass != s.mClass)
                            {
                                ++stats->mMixedEdges;
                                break;
                            }
                        }
                    }
                    if (n == 0)
                    {
                        run.mClass = s.mClass;
                        run.mSpace = own;
                        run.mLow = s.mClass != WallClass::NONE;
                        run.mHigh = s.mClass == WallClass::SOLID;
                        run.mTopMin = run.mTopMax = s.mTop;
                    }
                    run.mTopMin = std::min(run.mTopMin, s.mTop);
                    run.mTopMax = std::max(run.mTopMax, s.mTop);
                    if (s.mFace >= 0.f)
                    {
                        face_sum += s.mFace;
                        ++face_n;
                    }
                    if (s.mFar >= 0.f)
                    {
                        far_sum += s.mFar;
                        ++far_n;
                        if (s.mFace >= 0.f)
                        {
                            depth_sum += s.mFar - s.mFace;
                            ++depth_n;
                        }
                    }
                    thick_n += s.mThick ? 1 : 0;
                    floor_sum += s.mFloor;
                    ++across_n[s.mAcross];
                    ++n;
                    run.mPolys.push_back(e.mPoly);
                    // Collinear sub-segment ends add nothing to the line: keep the edge's end and the splits.
                    if (k + 1 == e.mSamples.size())
                    {
                        run.mLine.push_back(e.mPts[k + 1]);
                    }
                }
            }
            close();
            // A closed chain started mid-run: its first and last runs are one.
            const bool loop = !ch.empty() && edges[ch.back()].mB == edges[ch.front()].mA;
            if (loop && out.size() > first_run + 1)
            {
                WallRun& head = out[first_run];
                WallRun& tail = out.back();
                if (head.mClass == tail.mClass && head.mSpace == tail.mSpace)
                {
                    const F32 nh = static_cast<F32>(head.mLine.size()), nt = static_cast<F32>(tail.mLine.size());
                    tail.mAcross = nh > nt ? head.mAcross : tail.mAcross;
                    tail.mLine.insert(tail.mLine.end(), head.mLine.begin() + 1, head.mLine.end());
                    tail.mTopMin = std::min(tail.mTopMin, head.mTopMin);
                    tail.mTopMax = std::max(tail.mTopMax, head.mTopMax);
                    const auto blend = [nh, nt](F32 t, F32 h) { return h < 0.f ? t : t < 0.f ? h : (t * nt + h * nh) / (nt + nh); };
                    tail.mFace = blend(tail.mFace, head.mFace);
                    tail.mFar = blend(tail.mFar, head.mFar);
                    tail.mDepth = blend(tail.mDepth, head.mDepth);
                    tail.mThick = nh > nt ? head.mThick : tail.mThick;
                    tail.mFar = tail.mThick ? -1.f : tail.mFar;
                    tail.mDepth = tail.mThick ? -1.f : tail.mDepth;
                    tail.mFloorZ = (tail.mFloorZ * nt + head.mFloorZ * nh) / (nt + nh);
                    tail.mPolys.insert(tail.mPolys.end(), head.mPolys.begin(), head.mPolys.end());
                    std::sort(tail.mPolys.begin(), tail.mPolys.end());
                    tail.mPolys.erase(std::unique(tail.mPolys.begin(), tail.mPolys.end()), tail.mPolys.end());
                    tail.mCover = std::max(tail.mCover, head.mCover);
                    if (stats)
                    {
                        --stats->mRuns[static_cast<S32>(head.mClass)];
                    }
                    out.erase(out.begin() + first_run);
                }
            }
        };
        auto walk = [&](size_t first) {
            chain.clear();
            size_t cur = first;
            while (true)
            {
                used[cur] = true;
                chain.push_back(cur);
                size_t next = edges.size();
                const auto range = from_vert.equal_range(edges[cur].mB);
                for (auto it = range.first; it != range.second; ++it)
                {
                    if (!used[it->second])
                    {
                        next = it->second;
                        break;
                    }
                }
                if (next == edges.size())
                {
                    break;
                }
                cur = next;
            }
            emit(chain);
        };
        for (size_t i = 0; i < edges.size(); ++i)
        {
            if (!used[i] && ending[edges[i].mA] == 0)
            {
                walk(i);
            }
        }
        for (size_t i = 0; i < edges.size(); ++i)
        {
            if (!used[i])
            {
                walk(i);
            }
        }
    }

    const char* wallClassName(WallClass c)
    {
        static const char* const NAMES[] = { "none", "hatched", "solid" };
        return NAMES[static_cast<S32>(c)];
    }
}
