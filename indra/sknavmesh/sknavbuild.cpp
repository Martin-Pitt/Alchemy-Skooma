/**
 * @file sknavbuild.cpp
 * @brief One navmesh tile from census records and terrain: the Recast pipeline, pure and thread-free.
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

#include "sknavbuild.h"
#include "sknavbuildstats.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"
#include "RecastAlloc.h"
#include "sknavflights.h"
#include "sknavlayout.h"
#include "sknavwalls.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace
{
    using namespace sknav;

    const F32 VOID_Z = -900.f;      ///< terrain samples below this have no region
    const S32 CYLINDER_SIDES = 16;
    const S32 SPHERE_RINGS = 6;
    const S32 SPHERE_SEGMENTS = 12;
    const F32 HEIGHT_SNAP = 1024.f;  ///< vertex heights snap to 1/1024 m, exact in binary

    void tri(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c)
    {
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    }

    // a, b, c, d counter-clockwise seen from outside.
    void quad(std::vector<LLVector3>& out, const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
    {
        tri(out, a, b, c);
        tri(out, a, c, d);
    }

    LLVector3 at(const Record& r, F32 u, F32 v, F32 w)
    {
        return r.mCenter + r.mAxes[0] * (u * r.mHalf[0]) + r.mAxes[1] * (v * r.mHalf[1]) + r.mAxes[2] * (w * r.mHalf[2]);
    }

    void emitBox(const Record& r, std::vector<LLVector3>& out)
    {
        const LLVector3 c000 = at(r, -1, -1, -1), c100 = at(r, 1, -1, -1), c110 = at(r, 1, 1, -1), c010 = at(r, -1, 1, -1);
        const LLVector3 c001 = at(r, -1, -1, 1), c101 = at(r, 1, -1, 1), c111 = at(r, 1, 1, 1), c011 = at(r, -1, 1, 1);
        quad(out, c001, c101, c111, c011); // +w (top when upright)
        quad(out, c000, c010, c110, c100); // -w
        quad(out, c000, c100, c101, c001); // -v
        quad(out, c010, c011, c111, c110); // +v
        quad(out, c000, c001, c011, c010); // -u
        quad(out, c100, c110, c111, c101); // +u
    }

    void emitCylinder(const Record& r, std::vector<LLVector3>& out)
    {
        const LLVector3 top = at(r, 0, 0, 1), bottom = at(r, 0, 0, -1);
        for (S32 i = 0; i < CYLINDER_SIDES; ++i)
        {
            const F32 a0 = F_TWO_PI * i / CYLINDER_SIDES, a1 = F_TWO_PI * (i + 1) / CYLINDER_SIDES;
            const F32 u0 = std::cos(a0), v0 = std::sin(a0), u1 = std::cos(a1), v1 = std::sin(a1);
            const LLVector3 b0 = at(r, u0, v0, -1), b1 = at(r, u1, v1, -1), t0 = at(r, u0, v0, 1), t1 = at(r, u1, v1, 1);
            quad(out, b0, b1, t1, t0);      // side, outward
            tri(out, top, t0, t1);          // cap, up
            tri(out, bottom, b1, b0);       // base, down
        }
    }

    void emitSphere(const Record& r, std::vector<LLVector3>& out)
    {
        for (S32 ring = 0; ring < SPHERE_RINGS; ++ring)
        {
            const F32 p0 = F_PI * ring / SPHERE_RINGS - F_PI_BY_TWO, p1 = F_PI * (ring + 1) / SPHERE_RINGS - F_PI_BY_TWO;
            for (S32 seg = 0; seg < SPHERE_SEGMENTS; ++seg)
            {
                const F32 t0 = F_TWO_PI * seg / SPHERE_SEGMENTS, t1 = F_TWO_PI * (seg + 1) / SPHERE_SEGMENTS;
                const LLVector3 a = at(r, std::cos(p0) * std::cos(t0), std::cos(p0) * std::sin(t0), std::sin(p0));
                const LLVector3 b = at(r, std::cos(p0) * std::cos(t1), std::cos(p0) * std::sin(t1), std::sin(p0));
                const LLVector3 c = at(r, std::cos(p1) * std::cos(t1), std::cos(p1) * std::sin(t1), std::sin(p1));
                const LLVector3 d = at(r, std::cos(p1) * std::cos(t0), std::cos(p1) * std::sin(t0), std::sin(p1));
                quad(out, a, b, c, d);
            }
        }
    }

    struct Soup
    {
        std::vector<F32> mVerts;      // Recast space
        std::vector<S32> mTris;
        std::vector<U8> mAreas;       // per triangle, the Area of its source
        std::vector<U8> mTwoSided;    // per triangle, walkable from either side
        std::vector<U8> mCues;        // per triangle, its object's NameCue

        void add(const std::vector<LLVector3>& tris, U8 area, F32 water, bool two_sided, U8 cue = CUE_NONE)
        {
            const size_t base = mVerts.size() / 3;
            for (size_t i = 0; i + 2 < tris.size(); i += 3)
            {
                F32 p[3];
                bool under = true;
                for (S32 k = 0; k < 3; ++k)
                {
                    toRecast(tris[i + k], p);
                    // Heights are SL heights, where a float resolves a quarter of a
                    // millimetre, and a rotated mesh leaves a flat floor a step or so
                    // either side of its height. On a cell boundary that noise rounds
                    // the floor up a cell in one tile and not in the next, since
                    // each clips the triangle at its own cell edges. Snapped to a
                    // millimetre grid a flat surface is exactly flat, and clipping
                    // it gives the same height in every tile.
                    p[1] = std::round(p[1] * HEIGHT_SNAP) / HEIGHT_SNAP;
                    mVerts.insert(mVerts.end(), p, p + 3);
                    under = under && tris[i + k].mV[VZ] < water;
                }
                const S32 v = static_cast<S32>(base + i);
                mTris.insert(mTris.end(), { v, v + 1, v + 2 });
                mAreas.push_back(under && area == AREA_GROUND ? AREA_SUBMERGED : area);
                mTwoSided.push_back(two_sided);
                mCues.push_back(cue);
            }
        }
        S32 nverts() const { return static_cast<S32>(mVerts.size() / 3); }
        S32 ntris() const { return static_cast<S32>(mTris.size() / 3); }
    };

    // A triangle range of the soup a class builds without.
    struct Excluded
    {
        S32 mBegin;
        S32 mEnd;
        U8 mClasses;
    };

    // The soup without the ranges the class leaves out, and the solid ranges
    // renumbered into it. A solid lies within one record, so it is kept or
    // dropped whole. Leaving the triangles out is the only way: a triangle
    // given no area still rasterises as an obstacle.
    void filterSoup(const Soup& soup, const std::vector<std::pair<S32, S32>>& solids, const std::vector<Excluded>& excluded,
                    U8 bit, Soup& out, std::vector<std::pair<S32, S32>>& out_solids)
    {
        std::vector<S32> removed_before(soup.ntris() + 1, 0);
        std::vector<U8> drop(soup.ntris(), 0);
        for (const Excluded& e : excluded)
        {
            if (e.mClasses & bit)
            {
                std::fill(drop.begin() + e.mBegin, drop.begin() + e.mEnd, 1);
            }
        }
        for (S32 t = 0; t < soup.ntris(); ++t)
        {
            removed_before[t + 1] = removed_before[t] + drop[t];
            if (drop[t])
            {
                continue;
            }
            const S32 base = out.nverts();
            for (S32 k = 0; k < 3; ++k)
            {
                const F32* v = &soup.mVerts[soup.mTris[t * 3 + k] * 3];
                out.mVerts.insert(out.mVerts.end(), v, v + 3);
            }
            out.mTris.insert(out.mTris.end(), { base, base + 1, base + 2 });
            out.mAreas.push_back(soup.mAreas[t]);
            out.mTwoSided.push_back(soup.mTwoSided[t]);
            out.mCues.push_back(soup.mCues[t]);
        }
        for (const auto& range : solids)
        {
            if (range.first < range.second && !drop[range.first])
            {
                out_solids.emplace_back(range.first - removed_before[range.first], range.second - removed_before[range.second]);
            }
        }
    }

    // Mark walkable by slope, keeping the source area; non-walkable stays
    // RC_NULL_AREA and still rasterises as an obstacle. A double-sided surface
    // is walkable whichever way its winding points: the sim collides with a
    // hull from either side, so a down-facing top is still the floor.
    void markWalkable(const Soup& soup, F32 slope_deg, std::vector<U8>& out)
    {
        const F32 threshold = std::cos(slope_deg * DEG_TO_RAD);
        out.assign(soup.ntris(), RC_NULL_AREA);
        for (S32 i = 0; i < soup.ntris(); ++i)
        {
            const F32* a = &soup.mVerts[soup.mTris[i * 3] * 3];
            const F32* b = &soup.mVerts[soup.mTris[i * 3 + 1] * 3];
            const F32* c = &soup.mVerts[soup.mTris[i * 3 + 2] * 3];
            F32 e0[3], e1[3], n[3];
            rcVsub(e0, b, a);
            rcVsub(e1, c, a);
            rcVcross(n, e0, e1);
            rcVnormalize(n);
            const F32 up = soup.mTwoSided[i] ? std::fabs(n[1]) : n[1];
            if (up > threshold)
            {
                out[i] = soup.mAreas[i];
            }
        }
    }

    /// The height range a closed body covers in each column of its footprint:
    /// mLo and mHi, indexed from (mX0, mZ0) over mW by mH cells, FLT_MAX and
    /// -FLT_MAX where the body does not pass over a column's centre.
    struct BodyExtent
    {
        S32 mX0 = 0;
        S32 mZ0 = 0;
        S32 mW = 0;
        S32 mH = 0;
        std::vector<F32> mLo;
        std::vector<F32> mHi;
    };

    void bodyExtent(const rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end, BodyExtent& out, F32 pad = 0.f)
    {
        out = BodyExtent();
        if (tri_end <= tri_begin)
        {
            return;
        }
        // The columns this body covers, clipped to the tile.
        F32 bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (S32 t = tri_begin * 3; t < tri_end * 3; ++t)
        {
            const F32* v = &soup.mVerts[soup.mTris[t] * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                bmin[k] = std::min(bmin[k], v[k]);
                bmax[k] = std::max(bmax[k], v[k]);
            }
        }
        bmin[0] -= pad;
        bmin[2] -= pad;
        bmax[0] += pad;
        bmax[2] += pad;
        const S32 x0 = std::max(0, static_cast<S32>(std::floor((bmin[0] - hf.bmin[0]) / hf.cs)));
        const S32 x1 = std::min(hf.width - 1, static_cast<S32>(std::floor((bmax[0] - hf.bmin[0]) / hf.cs)));
        const S32 z0 = std::max(0, static_cast<S32>(std::floor((bmin[2] - hf.bmin[2]) / hf.cs)));
        const S32 z1 = std::min(hf.height - 1, static_cast<S32>(std::floor((bmax[2] - hf.bmin[2]) / hf.cs)));
        if (x1 < x0 || z1 < z0)
        {
            return;
        }
        out.mX0 = x0;
        out.mZ0 = z0;
        out.mW = x1 - x0 + 1;
        out.mH = z1 - z0 + 1;
        out.mLo.assign(static_cast<size_t>(out.mW) * out.mH, FLT_MAX);
        out.mHi.assign(static_cast<size_t>(out.mW) * out.mH, -FLT_MAX);

        for (S32 t = tri_begin; t < tri_end; ++t)
        {
            const F32* a = &soup.mVerts[soup.mTris[t * 3] * 3];
            const F32* b = &soup.mVerts[soup.mTris[t * 3 + 1] * 3];
            const F32* c = &soup.mVerts[soup.mTris[t * 3 + 2] * 3];
            // Its footprint, and its plane, in the xz ground plane.
            const F32 d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(d) < 1e-9f)
            {
                continue;   // edge on: it adds nothing a neighbouring face does not
            }
            const S32 tx0 = std::max(x0, static_cast<S32>(std::floor((std::min(std::min(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tx1 = std::min(x1, static_cast<S32>(std::floor((std::max(std::max(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tz0 = std::max(z0, static_cast<S32>(std::floor((std::min(std::min(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            const S32 tz1 = std::min(z1, static_cast<S32>(std::floor((std::max(std::max(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            for (S32 cz = tz0; cz <= tz1; ++cz)
            {
                for (S32 cx = tx0; cx <= tx1; ++cx)
                {
                    const F32 px = hf.bmin[0] + (cx + 0.5f) * hf.cs;
                    const F32 pz = hf.bmin[2] + (cz + 0.5f) * hf.cs;
                    const F32 l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                    const F32 l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                    const F32 l2 = 1.f - l0 - l1;
                    if (l0 < -pad || l1 < -pad || l2 < -pad)
                    {
                        continue;
                    }
                    const F32 y = l0 * a[1] + l1 * b[1] + l2 * c[1];
                    const size_t i = static_cast<size_t>(cz - z0) * out.mW + (cx - x0);
                    out.mLo[i] = std::min(out.mLo[i], y);
                    out.mHi[i] = std::max(out.mHi[i], y);
                }
            }
        }
    }

    /// Recast rasterises surfaces, so a closed body arrives hollow: the floor
    /// it stands on stays walkable inside it with the body's own roof as
    /// headroom, and the navmesh offers a storey inside every solid object.
    /// Every body filled here is one the sim collides with as a volume - a
    /// prim, a hull, a sculpt's hull - so its inside is filled whatever it
    /// holds: a surface in there is another record's, or the terrain, and no
    /// avatar stands on it.
    ///
    /// The inside is what the column's centre crosses an odd number of
    /// surfaces to reach, so the crossings are taken in pairs from the bottom
    /// up. A convex body is one pair. A box with a tunnel through it is two,
    /// and the tunnel's floor stays a floor; a hollow prim's hollow is open
    /// the same way. Facing is never consulted: a hull's winding is whatever
    /// the decomposition left it, and the count is the same either way.
    /// A column the body only touches is odd, and is filled from its lowest
    /// crossing to its highest, which is what a convex body would have been.
    void fillSolid(rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end, S32 climb)
    {
        if (tri_end <= tri_begin)
        {
            return;
        }
        F32 bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (S32 t = tri_begin * 3; t < tri_end * 3; ++t)
        {
            const F32* v = &soup.mVerts[soup.mTris[t] * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                bmin[k] = std::min(bmin[k], v[k]);
                bmax[k] = std::max(bmax[k], v[k]);
            }
        }
        const S32 x0 = std::max(0, static_cast<S32>(std::floor((bmin[0] - hf.bmin[0]) / hf.cs)));
        const S32 x1 = std::min(hf.width - 1, static_cast<S32>(std::floor((bmax[0] - hf.bmin[0]) / hf.cs)));
        const S32 z0 = std::max(0, static_cast<S32>(std::floor((bmin[2] - hf.bmin[2]) / hf.cs)));
        const S32 z1 = std::min(hf.height - 1, static_cast<S32>(std::floor((bmax[2] - hf.bmin[2]) / hf.cs)));
        if (x1 < x0 || z1 < z0)
        {
            return;
        }
        const S32 w = x1 - x0 + 1;

        // Every surface crossing, by column then height.
        struct Hit
        {
            S32 mColumn;
            F32 mY;
            bool operator<(const Hit& o) const { return mColumn < o.mColumn || (mColumn == o.mColumn && mY < o.mY); }
        };
        std::vector<Hit> hits;
        // A centre on a shared edge is inside both triangles; the two crossings
        // it yields sit at one height and are merged below. The tolerance keeps
        // a centre a rounding error outside both from being in neither.
        const F32 eps = 1e-4f;
        for (S32 t = tri_begin; t < tri_end; ++t)
        {
            const F32* a = &soup.mVerts[soup.mTris[t * 3] * 3];
            const F32* b = &soup.mVerts[soup.mTris[t * 3 + 1] * 3];
            const F32* c = &soup.mVerts[soup.mTris[t * 3 + 2] * 3];
            const F32 d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(d) < 1e-9f)
            {
                continue;   // vertical: the column crosses it nowhere; the faces it joins count
            }
            const S32 tx0 = std::max(x0, static_cast<S32>(std::floor((std::min(std::min(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tx1 = std::min(x1, static_cast<S32>(std::floor((std::max(std::max(a[0], b[0]), c[0]) - hf.bmin[0]) / hf.cs)));
            const S32 tz0 = std::max(z0, static_cast<S32>(std::floor((std::min(std::min(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            const S32 tz1 = std::min(z1, static_cast<S32>(std::floor((std::max(std::max(a[2], b[2]), c[2]) - hf.bmin[2]) / hf.cs)));
            for (S32 cz = tz0; cz <= tz1; ++cz)
            {
                for (S32 cx = tx0; cx <= tx1; ++cx)
                {
                    const F32 px = hf.bmin[0] + (cx + 0.5f) * hf.cs;
                    const F32 pz = hf.bmin[2] + (cz + 0.5f) * hf.cs;
                    const F32 l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                    const F32 l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                    const F32 l2 = 1.f - l0 - l1;
                    if (l0 < -eps || l1 < -eps || l2 < -eps)
                    {
                        continue;
                    }
                    hits.push_back({ (cz - z0) * w + (cx - x0), l0 * a[1] + l1 * b[1] + l2 * c[1] });
                }
            }
        }
        std::sort(hits.begin(), hits.end());

        rcContext ctx(false);
        auto fill = [&](S32 column, F32 lo, F32 hi)
        {
            const F32 top = std::min(hi, hf.bmax[1]);
            const F32 bottom = std::max(lo, hf.bmin[1]);
            if (top <= bottom)
            {
                return;
            }
            // Spans are cell counts in an unsigned short and Recast caps
            // them; a megaprim reaching a kilometre would otherwise wrap.
            S32 smin = static_cast<S32>(std::floor((bottom - hf.bmin[1]) / hf.ch));
            S32 smax = static_cast<S32>(std::ceil((top - hf.bmin[1]) / hf.ch));
            smin = llclamp(smin, 0, RC_SPAN_MAX_HEIGHT);
            smax = llclamp(smax, 0, RC_SPAN_MAX_HEIGHT);
            if (smax <= smin)
            {
                return;
            }
            // The fill carries no area: it is the inside of something, never
            // a floor. Merging keeps the surfaces' own area, so the body's
            // roof stays walkable.
            rcAddSpan(&ctx, hf, x0 + column % w, z0 + column / w, static_cast<unsigned short>(smin),
                      static_cast<unsigned short>(smax), RC_NULL_AREA, climb);
        };
        // The same surface met at a shared edge or vertex is one crossing.
        const F32 same = 1e-3f;
        std::vector<F32> column;
        size_t i = 0;
        while (i < hits.size())
        {
            const S32 at = hits[i].mColumn;
            column.clear();
            for (; i < hits.size() && hits[i].mColumn == at; ++i)
            {
                if (column.empty() || hits[i].mY - column.back() > same)
                {
                    column.push_back(hits[i].mY);
                }
            }
            if (column.size() % 2)
            {
                fill(at, column.front(), column.back());
                continue;
            }
            for (size_t k = 0; k + 1 < column.size(); k += 2)
            {
                fill(at, column[k], column[k + 1]);
            }
        }
    }

    /// Take the navmesh out of a volume. The sim's exclusion volumes are holes
    /// in its own mesh, so they are never a surface to stand on: every walkable
    /// span that overlaps the volume in a column it covers is cleared.
    void cutVolume(rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end)
    {
        BodyExtent body;
        bodyExtent(hf, soup, tri_begin, tri_end, body);
        for (S32 cz = body.mZ0; cz < body.mZ0 + body.mH; ++cz)
        {
            for (S32 cx = body.mX0; cx < body.mX0 + body.mW; ++cx)
            {
                const size_t i = static_cast<size_t>(cz - body.mZ0) * body.mW + (cx - body.mX0);
                if (body.mLo[i] > body.mHi[i])
                {
                    continue;
                }
                const S32 smin = static_cast<S32>(std::floor((body.mLo[i] - hf.bmin[1]) / hf.ch));
                const S32 smax = static_cast<S32>(std::ceil((body.mHi[i] - hf.bmin[1]) / hf.ch));
                for (rcSpan* s = hf.spans[cx + cz * hf.width]; s; s = s->next)
                {
                    if (s->area != RC_NULL_AREA && static_cast<S32>(s->smin) < smax && static_cast<S32>(s->smax) > smin)
                    {
                        s->area = RC_NULL_AREA;
                    }
                }
            }
        }
    }

    /// A rectangle of heightfield cells, half-open.
    struct CellRect
    {
        S32 mX0 = 0;
        S32 mZ0 = 0;
        S32 mX1 = 0;
        S32 mZ1 = 0;
        bool covers(const rcHeightfield& hf) const { return mX0 <= 0 && mZ0 <= 0 && mX1 >= hf.width && mZ1 >= hf.height; }
    };

    /// What a raster is made of: the soup with an area code per triangle, its
    /// closed bodies, the exclusion volumes cut out of it, and optionally the
    /// triangles to leave out.
    struct RasterSource
    {
        const Soup* mSoup = nullptr;
        const std::vector<U8>* mCodes = nullptr;
        const std::vector<std::pair<S32, S32>>* mSolids = nullptr;
        const Soup* mCutSoup = nullptr;
        const std::vector<std::pair<S32, S32>>* mCuts = nullptr;
        const std::vector<U8>* mDrop = nullptr;
    };

    /// The cells a triangle range's bounding box covers, a cell wider each way.
    CellRect cellsOf(const rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end)
    {
        F32 lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (S32 t = tri_begin * 3; t < tri_end * 3; ++t)
        {
            const F32* v = &soup.mVerts[soup.mTris[t] * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
        }
        return { static_cast<S32>(std::floor((lo[0] - hf.bmin[0]) / hf.cs)) - 1, static_cast<S32>(std::floor((lo[2] - hf.bmin[2]) / hf.cs)) - 1,
                 static_cast<S32>(std::floor((hi[0] - hf.bmin[0]) / hf.cs)) + 2, static_cast<S32>(std::floor((hi[2] - hf.bmin[2]) / hf.cs)) + 2 };
    }

    bool overlaps(const CellRect& a, const CellRect& b)
    {
        return a.mX0 < b.mX1 && b.mX0 < a.mX1 && a.mZ0 < b.mZ1 && b.mZ0 < a.mZ1;
    }

    /// Rasterise, fill and cut, in that order, every triangle, body and cut of
    /// the source that touches the rectangle. The columns inside it come out
    /// exactly as a raster of the whole source would make them, provided the
    /// heightfield has the same grid; columns outside it are incomplete.
    bool rasterisePasses(rcHeightfield& hf, const RasterSource& src, const CellRect& rect, S32 climb, StageClock& clock)
    {
        rcContext ctx(false);
        const Soup& soup = *src.mSoup;
        const bool whole = rect.covers(hf);
        if (whole && !src.mDrop)
        {
            if (!rcRasterizeTriangles(&ctx, soup.mVerts.data(), soup.nverts(), soup.mTris.data(), src.mCodes->data(), soup.ntris(), hf, climb))
            {
                return false;
            }
        }
        else
        {
            std::vector<S32> tris;
            std::vector<U8> codes;
            for (S32 t = 0; t < soup.ntris(); ++t)
            {
                if ((src.mDrop && (*src.mDrop)[t]) || (!whole && !overlaps(rect, cellsOf(hf, soup, t, t + 1))))
                {
                    continue;
                }
                tris.insert(tris.end(), soup.mTris.begin() + t * 3, soup.mTris.begin() + t * 3 + 3);
                codes.push_back((*src.mCodes)[t]);
            }
            if (!tris.empty() && !rcRasterizeTriangles(&ctx, soup.mVerts.data(), soup.nverts(), tris.data(), codes.data(),
                                                       static_cast<S32>(codes.size()), hf, climb))
            {
                return false;
            }
        }
        clock.lap(ClassBuildStats::RASTERISE);
        for (const auto& range : *src.mSolids)
        {
            if ((src.mDrop && range.first < range.second && (*src.mDrop)[range.first]) ||
                (!whole && !overlaps(rect, cellsOf(hf, soup, range.first, range.second))))
            {
                continue;
            }
            fillSolid(hf, soup, range.first, range.second, climb);
        }
        clock.lap(ClassBuildStats::FILL);
        for (const auto& range : *src.mCuts)
        {
            cutVolume(hf, *src.mCutSoup, range.first, range.second);
        }
        clock.lap(ClassBuildStats::CUT);
        return true;
    }

    /// A span of a surface only the air's slope walks becomes none, as
    /// though AVATAR had rasterised it unwalkable.
    void maskTopOnly(rcHeightfield& hf)
    {
        for (S32 c = 0; c < hf.width * hf.height; ++c)
        {
            for (rcSpan* s = hf.spans[c]; s; s = s->next)
            {
                if (s->area < AREA_CODE_BASE)
                {
                    s->area = RC_NULL_AREA;
                }
            }
        }
    }

    /// Move a partition's flights onto the codes encodeAreas gives their triangles.
    void shiftFlightCodes(SlopePartition& slopes)
    {
        for (Flight& f : slopes.mFlights)
        {
            for (U8& code : f.mCodes)
            {
                llassert(code >= 1 && code + AREA_TOP_ONLY <= AREA_CODE_MAX);
                code = static_cast<U8>(code + AREA_TOP_ONLY);
            }
        }
    }

    /// The area byte each triangle rasterises as. A merge keeps the higher of
    /// two codes when their tops meet, so AVATAR's codes sit above every
    /// TOP-only one: masking 1-3 to none after the merge leaves exactly the
    /// raster AVATAR would have made on its own.
    std::vector<U8> encodeAreas(const std::vector<U8>& avatar_walk, const std::vector<U8>& top_walk, const std::vector<U8>* codes)
    {
        std::vector<U8> out(avatar_walk.size(), RC_NULL_AREA);
        for (size_t t = 0; t < out.size(); ++t)
        {
            if (avatar_walk[t])
            {
                out[t] = static_cast<U8>((codes ? (*codes)[t] : avatar_walk[t]) + AREA_TOP_ONLY);
            }
            else if (top_walk[t])
            {
                out[t] = top_walk[t];
            }
        }
        return out;
    }

    /// The real area of every area byte encodeAreas makes, from the partition's own table.
    std::vector<U8> realAreas(const std::vector<U8>* partition_real)
    {
        std::vector<U8> out(AREA_CODE_MAX + 1, RC_NULL_AREA);
        for (S32 c = 1; c <= AREA_CODE_MAX; ++c)
        {
            const S32 code = c - AREA_TOP_ONLY;
            if (c <= AREA_TOP_ONLY)
            {
                out[c] = static_cast<U8>(c);
            }
            else if (!partition_real)
            {
                out[c] = static_cast<U8>(code <= AREA_SUBMERGED ? code : RC_NULL_AREA);
            }
            else if (code < static_cast<S32>(partition_real->size()))
            {
                out[c] = (*partition_real)[code];
            }
        }
        return out;
    }

    /// A walkable span in the column already within the agent's climb of both
    /// surfaces: they are joined through it and need no bridge.
    bool columnJoins(const rcHeightfield& hf, S32 x, S32 z, S32 lo, S32 hi, S32 climb, U8 min_code)
    {
        for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
        {
            if (s->area >= min_code && rcAbs(static_cast<S32>(s->smax) - lo) <= climb && rcAbs(static_cast<S32>(s->smax) - hi) <= climb)
            {
                return true;
            }
        }
        return false;
    }

    /// Matter that stands in the way of joining two surfaces at lo and hi: a
    /// span that reaches above the lower surface and starts at or below the
    /// higher one. Ground and slabs below the seam are not in the way, so a
    /// crevice on an upper storey still joins; a wall face, a filled body or a
    /// riser is, so the wall's inside cannot tunnel to its outside.
    bool creviceBlocked(const rcHeightfield& hf, S32 x, S32 z, S32 lo, S32 hi)
    {
        for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
        {
            if (static_cast<S32>(s->smax) > lo && static_cast<S32>(s->smin) <= hi)
            {
                return true;
            }
        }
        return false;
    }

    /// A span to lay across a crevice, in the cells of the heightfield it was found on.
    struct Bridge
    {
        S32 mX = 0;
        S32 mZ = 0;
        unsigned short mSMin = 0;
        unsigned short mSMax = 0;
        U8 mArea = RC_NULL_AREA;

        bool operator==(const Bridge& o) const
        {
            return mX == o.mX && mZ == o.mZ && mSMin == o.mSMin && mSMax == o.mSMax && mArea == o.mArea;
        }
    };

    /// Recast joins a span to its neighbours only, so a crevice that leaves a
    /// column without a span at the surfaces' level splits them, and the
    /// erosion that follows widens it: the top of a staircase, solid in the
    /// sim, read as a gap. Find where to join the two sides across one such
    /// column, when their tops are within the agent's climb and nothing stands
    /// at that level between them; a class walks the spans of min_code and up.
    /// Nothing is added here: a span laid while scanning would be read as a
    /// neighbour by a later column and by the column's other axis, which must
    /// judge the heightfield they were given.
    std::vector<Bridge> findBridges(const rcHeightfield& hf, S32 climb, U8 min_code)
    {
        std::vector<Bridge> bridges;
        for (S32 z = 1; z + 1 < hf.height; ++z)
        {
            for (S32 x = 1; x + 1 < hf.width; ++x)
            {
                for (S32 axis = 0; axis < 2; ++axis)
                {
                    const S32 dx = axis == 0 ? 1 : 0;
                    const S32 dz = axis == 0 ? 0 : 1;
                    for (const rcSpan* a = hf.spans[(x - dx) + (z - dz) * hf.width]; a; a = a->next)
                    {
                        if (a->area < min_code)
                        {
                            continue;
                        }
                        for (const rcSpan* b = hf.spans[(x + dx) + (z + dz) * hf.width]; b; b = b->next)
                        {
                            if (b->area < min_code)
                            {
                                continue;
                            }
                            const S32 atop = a->smax, btop = b->smax;
                            if (rcAbs(atop - btop) > climb)
                            {
                                continue;
                            }
                            const S32 lo = std::min(atop, btop), hi = std::max(atop, btop);
                            if (columnJoins(hf, x, z, lo, hi, climb, min_code) || creviceBlocked(hf, x, z, lo, hi))
                            {
                                continue;
                            }
                            // The bridge's surface is the higher one, so no
                            // side is lifted; its floor reaches down to the
                            // lower, so both sides connect.
                            const S32 bottom = hi > lo ? lo : hi - 1;
                            Bridge bridge;
                            bridge.mX = x;
                            bridge.mZ = z;
                            bridge.mSMin = static_cast<unsigned short>(std::max(0, bottom));
                            bridge.mSMax = static_cast<unsigned short>(std::max(hi, bottom + 1));
                            bridge.mArea = std::max(a->area, b->area);
                            bridges.push_back(bridge);
                        }
                    }
                }
            }
        }
        return bridges;
    }

    /// Lay bridges found on a heightfield whose cells sit (dx, dz) inside this one.
    void addBridges(rcHeightfield& hf, const std::vector<Bridge>& bridges, S32 climb, S32 dx = 0, S32 dz = 0)
    {
        rcContext ctx(false);
        for (const Bridge& bridge : bridges)
        {
            rcAddSpan(&ctx, hf, bridge.mX + dx, bridge.mZ + dz, bridge.mSMin, bridge.mSMax, bridge.mArea, climb);
        }
    }

    /// How many bridges of one list the other lacks, counting repeats.
    U32 bridgesMissing(const std::vector<Bridge>& from, const std::vector<Bridge>& in)
    {
        std::vector<Bridge> rest = in;
        U32 missing = 0;
        for (const Bridge& b : from)
        {
            const auto it = std::find(rest.begin(), rest.end(), b);
            if (it == rest.end())
            {
                ++missing;
            }
            else
            {
                rest.erase(it);
            }
        }
        return missing;
    }

    /// Walk from a compact span along one direction until the floor drops by
    /// a step or more in one cell. Reports how far that was and the span it
    /// dropped to. Fails on losing the connection, on rising past the start,
    /// or on running out of reach.
    bool walkToStep(const rcCompactHeightfield& chf, S32 x, S32 z, S32 i, S32 dir, S32 reach, S32 step, S32& dist, S32& low)
    {
        const S32 y0 = chf.spans[i].y;
        S32 prev = y0;
        for (S32 n = 1; n <= reach; ++n)
        {
            const S32 con = rcGetCon(chf.spans[i], dir);
            if (con == RC_NOT_CONNECTED)
            {
                return false;
            }
            x += rcGetDirOffsetX(dir);
            z += rcGetDirOffsetY(dir);
            i = static_cast<S32>(chf.cells[x + z * chf.width].index) + con;
            if (chf.areas[i] == RC_NULL_AREA)
            {
                return false;
            }
            const S32 y = chf.spans[i].y;
            if (prev - y >= step)
            {
                dist = n;
                low = i;
                return true;
            }
            if (y >= y0 + step)
            {
                return false;
            }
            prev = y;
        }
        return false;
    }

    /// A kerb, rail or plinth narrower than the agent and lower than its
    /// climb is stepped over, not stood on, so the floor it stands on is the
    /// floor beside it. Left in, the detail mesh's samples land on it here
    /// and miss it there, and every hit lifts a sample's worth of flat floor
    /// around it into a pyramid. Lower such tops to the higher of the floors
    /// on either side, and give them that floor's area so they join its
    /// polygon. Compact heightfield only: the raw one keeps the feature's
    /// faces for the walls.
    void flattenNarrowSteps(rcCompactHeightfield& chf, S32 reach, S32 climb)
    {
        if (reach <= 0)
        {
            return;
        }
        const S32 step = 2; // cells: an edge, not a slope
        std::vector<std::pair<S32, S32>> lowered;
        for (S32 z = 0; z < chf.height; ++z)
        {
            for (S32 x = 0; x < chf.width; ++x)
            {
                const rcCompactCell& c = chf.cells[x + z * chf.width];
                for (S32 i = static_cast<S32>(c.index), ni = static_cast<S32>(c.index + c.count); i < ni; ++i)
                {
                    if (chf.areas[i] == RC_NULL_AREA)
                    {
                        continue;
                    }
                    const S32 y0 = chf.spans[i].y;
                    for (S32 axis = 0; axis < 2; ++axis)
                    {
                        S32 dist[2] = { 0, 0 }, low[2] = { 0, 0 };
                        if (!walkToStep(chf, x, z, i, axis, reach, step, dist[0], low[0])
                            || !walkToStep(chf, x, z, i, axis + 2, reach, step, dist[1], low[1])
                            || dist[0] + dist[1] - 1 > reach)
                        {
                            continue;
                        }
                        const S32 floor = chf.spans[low[0]].y >= chf.spans[low[1]].y ? low[0] : low[1];
                        if (y0 - static_cast<S32>(chf.spans[floor].y) <= climb)
                        {
                            lowered.emplace_back(i, floor);
                            break;
                        }
                    }
                }
            }
        }
        for (const auto& [i, floor] : lowered)
        {
            rcCompactSpan& s = chf.spans[i];
            const S32 y = chf.spans[floor].y;
            s.h = static_cast<unsigned int>(std::min<S32>(0xff, static_cast<S32>(s.h) + static_cast<S32>(s.y) - y));
            s.y = static_cast<unsigned int>(y);
            chf.areas[i] = chf.areas[floor];
        }
    }

    constexpr F32 SLOPE_FRAGMENT_M2 = 1.f;  ///< smaller patches join the neighbour they share most border with

    /// Visit each span with its cell coordinates.
    template <typename Fn> void eachSpan(const rcCompactHeightfield& chf, Fn&& fn)
    {
        for (S32 z = 0; z < chf.height; ++z)
        {
            for (S32 x = 0; x < chf.width; ++x)
            {
                const rcCompactCell& c = chf.cells[x + z * chf.width];
                for (S32 i = static_cast<S32>(c.index), ni = static_cast<S32>(c.index + c.count); i < ni; ++i)
                {
                    fn(x, z, i);
                }
            }
        }
    }

    /// The connected neighbour of a span in a direction, or -1.
    S32 neighbourOf(const rcCompactHeightfield& chf, S32 x, S32 z, S32 i, S32 dir)
    {
        const S32 con = rcGetCon(chf.spans[i], dir);
        if (con == RC_NOT_CONNECTED)
        {
            return -1;
        }
        return static_cast<S32>(chf.cells[(x + rcGetDirOffsetX(dir)) + (z + rcGetDirOffsetY(dir)) * chf.width].index) + con;
    }

    /// The slope partition's codes arrive rasterised; a patch of one code
    /// smaller than SLOPE_FRAGMENT_M2 is a sliver where regions overlap or a
    /// triangle clipped at a cell, and joins the neighbour of the same area
    /// that it shares most border with, smallest first, so a chain of
    /// slivers folds into the patch it hangs off. A flight's codes are kept
    /// whatever their size, since a short flight is smaller than a sliver.
    void mergeFragments(rcCompactHeightfield& chf, const std::vector<U8>& real, const std::vector<Flight>& flights)
    {
        std::vector<S32> cellOf(chf.spanCount);
        eachSpan(chf, [&](S32 x, S32 z, S32 i) { cellOf[i] = x + z * chf.width; });
        const auto neighbours = [&](S32 i, auto&& fn) {
            const S32 x = cellOf[i] % chf.width, z = cellOf[i] / chf.width;
            for (S32 dir = 0; dir < 4; ++dir)
            {
                const S32 n = neighbourOf(chf, x, z, i, dir);
                if (n >= 0 && chf.areas[n] != RC_NULL_AREA)
                {
                    fn(n);
                }
            }
        };

        std::vector<S32> patch(chf.spanCount, -1);
        std::vector<S32> size;
        std::vector<U8> codeOf;
        std::vector<S32> stack;
        for (S32 s = 0; s < chf.spanCount; ++s)
        {
            if (patch[s] >= 0 || chf.areas[s] == RC_NULL_AREA)
            {
                continue;
            }
            const S32 id = static_cast<S32>(size.size());
            size.push_back(0);
            codeOf.push_back(chf.areas[s]);
            patch[s] = id;
            stack.assign(1, s);
            while (!stack.empty())
            {
                const S32 i = stack.back();
                stack.pop_back();
                ++size[id];
                neighbours(i, [&](S32 n) {
                    if (patch[n] < 0 && chf.areas[n] == chf.areas[i])
                    {
                        patch[n] = id;
                        stack.push_back(n);
                    }
                });
            }
        }
        const S32 npatch = static_cast<S32>(size.size());

        std::vector<std::map<S32, S32>> border(npatch);
        for (S32 i = 0; i < chf.spanCount; ++i)
        {
            if (patch[i] < 0)
            {
                continue;
            }
            neighbours(i, [&](S32 n) {
                if (patch[n] != patch[i] && real[chf.areas[n]] == real[chf.areas[i]])
                {
                    ++border[patch[i]][patch[n]];
                }
            });
        }

        const S32 min_spans = static_cast<S32>(SLOPE_FRAGMENT_M2 / (chf.cs * chf.cs));
        std::vector<S32> into(npatch);
        std::vector<S32> order(npatch);
        for (S32 p = 0; p < npatch; ++p)
        {
            into[p] = order[p] = p;
        }
        const auto root = [&](S32 p) {
            while (into[p] != p)
            {
                p = into[p] = into[into[p]];
            }
            return p;
        };
        std::sort(order.begin(), order.end(), [&](S32 a, S32 b) { return size[a] < size[b]; });
        std::vector<U8> keep(real.size(), 0);
        for (const Flight& f : flights)
        {
            for (const U8 c : f.mCodes)
            {
                keep[c] = 1;
            }
        }
        for (const S32 p : order)
        {
            if (size[root(p)] >= min_spans || keep[codeOf[p]])
            {
                continue;
            }
            S32 best = -1, most = 0;
            for (const auto& [q, n] : border[p])
            {
                if (root(q) != root(p) && n > most)
                {
                    best = root(q);
                    most = n;
                }
            }
            if (best >= 0)
            {
                size[best] += size[root(p)];
                into[root(p)] = best;
            }
        }
        for (S32 i = 0; i < chf.spanCount; ++i)
        {
            if (patch[i] >= 0)
            {
                chf.areas[i] = codeOf[root(patch[i])];
            }
        }
    }

    /// The land's surface as a span index at one heightfield cell, or -1 where
    /// the terrain window has no say: outside it, or in the void past a
    /// region's edge. Interpolated on the same two triangles per sample quad
    /// the land is rasterised with, so the surface the cull reads is the one
    /// the build put down.
    S32 groundIndex(const rcHeightfield& hf, const TerrainWindow& t, S32 cx, S32 cz)
    {
        const F32 px = hf.bmin[0] + (cx + 0.5f) * hf.cs;    // Recast x runs along SL y
        const F32 pz = hf.bmin[2] + (cz + 0.5f) * hf.cs;    // and Recast z along SL x
        const F32 gx = (pz - t.mOrigin.mV[VX]) / t.mSpacing;
        const F32 gy = (px - t.mOrigin.mV[VY]) / t.mSpacing;
        if (gx < 0.f || gy < 0.f || gx > t.mWidth - 1.f || gy > t.mHeight - 1.f)
        {
            return -1;
        }
        const S32 i0 = llclamp(static_cast<S32>(gx), 0, t.mWidth - 2);
        const S32 j0 = llclamp(static_cast<S32>(gy), 0, t.mHeight - 2);
        const F32 u = llclamp(gx - i0, 0.f, 1.f);
        const F32 v = llclamp(gy - j0, 0.f, 1.f);
        const F32 z00 = t.mZ[j0 * t.mWidth + i0], z10 = t.mZ[j0 * t.mWidth + i0 + 1];
        const F32 z01 = t.mZ[(j0 + 1) * t.mWidth + i0], z11 = t.mZ[(j0 + 1) * t.mWidth + i0 + 1];
        if (z00 < VOID_Z || z10 < VOID_Z || z01 < VOID_Z || z11 < VOID_Z)
        {
            return -1;
        }
        const F32 z = u > v ? z00 + u * (z10 - z00) + v * (z11 - z10)
                            : z00 + v * (z01 - z00) + u * (z11 - z01);
        return llclamp(static_cast<S32>(std::floor((z - hf.bmin[1]) / hf.ch)), 0, RC_SPAN_MAX_HEIGHT);
    }

    /// Nothing legitimate lies below the land: the terrain surface is the
    /// ground, the sim keeps every agent on or above it, and a prim buried in
    /// it is out of reach. A span wholly under the surface at its column is
    /// removed, and one that reaches through it is trimmed to start there, so
    /// no floor, wall face or drop is ever read off the buried part. Columns
    /// the terrain window does not cover - the void past a region's edge, a
    /// mesh clear of the ground - keep everything.
    void cullUnderground(rcHeightfield& hf, const TerrainWindow& t)
    {
        if (t.mWidth < 2 || t.mHeight < 2 || t.mZ.size() < static_cast<size_t>(t.mWidth * t.mHeight))
        {
            return;
        }
        for (S32 cz = 0; cz < hf.height; ++cz)
        {
            for (S32 cx = 0; cx < hf.width; ++cx)
            {
                rcSpan** link = &hf.spans[cx + cz * hf.width];
                if (!*link)
                {
                    continue;
                }
                const S32 ground = groundIndex(hf, t, cx, cz);
                if (ground < 0)
                {
                    continue;
                }
                while (*link)
                {
                    rcSpan* s = *link;
                    if (static_cast<S32>(s->smax) < ground)
                    {
                        *link = s->next;    // wholly underground
                        continue;
                    }
                    if (static_cast<S32>(s->smin) < ground)
                    {
                        s->smin = static_cast<unsigned short>(ground);  // the earth fills below the surface
                    }
                    link = &s->next;
                }
            }
        }
    }

    /// The heightfield's spans and the bytes its column heads and span pools hold.
    void measureHeightfield(const rcHeightfield& hf, ClassBuildStats& out)
    {
        out.mHfSpans = 0;
        for (S32 c = 0; c < hf.width * hf.height; ++c)
        {
            for (const rcSpan* s = hf.spans[c]; s; s = s->next)
            {
                ++out.mHfSpans;
            }
        }
        out.mHfBytes = static_cast<size_t>(hf.width) * hf.height * sizeof(rcSpan*);
        for (const rcSpanPool* pool = hf.pools; pool; pool = pool->next)
        {
            out.mHfBytes += sizeof(rcSpanPool);
        }
    }

    /// A digest of the spans in a cell rectangle, relative to its corner, with
    /// each area passed through a lookup; empty lookup keeps only walkability.
    U64 hashHeightfield(const rcHeightfield& hf, S32 x0, S32 z0, S32 x1, S32 z1, const std::vector<U8>& lut)
    {
        U64 h = 0x9e3779b97f4a7c15ull;
        for (S32 z = z0; z < z1; ++z)
        {
            for (S32 x = x0; x < x1; ++x)
            {
                h = mix(h, static_cast<U64>((x - x0) | (z - z0) << 16));
                for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
                {
                    const U8 a = lut.empty() ? (s->area != RC_NULL_AREA) : lut[s->area];
                    h = mix(h, static_cast<U64>(s->smin) | static_cast<U64>(s->smax) << 16 | static_cast<U64>(a) << 32);
                }
            }
        }
        return h;
    }

    /// A digest of a compact heightfield: cells, spans, links and areas through a lookup.
    U64 hashCompact(const rcCompactHeightfield& chf, const std::vector<U8>& lut)
    {
        U64 h = mix(0x9e3779b97f4a7c15ull, static_cast<U64>(chf.width) | static_cast<U64>(chf.height) << 32);
        h = mixF32(mixF32(mixF32(h, chf.bmin[0]), chf.bmin[1]), chf.bmin[2]);
        for (S32 c = 0; c < chf.width * chf.height; ++c)
        {
            h = mix(h, static_cast<U64>(chf.cells[c].index) | static_cast<U64>(chf.cells[c].count) << 32);
        }
        for (S32 i = 0; i < chf.spanCount; ++i)
        {
            const rcCompactSpan& s = chf.spans[i];
            const U8 a = lut.empty() ? chf.areas[i] : lut[chf.areas[i]];
            h = mix(h, static_cast<U64>(s.y) | static_cast<U64>(s.con) << 16 | static_cast<U64>(s.h) << 40 | static_cast<U64>(a) << 48);
        }
        return h;
    }

    /// AVATAR's walkable spans on the tile proper, and the spans under each poly:
    /// those of the poly's region in the columns whose centres it covers.
    void recordPolySpans(const rcCompactHeightfield& chf, const rcPolyMesh& pmesh, S32 border, S32 tile_cells, const std::vector<U32>& span_gap,
                         TileBuildStats& out)
    {
        std::vector<U32> index(chf.spanCount, U32_MAX);
        for (S32 z = border; z < border + tile_cells; ++z)
        {
            for (S32 x = border; x < border + tile_cells; ++x)
            {
                const rcCompactCell& c = chf.cells[x + z * chf.width];
                for (U32 i = c.index; i < c.index + c.count; ++i)
                {
                    if (chf.areas[i] == RC_NULL_AREA)
                    {
                        continue;
                    }
                    StatSpan span;
                    span.mX = static_cast<U16>(z - border);
                    span.mY = static_cast<U16>(x - border);
                    span.mTop = chf.spans[i].y;
                    span.mRegion = chf.spans[i].reg;
                    span.mCode = chf.areas[i];
                    span.mGap = i < span_gap.size() ? span_gap[i] : U32_MAX;
                    index[i] = static_cast<U32>(out.mSpans.size());
                    out.mSpans.push_back(span);
                }
            }
        }
        out.mPolySpanStart.assign(1, 0);
        for (S32 p = 0; p < pmesh.npolys; ++p)
        {
            const unsigned short* poly = &pmesh.polys[p * pmesh.nvp * 2];
            S32 nv = 0;
            F32 vx[16], vz[16];
            S32 lo_x = INT_MAX, hi_x = INT_MIN, lo_z = INT_MAX, hi_z = INT_MIN;
            for (; nv < pmesh.nvp && poly[nv] != RC_MESH_NULL_IDX; ++nv)
            {
                const unsigned short* v = &pmesh.verts[poly[nv] * 3];
                vx[nv] = v[0];
                vz[nv] = v[2];
                lo_x = std::min<S32>(lo_x, v[0]);
                hi_x = std::max<S32>(hi_x, v[0]);
                lo_z = std::min<S32>(lo_z, v[2]);
                hi_z = std::max<S32>(hi_z, v[2]);
            }
            F32 area = 0.f;
            for (S32 k = 0; k < nv; ++k)
            {
                const S32 n = (k + 1) % nv;
                area += vx[k] * vz[n] - vx[n] * vz[k];
            }
            const F32 sign = area < 0.f ? -1.f : 1.f;
            out.mPolyArea.push_back(std::fabs(area) * 0.5f * pmesh.cs * pmesh.cs);
            for (S32 z = lo_z; z < hi_z; ++z)
            {
                for (S32 x = lo_x; x < hi_x; ++x)
                {
                    const F32 px = x + 0.5f, pz = z + 0.5f;
                    bool inside = true;
                    for (S32 k = 0; k < nv && inside; ++k)
                    {
                        const S32 n = (k + 1) % nv;
                        inside = sign * ((vx[n] - vx[k]) * (pz - vz[k]) - (vz[n] - vz[k]) * (px - vx[k])) >= 0.f;
                    }
                    const S32 cx = x + border, cz = z + border;
                    if (!inside || cx < 0 || cz < 0 || cx >= chf.width || cz >= chf.height)
                    {
                        continue;
                    }
                    const rcCompactCell& c = chf.cells[cx + cz * chf.width];
                    for (U32 i = c.index; i < c.index + c.count; ++i)
                    {
                        if (chf.spans[i].reg == pmesh.regs[p] && index[i] != U32_MAX)
                        {
                            out.mPolySpans.push_back(index[i]);
                            break;
                        }
                    }
                }
            }
            out.mPolySpanStart.push_back(static_cast<U32>(out.mPolySpans.size()));
        }
    }

    /// A heightfield over a rectangle of other heightfields' cells, sharing their
    /// spans rather than copying them: Recast's filters and compaction run on
    /// it exactly as on a heightfield of that size, edges and all, and what
    /// they change they change in the spans underneath.
    struct CropView
    {
        rcHeightfield mHf;
        S32 mInset = 0;

        CropView(const rcConfig& cfg, S32 inset) : mInset(inset)
        {
            mHf.width = cfg.width;
            mHf.height = cfg.height;
            rcVcopy(mHf.bmin, cfg.bmin);
            rcVcopy(mHf.bmax, cfg.bmax);
            mHf.cs = cfg.cs;
            mHf.ch = cfg.ch;
            mHf.spans = static_cast<rcSpan**>(rcAlloc(sizeof(rcSpan*) * cfg.width * cfg.height, RC_ALLOC_PERM));
        }
        CropView(const CropView&) = delete;
        CropView& operator=(const CropView&) = delete;

        /// Point every column at base's, or at over's where own marks it; again after either changes.
        void point(const rcHeightfield& base, const rcHeightfield* over = nullptr, const std::vector<U8>* own = nullptr)
        {
            for (S32 z = 0; z < mHf.height; ++z)
            {
                for (S32 x = 0; x < mHf.width; ++x)
                {
                    const S32 c = (x + mInset) + (z + mInset) * base.width;
                    mHf.spans[x + z * mHf.width] = over && (*own)[c] ? over->spans[c] : base.spans[c];
                }
            }
        }
    };

    /// Every span's area, column by column, to put back after a pass that rewrites them.
    struct AreaSnapshot
    {
        std::vector<U8> mAreas;
        std::vector<U32> mStart;    ///< per column, where its areas begin

        void take(const rcHeightfield& hf)
        {
            mAreas.clear();
            mStart.assign(1, 0);
            for (S32 c = 0; c < hf.width * hf.height; ++c)
            {
                for (const rcSpan* s = hf.spans[c]; s; s = s->next)
                {
                    mAreas.push_back(static_cast<U8>(s->area));
                }
                mStart.push_back(static_cast<U32>(mAreas.size()));
            }
        }

        /// Every column's areas back, or only those of the columns marked.
        void restore(rcHeightfield& hf, const std::vector<U8>* only = nullptr) const
        {
            for (S32 c = 0; c < hf.width * hf.height; ++c)
            {
                if (only && !(*only)[c])
                {
                    continue;
                }
                U32 i = mStart[c];
                for (rcSpan* s = hf.spans[c]; s; s = s->next)
                {
                    llassert(i < mStart[c + 1]);
                    s->area = mAreas[i++];
                }
                llassert(i == mStart[c + 1]);
            }
        }
    };

    /// A copy of one column's spans into the same column of another heightfield.
    void copyColumn(const rcHeightfield& from, rcHeightfield& to, S32 x, S32 z)
    {
        rcContext ctx(false);
        for (const rcSpan* s = from.spans[x + z * from.width]; s; s = s->next)
        {
            rcAddSpan(&ctx, to, x, z, static_cast<unsigned short>(s->smin), static_cast<unsigned short>(s->smax), static_cast<U8>(s->area), 0);
        }
    }

    /// Recast's three span filters for one agent: the ledge filter's climb
    /// follows the agent's slope, since at its own climb it reads every slope
    /// steeper than atan(climb / two cells) as a ledge - 72 degrees at 0.75 m,
    /// below either slope limit - and cuts lines and holes out of exactly the
    /// steep ground the agent walks. Matched to the slope, it only fires past
    /// the limit, where the slope test has already said no.
    /// The ledge and headroom filters only clear areas, on tests that read no
    /// area but the span's own, so in either order they clear the same spans;
    /// before_ledges keeps the areas between them.
    void filterSpans(rcHeightfield& hf, const ClassParams& p, F32 ch, F32 cs, AreaSnapshot* before_ledges = nullptr)
    {
        rcContext ctx(false);
        const S32 climb = static_cast<S32>(std::floor(p.mClimb / ch));
        const S32 height = static_cast<S32>(std::ceil(p.mHeight / ch));
        const F32 ledge_climb = llmax(p.mClimb, 2.f * cs * std::tan(p.mSlope * DEG_TO_RAD));
        rcFilterLowHangingWalkableObstacles(&ctx, climb, hf);
        if (before_ledges)
        {
            rcFilterWalkableLowHeightSpans(&ctx, height, hf);
            before_ledges->take(hf);
            rcFilterLedgeSpans(&ctx, height, static_cast<int>(std::ceil(ledge_climb / ch)), hf);
            return;
        }
        rcFilterLedgeSpans(&ctx, height, static_cast<int>(std::ceil(ledge_climb / ch)), hf);
        rcFilterWalkableLowHeightSpans(&ctx, height, hf);
    }

    struct FreeHeightfield
    {
        void operator()(rcHeightfield* hf) const { rcFreeHeightField(hf); }
    };
    typedef std::unique_ptr<rcHeightfield, FreeHeightfield> HeightfieldPtr;

    rcHeightfield* makeHeightfield(const rcConfig& cfg)
    {
        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        if (hf && !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        {
            rcFreeHeightField(hf);
            return nullptr;
        }
        return hf;
    }

    /// The tile's Recast grid for an agent, with border cells of padding each side.
    rcConfig tileConfig(const TileBuildInput& input, const ClassParams& p, S32 border)
    {
        rcConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.cs = input.mCell;
        cfg.ch = input.mCell;
        cfg.walkableSlopeAngle = p.mSlope;
        cfg.walkableHeight = static_cast<int>(std::ceil(p.mHeight / cfg.ch));
        cfg.walkableClimb = static_cast<int>(std::floor(p.mClimb / cfg.ch));
        cfg.walkableRadius = static_cast<int>(std::ceil(p.mRadius / cfg.cs));
        cfg.maxEdgeLen = static_cast<int>(12.f / cfg.cs);
        cfg.maxSimplificationError = 1.3f;
        cfg.minRegionArea = 8 * 8;
        cfg.mergeRegionArea = 20 * 20;
        cfg.maxVertsPerPoly = 6;
        cfg.detailSampleDist = input.mDetailSampleCells * cfg.cs;
        cfg.detailSampleMaxError = input.mDetailMaxErrorCells * cfg.ch;
        cfg.borderSize = border;
        cfg.tileSize = static_cast<int>(input.mTileSize / cfg.cs);
        cfg.width = cfg.tileSize + cfg.borderSize * 2;
        cfg.height = cfg.width;

        // The tile in Recast space: x = SL y, z = SL x, y = SL z.
        const F32 x0 = input.mTileX * input.mTileSize, y0 = input.mTileY * input.mTileSize;
        cfg.bmin[0] = y0 - cfg.borderSize * cfg.cs;
        // Snapped to the cell grid, not to this tile's own lowest point:
        // heights quantise to ch from bmin, so two tiles that started
        // from different z put the same floor on different spans, and the
        // two sides of a seam end up at slightly different heights.
        cfg.bmin[1] = std::floor((input.mZMin - cfg.ch) / cfg.ch) * cfg.ch;
        cfg.bmin[2] = x0 - cfg.borderSize * cfg.cs;
        cfg.bmax[0] = y0 + input.mTileSize + cfg.borderSize * cfg.cs;
        cfg.bmax[1] = std::ceil((input.mZMax + cfg.ch) / cfg.ch) * cfg.ch;
        cfg.bmax[2] = x0 + input.mTileSize + cfg.borderSize * cfg.cs;
        return cfg;
    }

    /// A deep copy of a compact heightfield, or null when Recast's allocator refuses.
    rcCompactHeightfield* cloneCompact(const rcCompactHeightfield& src)
    {
        rcCompactHeightfield* out = rcAllocCompactHeightfield();
        if (!out)
        {
            return nullptr;
        }
        out->width = src.width;
        out->height = src.height;
        out->spanCount = src.spanCount;
        out->walkableHeight = src.walkableHeight;
        out->walkableClimb = src.walkableClimb;
        out->borderSize = src.borderSize;
        out->maxDistance = src.maxDistance;
        out->maxRegions = src.maxRegions;
        rcVcopy(out->bmin, src.bmin);
        rcVcopy(out->bmax, src.bmax);
        out->cs = src.cs;
        out->ch = src.ch;
        const size_t cells = static_cast<size_t>(src.width) * src.height, spans = static_cast<size_t>(src.spanCount);
        out->cells = static_cast<rcCompactCell*>(rcAlloc(sizeof(rcCompactCell) * cells, RC_ALLOC_PERM));
        out->spans = static_cast<rcCompactSpan*>(rcAlloc(sizeof(rcCompactSpan) * spans, RC_ALLOC_PERM));
        out->areas = static_cast<unsigned char*>(rcAlloc(spans, RC_ALLOC_PERM));
        out->dist = src.dist ? static_cast<unsigned short*>(rcAlloc(sizeof(unsigned short) * spans, RC_ALLOC_PERM)) : nullptr;
        if (!out->cells || !out->spans || !out->areas || (src.dist && !out->dist))
        {
            rcFreeCompactHeightfield(out);
            return nullptr;
        }
        std::memcpy(out->cells, src.cells, sizeof(rcCompactCell) * cells);
        std::memcpy(out->spans, src.spans, sizeof(rcCompactSpan) * spans);
        std::memcpy(out->areas, src.areas, spans);
        if (src.dist)
        {
            std::memcpy(out->dist, src.dist, sizeof(unsigned short) * spans);
        }
        return out;
    }

    struct Freed
    {
        rcHeightfield* hf = nullptr;
        rcCompactHeightfield* chf = nullptr;
        rcContourSet* cset = nullptr;
        rcPolyMesh* pmesh = nullptr;
        rcPolyMeshDetail* dmesh = nullptr;
        ~Freed()
        {
            rcFreeHeightField(hf);
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
        }
    };
}

namespace sknav
{
    void FreeCompact::operator()(rcCompactHeightfield* chf) const
    {
        rcFreeCompactHeightfield(chf);
    }

    size_t TileRaster::chfBytes() const
    {
        if (!mChf)
        {
            return 0;
        }
        const size_t spans = static_cast<size_t>(mChf->spanCount);
        return sizeof(rcCompactHeightfield) + static_cast<size_t>(mChf->width) * mChf->height * sizeof(rcCompactCell) +
               spans * (sizeof(rcCompactSpan) + 1 + (mChf->dist ? sizeof(unsigned short) : 0));
    }

    size_t TileRaster::bytes() const
    {
        size_t n = sizeof(TileRaster) + chfBytes() + mEdges.bytes() + mReal.capacity() + mStanding.capacity() + mSpanGap.capacity() * sizeof(U32);
        for (const Flight& f : mFlights)
        {
            n += sizeof(Flight) + f.mFootprint.capacity() * sizeof(LLVector2) + f.mCodes.capacity() + f.mPolys.capacity() * sizeof(U16);
        }
        return n;
    }

    F32 partitionSlope(const TileBuildInput& input)
    {
        if (input.mPartitionSlope > 0.f)
        {
            return input.mPartitionSlope;
        }
        return input.mAir ? input.mTopParams.mSlope : input.mParams[static_cast<S32>(AgentClass::AVATAR)].mSlope;
    }

    S32 airPadCells(F32 cell)
    {
        return static_cast<S32>(std::ceil(AIR_PAD_M / cell - 1e-4f));
    }

    S32 borderCells(F32 radius, F32 cell)
    {
        const S32 r = static_cast<S32>(std::ceil(radius / cell));
        return r + 2 * r + 3;
    }

    void emitRecord(const Record& record, const LLVector3d& mesh_origin, std::vector<LLVector3>& out)
    {
        // F64 until the shift is applied, then F32 in mesh space.
        const LLVector3d shift64 = record.mOrigin - mesh_origin;
        const LLVector3 shift(static_cast<F32>(shift64.mdV[VX]), static_cast<F32>(shift64.mdV[VY]), static_cast<F32>(shift64.mdV[VZ]));
        std::vector<LLVector3> local;
        const std::vector<LLVector3>* src = &local;
        switch (record.mShape)
        {
            case Shape::TRIANGLES:
                if (record.mTriangles)
                {
                    src = record.mTriangles.get();
                }
                break;
            case Shape::BOX: emitBox(record, local); break;
            case Shape::CYLINDER: emitCylinder(record, local); break;
            case Shape::SPHERE: emitSphere(record, local); break;
        }
        out.reserve(out.size() + src->size());
        for (const LLVector3& p : *src)
        {
            out.push_back(p + shift);
        }
    }

    const char* AirBuildStats::stageName(S32 stage)
    {
        static const char* names[NUM_STAGES] = { "extract", "fold", "flights", "clearance", "crop", "distil", "seams", "tops" };
        return stage >= 0 && stage < NUM_STAGES ? names[stage] : "";
    }

    const char* ClassBuildStats::stageName(S32 stage)
    {
        static const char* names[NUM_STAGES] = { "partition", "rasterise", "fill", "cut", "bridges", "cull", "filters", "compact", "erode",
                                                 "regions", "contours", "polymesh", "stamp", "flightmap", "detail", "walls", "air", "edges", "detour" };
        return stage >= 0 && stage < NUM_STAGES ? names[stage] : "";
    }

    namespace
    {
        /// What the raster stage makes of a tile before its cell matters: the soup and each triangle's area code.
        struct TileSoup
        {
            Soup mSoup;
            Soup mSolidSoup;                                ///< without what the air leaves out, when it leaves anything out
            Soup mCutSoup;
            std::vector<std::pair<S32, S32>> mSolids;      ///< triangle ranges of closed bodies
            std::vector<std::pair<S32, S32>> mSolidSolids;
            std::vector<std::pair<S32, S32>> mCuts;
            std::vector<Excluded> mExcluded;                ///< triangle ranges some class builds without
            bool mFiltered = false;
            std::vector<U8> mLeafTri;
            std::vector<U8> mCodes;
            std::vector<U8> mReal;
            std::vector<Flight> mFlights;

            const Soup& main() const { return mFiltered ? mSolidSoup : mSoup; }
            const std::vector<std::pair<S32, S32>>& mainSolids() const { return mFiltered ? mSolidSolids : mSolids; }
        };

        /// The soup of a tile and its partition; false when it holds no triangle.
        bool soupTile(const TileBuildInput& input, TileBuildResult& result, TileBuildStats* stats, std::chrono::steady_clock::time_point started,
                      TileSoup& out)
        {
            // Gather the soup in Recast space.
            Soup& soup = out.mSoup;
            std::vector<LLVector3> tris;
            std::vector<std::pair<S32, S32>>& solids = out.mSolids;
            std::vector<Excluded>& excluded = out.mExcluded;
            for (const RecordPtr& r : input.mRecords)
            {
                if (!r)
                {
                    continue;
                }
                // A hull is a solid the sim collides with from either side: mark
                // it walkable whichever way its triangles wind.
                const bool hull = r->mProvenance == Provenance::HULL || r->mProvenance == Provenance::BASE_HULL;
                tris.clear();
                emitRecord(*r, input.mMeshOrigin, tris);
                const S32 before = soup.ntris();
                const auto cue = std::lower_bound(input.mRootCues.begin(), input.mRootCues.end(), std::make_pair(r->mRoot, U8(0)));
                soup.add(tris, AREA_OBJECT, input.mTerrain.mWater, hull,
                         cue != input.mRootCues.end() && cue->first == r->mRoot ? cue->second : static_cast<U8>(CUE_NONE));
                if (r->mExclude && soup.ntris() > before)
                {
                    excluded.push_back({ before, soup.ntris(), r->mExclude });
                }
                // A solid is filled. A decomposition is a set of bodies, not one:
                // its hulls overlap, and a column through two of them would count
                // four crossings and fill none of the middle. Fill each body on
                // its own. A record whose bodies are not told apart arrives as one
                // range and is filled as one closed surface.
                S32 pieces = 0;
                for (const S32 count : r->mConvexTris)
                {
                    pieces += count;
                }
                if (r->mSolid && r->mConvexTris.size() > 1 && pieces == soup.ntris() - before)
                {
                    S32 at = before;
                    for (const S32 count : r->mConvexTris)
                    {
                        if (count > 0)
                        {
                            solids.emplace_back(at, at + count);
                        }
                        at += count;
                    }
                }
                else if (r->mSolid)
                {
                    solids.emplace_back(before, soup.ntris());
                }
            }
            // An exclusion volume is a cut, not a shape: its triangles never enter
            // the soup, they only say where the navmesh is taken out of.
            Soup& cut_soup = out.mCutSoup;
            std::vector<std::pair<S32, S32>>& cuts = out.mCuts;
            for (const RecordPtr& r : input.mCuts)
            {
                if (!r)
                {
                    continue;
                }
                tris.clear();
                emitRecord(*r, input.mMeshOrigin, tris);
                const S32 before = cut_soup.ntris();
                cut_soup.add(tris, AREA_OBJECT, input.mTerrain.mWater, false);
                if (cut_soup.ntris() > before)
                {
                    cuts.emplace_back(before, cut_soup.ntris());
                }
            }
            const TerrainWindow& t = input.mTerrain;
            if (t.mWidth >= 2 && t.mHeight >= 2 && t.mZ.size() >= static_cast<size_t>(t.mWidth * t.mHeight))
            {
                tris.clear();
                for (S32 y = 0; y + 1 < t.mHeight; ++y)
                {
                    for (S32 x = 0; x + 1 < t.mWidth; ++x)
                    {
                        // A sample below the void sentinel has no region under it: no floor there.
                        const F32 z00 = t.mZ[y * t.mWidth + x], z10 = t.mZ[y * t.mWidth + x + 1], z11 = t.mZ[(y + 1) * t.mWidth + x + 1], z01 = t.mZ[(y + 1) * t.mWidth + x];
                        if (z00 < VOID_Z || z10 < VOID_Z || z11 < VOID_Z || z01 < VOID_Z)
                        {
                            continue;
                        }
                        const LLVector3 a(t.mOrigin.mV[VX] + x * t.mSpacing, t.mOrigin.mV[VY] + y * t.mSpacing, z00);
                        const LLVector3 b(t.mOrigin.mV[VX] + (x + 1) * t.mSpacing, t.mOrigin.mV[VY] + y * t.mSpacing, t.mZ[y * t.mWidth + x + 1]);
                        const LLVector3 c(t.mOrigin.mV[VX] + (x + 1) * t.mSpacing, t.mOrigin.mV[VY] + (y + 1) * t.mSpacing, t.mZ[(y + 1) * t.mWidth + x + 1]);
                        const LLVector3 d(t.mOrigin.mV[VX] + x * t.mSpacing, t.mOrigin.mV[VY] + (y + 1) * t.mSpacing, t.mZ[(y + 1) * t.mWidth + x]);
                        quad(tris, a, b, c, d); // counter-clockwise from above: +z in SL, +y in Recast
                    }
                }
                soup.add(tris, AREA_GROUND, t.mWater, false);
            }
            result.mTriangles = soup.ntris();
            if (stats)
            {
                stats->mSoupMs = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
            }
            if (soup.ntris() == 0)
            {
                result.mStatus = TileBuildResult::EMPTY;
                return false;
            }
            if (stats)
            {
                // Each excluded range's box, two cells wider, over the tile's cells.
                const S32 n = static_cast<S32>(input.mTileSize / input.mCell);
                const F32 x0 = input.mTileX * input.mTileSize, y0 = input.mTileY * input.mTileSize;
                std::vector<U8> window(static_cast<size_t>(n) * n, 0);
                for (const Excluded& e : excluded)
                {
                    F32 lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    for (S32 t = e.mBegin * 3; t < e.mEnd * 3; ++t)
                    {
                        const F32* v = &soup.mVerts[soup.mTris[t] * 3];
                        for (S32 k = 0; k < 3; ++k)
                        {
                            lo[k] = llmin(lo[k], v[k]);
                            hi[k] = llmax(hi[k], v[k]);
                        }
                    }
                    const S32 ix0 = std::max(0, static_cast<S32>(std::floor((lo[2] - x0) / input.mCell)) - 2);
                    const S32 ix1 = std::min(n, static_cast<S32>(std::ceil((hi[2] - x0) / input.mCell)) + 2);
                    const S32 iy0 = std::max(0, static_cast<S32>(std::floor((lo[0] - y0) / input.mCell)) - 2);
                    const S32 iy1 = std::min(n, static_cast<S32>(std::ceil((hi[0] - y0) / input.mCell)) + 2);
                    for (S32 iy = iy0; iy < iy1; ++iy)
                    {
                        for (S32 ix = ix0; ix < ix1; ++ix)
                        {
                            window[ix + iy * n] = 1;
                        }
                    }
                }
                stats->mExcludedRecords = static_cast<U32>(excluded.size());
                stats->mExcludeWindowCells = static_cast<U32>(std::count(window.begin(), window.end(), 1));
                stats->mTileCells = static_cast<U32>(window.size());
            }

            // One solid raster serves AVATAR and the air. It holds every solid
            // but what the air leaves out, at AVATAR's grid widened to the air's
            // pad; AVATAR reads it through a view cropped to its own border, and
            // the surfaces only the air's slope walks carry codes it masks away.
            // Records AVATAR leaves out and the air keeps - door leaves - are
            // rasterised again without them over a window round them, whose
            // columns AVATAR's view reads instead.
            rcContext ctx(false);
            const S32 avatar = static_cast<S32>(AgentClass::AVATAR);
            const ClassParams& ap = input.mParams[avatar];
            ClassParams tp = input.mTopParams;
            tp.mSlope = llmax(tp.mSlope, ap.mSlope);
            tp.mClimb = ap.mClimb;
            ClassBuildStats* astats = stats ? &stats->mClass[avatar] : nullptr;
            ClassBuildStats* tstats = stats ? &stats->mTop : nullptr;
            StageClock clock(astats ? astats->mMs : nullptr);
            if (astats)
            {
                astats->mBuilt = true;
            }

            // The raster's soup, and per triangle of it whether AVATAR leaves it out.
            const U8 solid_bit = EXCLUDE_SOLID, avatar_bit = EXCLUDE_AVATAR;
            Soup& solid_soup = out.mSolidSoup;
            std::vector<std::pair<S32, S32>>& solid_solids = out.mSolidSolids;
            const Soup* main_soup = &soup;
            std::vector<U8>& leaf_tri = out.mLeafTri;
            {
                const auto filter_started = std::chrono::steady_clock::now();
                std::vector<U8> solid_drop(soup.ntris(), 0), avatar_drop(soup.ntris(), 0);
                bool solid_any = false, avatar_any = false;
                for (const Excluded& e : excluded)
                {
                    const bool solid_out = (e.mClasses & solid_bit) != 0;
                    const bool avatar_out = !solid_out && (e.mClasses & avatar_bit) != 0;
                    std::fill(solid_drop.begin() + e.mBegin, solid_drop.begin() + e.mEnd, solid_out ? 1 : 0);
                    std::fill(avatar_drop.begin() + e.mBegin, avatar_drop.begin() + e.mEnd, avatar_out ? 1 : 0);
                    solid_any = solid_any || solid_out;
                    avatar_any = avatar_any || avatar_out;
                }
                if (solid_any)
                {
                    filterSoup(soup, solids, excluded, solid_bit, solid_soup, solid_solids);
                    main_soup = &solid_soup;
                    out.mFiltered = true;
                }
                if (avatar_any)
                {
                    for (S32 t = 0; t < soup.ntris(); ++t)
                    {
                        if (!solid_drop[t])
                        {
                            leaf_tri.push_back(avatar_drop[t]);
                        }
                    }
                }
                if (solid_any || avatar_any)
                {
                    result.mExcludeMillis += std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - filter_started).count();
                    clock.restart();
                }
            }

            // The area byte of each triangle. The partition is AVATAR's: over the
            // raster's soup at the pinned slope, or where AVATAR leaves records out,
            // over the soup without them at AVATAR's own slope, as it always was.
            const bool partitioned = input.mPartition == Partition::SLOPE;
            const bool own_partition = std::any_of(excluded.begin(), excluded.end(), [&](const Excluded& e) { return (e.mClasses & avatar_bit) != 0; });
            std::vector<U8>& codes = out.mCodes;
            std::vector<U8>& real = out.mReal;
            std::vector<Flight>& flights = out.mFlights;
            {
                std::vector<U8> avatar_walk, top_walk;
                markWalkable(*main_soup, ap.mSlope, avatar_walk);
                markWalkable(*main_soup, tp.mSlope, top_walk);
                for (S32 t = 0; t < static_cast<S32>(leaf_tri.size()); ++t)
                {
                    avatar_walk[t] = leaf_tri[t] ? 0 : avatar_walk[t];     // AVATAR never stands on what it leaves out
                }
                SlopePartition slopes;
                std::vector<U8> main_codes;     // per triangle of the raster's soup
                if (partitioned)
                {
                    Soup avatar_soup;
                    std::vector<std::pair<S32, S32>> avatar_solids;
                    if (!leaf_tri.empty())
                    {
                        filterSoup(soup, solids, excluded, static_cast<U8>(avatar_bit | solid_bit), avatar_soup, avatar_solids);
                    }
                    const Soup& part = leaf_tri.empty() ? *main_soup : avatar_soup;
                    std::vector<U8> walkable;
                    markWalkable(part, own_partition ? ap.mSlope : llmax(partitionSlope(input), ap.mSlope), walkable);
                    std::vector<LLVector3> sl(part.nverts());
                    for (S32 v = 0; v < part.nverts(); ++v)
                    {
                        sl[v] = fromRecast(&part.mVerts[v * 3]);
                    }
                    slopes = partitionSlopes(sl, part.mTris, walkable, part.mTwoSided, AREA_OBJECT, ap.mClimb, PARTITION_MAX_CODES, part.mCues);
                    shiftFlightCodes(slopes);
                    // Back onto the raster's triangles, the left-out ones coded by area alone.
                    main_codes.assign(main_soup->ntris(), 0);
                    for (S32 t = 0, j = 0; t < main_soup->ntris(); ++t)
                    {
                        if (leaf_tri.empty() || !leaf_tri[t])
                        {
                            main_codes[t] = slopes.mCodes[j++];
                        }
                    }
                    if (stats)
                    {
                        std::vector<U8> used(256, 0);
                        for (const U8 code : slopes.mCodes)
                        {
                            used[code] = 1;
                        }
                        used[0] = 0;
                        stats->mSharedCodes = static_cast<U32>(std::count(used.begin(), used.end(), 1));
                    }
                }
                codes = encodeAreas(avatar_walk, top_walk, partitioned ? &main_codes : nullptr);
                real = realAreas(partitioned ? &slopes.mReal : nullptr);
                flights = std::move(slopes.mFlights);
            }
            if (astats)
            {
                std::vector<U8> used(256, 0);
                for (S32 t = 0; t < static_cast<S32>(codes.size()); ++t)
                {
                    used[codes[t]] = codes[t] >= AREA_CODE_BASE && (leaf_tri.empty() || !leaf_tri[t]);
                }
                astats->mCodes = static_cast<U32>(std::count(used.begin(), used.end(), 1));
            }
            clock.lap(ClassBuildStats::PARTITION);
            return true;
        }

        TileAirPtr coarseAirOf(const TileBuildInput& input, const TileSoup& ts);

        /// The raster stage over a tile's soup at the input's cell.
        TileRasterPtr rasteriseSoup(const TileBuildInput& input, const TileSoup& ts, TileBuildResult& result, TileBuildStats* stats,
                                    std::chrono::steady_clock::time_point started)
        {
            rcContext ctx(false);
            const S32 avatar = static_cast<S32>(AgentClass::AVATAR);
            const ClassParams& ap = input.mParams[avatar];
            ClassParams tp = input.mTopParams;
            tp.mSlope = llmax(tp.mSlope, ap.mSlope);
            tp.mClimb = ap.mClimb;
            ClassBuildStats* astats = stats ? &stats->mClass[avatar] : nullptr;
            ClassBuildStats* tstats = stats ? &stats->mTop : nullptr;
            StageClock clock(astats ? astats->mMs : nullptr);
            const rcConfig cfg = tileConfig(input, ap, borderCells(ap.mRadius, input.mCell));
            const rcConfig grid = tileConfig(input, ap, input.mAir ? std::max(cfg.borderSize, input.mAirPad) : cfg.borderSize);
            const S32 inset = grid.borderSize - cfg.borderSize;
            const Soup* main_soup = &ts.main();
            const std::vector<std::pair<S32, S32>>* main_solids = &ts.mainSolids();
            const Soup& cut_soup = ts.mCutSoup;
            const std::vector<std::pair<S32, S32>>& cuts = ts.mCuts;
            const std::vector<U8>& codes = ts.mCodes;
            const std::vector<U8>& leaf_tri = ts.mLeafTri;
            std::vector<U8> real = ts.mReal;
            std::vector<Flight> flights = ts.mFlights;

            Freed mem;
            HeightfieldPtr overlay;
            mem.hf = makeHeightfield(grid);
            if (!mem.hf)
            {
                result.mError = "heightfield";
                return nullptr;
            }
            rcHeightfield& hf = *mem.hf;
            const RasterSource source{ main_soup, &codes, main_solids, &cut_soup, &cuts };
            if (!rasterisePasses(hf, source, CellRect{ 0, 0, hf.width, hf.height }, grid.walkableClimb, clock))
            {
                result.mError = "rasterize";
                return nullptr;
            }

            // The window: every cell a left-out triangle touches and one more
            // round it is AVATAR's own; its raster reaches a cell further, so
            // a bridge in the outer ring reads the columns past it complete.
            const size_t columns = static_cast<size_t>(hf.width) * hf.height;
            std::vector<U8> in_window, own_column;
            if (!leaf_tri.empty())
            {
                in_window.assign(columns, 0);
                CellRect rect{ hf.width, hf.height, 0, 0 };
                for (S32 t = 0; t < main_soup->ntris(); ++t)
                {
                    if (!leaf_tri[t])
                    {
                        continue;
                    }
                    const CellRect r = cellsOf(hf, *main_soup, t, t + 1);
                    for (S32 z = std::max(0, r.mZ0); z < std::min(hf.height, r.mZ1); ++z)
                    {
                        for (S32 x = std::max(0, r.mX0); x < std::min(hf.width, r.mX1); ++x)
                        {
                            in_window[x + z * hf.width] = 1;
                        }
                    }
                    rect = { std::min(rect.mX0, r.mX0 - 1), std::min(rect.mZ0, r.mZ0 - 1), std::max(rect.mX1, r.mX1 + 1), std::max(rect.mZ1, r.mZ1 + 1) };
                }
                overlay.reset(makeHeightfield(grid));
                const RasterSource window{ main_soup, &codes, main_solids, &cut_soup, &cuts, &leaf_tri };
                if (!overlay || !rasterisePasses(*overlay, window, rect, grid.walkableClimb, clock))
                {
                    result.mError = "rasterize";
                    return nullptr;
                }
                own_column = in_window;
                if (stats)
                {
                    stats->mWindowCells = static_cast<U32>(std::count(in_window.begin(), in_window.end(), 1));
                    stats->mWindowRect[0] = rect.mX0 - grid.borderSize;
                    stats->mWindowRect[1] = rect.mZ0 - grid.borderSize;
                    stats->mWindowRect[2] = rect.mX1 - grid.borderSize;
                    stats->mWindowRect[3] = rect.mZ1 - grid.borderSize;
                }
            }
            CropView view(cfg, inset);
            view.point(hf, overlay.get(), &own_column);

            // Both bridge lists off the unbridged views. A column whose lists
            // differ gets a copy of its own for AVATAR, so neither view lays the
            // other's bridges.
            const std::vector<Bridge> top_bridges = findBridges(hf, grid.walkableClimb, 1);
            const std::vector<Bridge> avatar_bridges = findBridges(view.mHf, cfg.walkableClimb, AREA_CODE_BASE);
            {
                const auto column = [&](const Bridge& b) { return b.mX + b.mZ * hf.width; };
                std::map<S32, std::vector<Bridge>> by_top, by_avatar;
                for (const Bridge& b : top_bridges)
                {
                    by_top[column(b)].push_back(b);
                }
                for (Bridge b : avatar_bridges)
                {
                    b.mX += inset;
                    b.mZ += inset;
                    by_avatar[column(b)].push_back(b);
                }
                std::vector<S32> differ;
                for (const auto& [c, list] : by_avatar)
                {
                    const auto it = by_top.find(c);
                    if (it == by_top.end() || it->second != list)
                    {
                        differ.push_back(c);
                    }
                }
                for (const auto& [c, list] : by_top)
                {
                    const S32 x = c % hf.width - inset, z = c / hf.width - inset;
                    if (!by_avatar.count(c) && x >= 0 && z >= 0 && x < cfg.width && z < cfg.height)
                    {
                        differ.push_back(c);
                    }
                }
                if (!differ.empty() && !overlay)
                {
                    overlay.reset(makeHeightfield(grid));
                    if (!overlay)
                    {
                        result.mError = "heightfield";
                        return nullptr;
                    }
                }
                if (own_column.empty() && !differ.empty())
                {
                    own_column.assign(columns, 0);
                }
                for (const S32 c : differ)
                {
                    if (!own_column[c])
                    {
                        own_column[c] = 1;
                        overlay->spans[c] = nullptr;
                        copyColumn(hf, *overlay, c % hf.width, c / hf.width);
                    }
                }
                for (const auto& [c, list] : by_avatar)
                {
                    if (!own_column.empty() && own_column[c])
                    {
                        addBridges(*overlay, list, cfg.walkableClimb);
                    }
                }
                addBridges(hf, top_bridges, grid.walkableClimb);
            }
            if (astats)
            {
                astats->mBridges = static_cast<U32>(avatar_bridges.size());
            }
            if (tstats)
            {
                tstats->mBridges = static_cast<U32>(top_bridges.size());
            }
            if (stats)
            {
                // The lists against each other over the cells AVATAR reads.
                std::vector<Bridge> top_in;
                for (const Bridge& b : top_bridges)
                {
                    const S32 x = b.mX - inset, z = b.mZ - inset;
                    if (x > 0 && z > 0 && x + 1 < cfg.width && z + 1 < cfg.height)
                    {
                        top_in.push_back(b);
                    }
                }
                std::vector<Bridge> avatar_in = avatar_bridges;
                for (Bridge& b : avatar_in)
                {
                    b.mX += inset;
                    b.mZ += inset;
                }
                stats->mTopOnlyBridges = bridgesMissing(top_in, avatar_in);
                stats->mAvatarOnlyBridges = bridgesMissing(avatar_in, top_in);
                stats->mOwnColumns = static_cast<U32>(std::count(own_column.begin(), own_column.end(), 1));
            }
            clock.lap(ClassBuildStats::BRIDGES);
            cullUnderground(hf, input.mTerrain);
            if (overlay)
            {
                cullUnderground(*overlay, input.mTerrain);
            }
            clock.lap(ClassBuildStats::CULL);

            // The air's filters first, kept aside, then AVATAR's over the same spans.
            const bool top_pass = input.mAir || input.mRunoff;
            AreaSnapshot unfiltered, top_areas, top_ledges;
            if (top_pass)
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                if (tstats)
                {
                    tstats->mBuilt = true;
                }
                unfiltered.take(hf);
                filterSpans(hf, tp, grid.ch, grid.cs, input.mRunoff ? &top_ledges : nullptr);
                top_clock.lap(ClassBuildStats::FILTERS);
                if (tstats)
                {
                    tstats->mHfHash = hashHeightfield(hf, 0, 0, hf.width, hf.height, {});
                    tstats->mHfAreaHash = hashHeightfield(hf, 0, 0, hf.width, hf.height, real);
                    top_clock.restart();
                }
                top_areas.take(hf);
                unfiltered.restore(hf);
                top_clock.lap(ClassBuildStats::FILTERS);
                clock.restart();
            }
            view.point(hf, overlay.get(), &own_column);
            maskTopOnly(view.mHf);
            filterSpans(view.mHf, ap, cfg.ch, cfg.cs);
            clock.lap(ClassBuildStats::FILTERS);
            std::vector<U8> code_lut;
            if (astats)
            {
                code_lut.resize(AREA_CODE_MAX + 1);
                for (S32 a = 0; a <= AREA_CODE_MAX; ++a)
                {
                    code_lut[a] = static_cast<U8>(a >= AREA_CODE_BASE ? a - AREA_TOP_ONLY : 0);
                }
                astats->mHfHash = hashHeightfield(view.mHf, 0, 0, cfg.width, cfg.height, {});
                astats->mHfAreaHash = hashHeightfield(view.mHf, 0, 0, cfg.width, cfg.height, code_lut);
                clock.restart();
            }

            mem.chf = rcAllocCompactHeightfield();
            if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, view.mHf, *mem.chf))
            {
                result.mError = "compact";
                return nullptr;
            }
            clock.lap(ClassBuildStats::COMPACT);
            // Which of the raster's spans AVATAR stands on, for the air: its own
            // where it shares the span, else a span of its own column at that top.
            SpanFlags avatar_floor;
            if (top_pass)
            {
                avatar_floor.mStart.assign(1, 0);
                for (S32 c = 0; c < hf.width * hf.height; ++c)
                {
                    const S32 x = c % hf.width - inset, z = c / hf.width - inset;
                    const bool inside = x >= 0 && z >= 0 && x < cfg.width && z < cfg.height;
                    const bool own = inside && !own_column.empty() && own_column[c];
                    for (const rcSpan* s = hf.spans[c]; s; s = s->next)
                    {
                        bool walk = inside && !own && s->area != RC_NULL_AREA;
                        for (const rcSpan* o = own ? overlay->spans[c] : nullptr; o && !walk; o = o->next)
                        {
                            walk = o->smax == s->smax && o->area != RC_NULL_AREA;
                        }
                        avatar_floor.mFlags.push_back(walk ? AirGap::AVATAR_FLOOR : 0);
                    }
                    avatar_floor.mStart.push_back(static_cast<U32>(avatar_floor.mFlags.size()));
                }
                clock.restart();
            }
            // Faces off AVATAR's view with the window's columns back in, their
            // areas the air's: the faces of every solid, door leaves included.
            if (input.mWallFaces)
            {
                if (!in_window.empty())
                {
                    if (top_pass)
                    {
                        top_areas.restore(hf, &in_window);
                    }
                    std::vector<U8> faces_own = own_column;
                    for (size_t c = 0; c < columns; ++c)
                    {
                        faces_own[c] = own_column[c] && !in_window[c];
                    }
                    view.point(hf, overlay.get(), &faces_own);
                }
                extractWallFaces(view.mHf, cfg.borderSize, input.mWallMinHeight, result.mFaces, real.data());
                view.point(hf, overlay.get(), &own_column);
            }
            if (top_pass)
            {
                top_areas.restore(hf);
            }
            clock.lap(ClassBuildStats::WALLS);

            // The tops as rasterised: the air's gaps sit over them, not over the floor a kerb is lowered to.
            std::vector<U16> tops;
            if (input.mAir)
            {
                tops.resize(static_cast<size_t>(mem.chf->spanCount));
                for (S32 i = 0; i < mem.chf->spanCount; ++i)
                {
                    tops[i] = static_cast<U16>(mem.chf->spans[i].y);
                }
            }
            flattenNarrowSteps(*mem.chf, 2 * cfg.walkableRadius, cfg.walkableClimb);
            if (cfg.walkableRadius > 0 && !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *mem.chf))
            {
                result.mError = "erode";
                return nullptr;
            }
            clock.lap(ClassBuildStats::ERODE);
            if (input.mPartition == Partition::SLOPE)
            {
                mergeFragments(*mem.chf, real, flights);
                clock.lap(ClassBuildStats::REGIONS);
                if (astats)
                {
                    astats->mChfHash = hashCompact(*mem.chf, code_lut);
                    clock.restart();
                }
            }

            // The air off the raster with the air's areas, marked with the flights
            // still standing on AVATAR's floor, before any polys exist.
            std::vector<U8> standing(flights.size(), 0);
            for (size_t f = 0; f < flights.size(); ++f)
            {
                standing[f] = flightSurvives(*mem.chf, cfg.borderSize, flights[f], ap.mClimb);
            }
            AirGrid fine;
            if (input.mAir)
            {
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                std::vector<Flight> marked;
                for (size_t f = 0; f < flights.size(); ++f)
                {
                    if (standing[f])
                    {
                        marked.push_back(flights[f]);
                    }
                }
                const S32 overlap = static_cast<S32>(std::ceil(0.5f / grid.ch));
                result.mAir = buildTileAir(hf, grid.borderSize, input.mAirPad, grid.walkableClimb, overlap,
                                          static_cast<S32>(std::ceil(input.mAirMinGap / grid.ch)), &marked,
                                          tstats ? &tstats->mAir : nullptr, real.data(), &avatar_floor, &fine);
                top_clock.lap(ClassBuildStats::AIR);
                clock.restart();
            }
            // Where the air's floors end over a drop, and the run-off down them.
            if (input.mRunoff && top_pass)
            {
                // Off the floors before the ledge filter: a roof's rim over a long
                // drop is where weather runs off, though the filter takes it.
                StageClock top_clock(tstats ? tstats->mMs : nullptr);
                top_ledges.restore(hf);
                extractRunoff(hf, grid.borderSize, grid.walkableClimb, static_cast<S32>(std::ceil(tp.mHeight / grid.ch)), input.mRunoffMax,
                              input.mDropSpacing, result.mLedges, result.mRunoff);
                top_clock.lap(ClassBuildStats::EDGES);
                clock.restart();
            }

            // Past the air's radius the spaces still need air: the tile again at the coarse cell, in this tile's cells.
            TileAirPtr coarse;
            if (!input.mAir && input.mCoarseAir)
            {
                const auto coarse_started = std::chrono::steady_clock::now();
                coarse = coarseAirOf(input, ts);
                if (stats)
                {
                    stats->mCoarseAirMs = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - coarse_started).count();
                }
                clock.restart();
            }

            // What the polygons are built from, kept; the heightfields go.
            auto raster = std::make_shared<TileRaster>();
            takeEdgeField(view.mHf, raster->mEdges);
            raster->mReal = std::move(real);
            raster->mFlights = std::move(flights);
            raster->mStanding = std::move(standing);
            if (result.mAir)
            {
                spanGaps(*mem.chf, cfg.borderSize, fine, *result.mAir, raster->mSpanGap, &tops);
                raster->mAir = result.mAir;
                if (stats)
                {
                    // AVATAR's floors the air has no gap over: under door leaves, in columns of AVATAR's own, or neither.
                    const rcCompactHeightfield& chf = *mem.chf;
                    for (S32 z = cfg.borderSize; z < chf.height - cfg.borderSize; ++z)
                    {
                        for (S32 x = cfg.borderSize; x < chf.width - cfg.borderSize; ++x)
                        {
                            const rcCompactCell& cell = chf.cells[x + z * chf.width];
                            const size_t c = static_cast<size_t>(x + inset) + static_cast<size_t>(z + inset) * hf.width;
                            for (U32 i = cell.index, n = cell.index + cell.count; i < n; ++i)
                            {
                                if (chf.areas[i] == RC_NULL_AREA)
                                {
                                    continue;
                                }
                                ++stats->mSpanGapSpans;
                                if (raster->mSpanGap[i] != SPAN_NO_GAP)
                                {
                                    continue;
                                }
                                ++stats->mSpanGapMisses;
                                if (!in_window.empty() && in_window[c])
                                {
                                    ++stats->mSpanGapWindowMisses;
                                }
                                else if (!own_column.empty() && own_column[c])
                                {
                                    ++stats->mSpanGapOwnMisses;
                                }
                            }
                        }
                    }
                }
            }
            if (coarse)
            {
                const S32 z_shift = static_cast<S32>(std::lround((mem.chf->bmin[1] - coarse->mOrigin.mV[VZ]) / coarse->mUnit));
                leafSpanGaps(*mem.chf, cfg.borderSize, *coarse, z_shift, static_cast<S32>(std::lround(COARSE_AIR_M / input.mCell)), raster->mSpanGap);
                raster->mAir = coarse;
                result.mAir = coarse;
                result.mAirBuilt = true;
                result.mCoarseAir = true;
            }
            raster->mChf.reset(mem.chf);
            mem.chf = nullptr;
            if (astats)
            {
                measureHeightfield(hf, *astats);
                if (overlay)
                {
                    ClassBuildStats extra;
                    measureHeightfield(*overlay, extra);
                    astats->mHfSpans += extra.mHfSpans;
                    astats->mHfBytes += extra.mHfBytes;
                }
            }
            result.mMillis = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (stats)
            {
                stats->mExcludeMs = result.mExcludeMillis;
                stats->mRasterMs = result.mMillis;
                stats->mTotalMs = result.mMillis;
                stats->mHeldChfBytes = raster->chfBytes();
                stats->mHeldEdgeBytes = raster->mEdges.bytes();
                stats->mHeldSpanGapBytes = raster->mSpanGap.capacity() * sizeof(U32);
                stats->mHeldBytes = raster->bytes();
            }
            return raster;
        }

        TileAirPtr coarseAirOf(const TileBuildInput& input, const TileSoup& ts)
        {
            const S32 factor = static_cast<S32>(std::lround(COARSE_AIR_M / std::max(input.mCell, 1e-3f)));
            if (factor < 1 || std::fabs(factor * input.mCell - COARSE_AIR_M) > 1e-4f)
            {
                return nullptr;
            }
            // The soup and its codes do not depend on the cell: the partition's slope is pinned to the fine tile's.
            TileBuildInput c = input;
            c.mCell = COARSE_AIR_M;
            c.mAir = true;
            c.mCoarseAir = false;
            c.mAirPad = airPadCells(c.mCell);
            c.mPartitionSlope = partitionSlope(input);
            c.mWallFaces = false;
            c.mRunoff = false;
            for (bool& e : c.mEdges)
            {
                e = false;
            }
            TileBuildResult cr;
            cr.mAirBuilt = true;
            return rasteriseSoup(c, ts, cr, nullptr, std::chrono::steady_clock::now()) && cr.mAir ? upsampleAir(*cr.mAir, factor) : nullptr;
        }
    }

    TileAirPtr buildCoarseAir(const TileBuildInput& input)
    {
        TileSoup ts;
        TileBuildResult r;
        return soupTile(input, r, nullptr, std::chrono::steady_clock::now(), ts) ? coarseAirOf(input, ts) : nullptr;
    }

    TileRasterPtr rasteriseTile(const TileBuildInput& input, TileBuildResult& result, TileBuildStats* stats)
    {
        const auto started = std::chrono::steady_clock::now();
        // An empty tile has no air to keep, which is an answer too.
        result.mAirBuilt = input.mAir;
        TileSoup ts;
        return soupTile(input, result, stats, started, ts) ? rasteriseSoup(input, ts, result, stats, started) : nullptr;
    }

    void polygoniseTile(const TileBuildInput& input, const TileRaster& raster, TileBuildResult& result, TileBuildStats* stats,
                        const TileStampInput* stamp_input, const TileStamp* stamp)
    {
        const auto started = std::chrono::steady_clock::now();
        rcContext ctx(false);
        const S32 avatar = static_cast<S32>(AgentClass::AVATAR);
        const ClassParams& ap = input.mParams[avatar];
        ClassBuildStats* astats = stats ? &stats->mClass[avatar] : nullptr;
        StageClock clock(astats ? astats->mMs : nullptr);
        const rcConfig cfg = tileConfig(input, ap, borderCells(ap.mRadius, input.mCell));
        const std::vector<U8>& real = raster.mReal;
        const std::vector<U8>& standing = raster.mStanding;
        std::vector<Flight> flights = raster.mFlights;
        const auto finish = [&]() {
            result.mMillis += std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (stats)
            {
                stats->mPolyMs = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
                stats->mTotalMs = stats->mRasterMs + stats->mPolyMs;
            }
        };

        // Regions rewrite the spans and the unfold their areas: a copy, so the raster can polygonise again.
        Freed mem;
        mem.chf = raster.mChf ? cloneCompact(*raster.mChf) : nullptr;
        if (!mem.chf)
        {
            result.mError = "compact copy";
            finish();
            return;
        }
        const bool stamped = stamp_input && stamp && stamp->mSpanCode.size() == static_cast<size_t>(mem.chf->spanCount);
        if (stamped)
        {
            // Each span's palette code: a region grows within one, so within one space.
            std::memcpy(mem.chf->areas, stamp->mSpanCode.data(), stamp->mSpanCode.size());
        }
        bool regions = false;
        switch (input.mPartition)
        {
            case Partition::SLOPE:
                regions = rcBuildDistanceField(&ctx, *mem.chf) &&
                          rcBuildRegions(&ctx, *mem.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea);
                break;
            case Partition::MONOTONE:
                regions = rcBuildRegionsMonotone(&ctx, *mem.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea);
                break;
            case Partition::LAYERS:
                regions = rcBuildLayerRegions(&ctx, *mem.chf, cfg.borderSize, cfg.minRegionArea);
                break;
            default:
                regions = rcBuildDistanceField(&ctx, *mem.chf) &&
                          rcBuildRegions(&ctx, *mem.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea);
                break;
        }
        if (!regions)
        {
            result.mError = "regions";
            finish();
            return;
        }
        clock.lap(ClassBuildStats::REGIONS);
        mem.cset = rcAllocContourSet();
        if (!mem.cset || !rcBuildContours(&ctx, *mem.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *mem.cset))
        {
            result.mError = "contours";
            finish();
            return;
        }
        clock.lap(ClassBuildStats::CONTOURS);
        mem.pmesh = rcAllocPolyMesh();
        if (!mem.pmesh || !rcBuildPolyMesh(&ctx, *mem.cset, cfg.maxVertsPerPoly, *mem.pmesh))
        {
            result.mError = "polymesh";
            finish();
            return;
        }
        clock.lap(ClassBuildStats::POLYMESH);
        // Refs number a tile's polys in 12 bits: more would alias silently.
        if (mem.pmesh->npolys > MAX_POLYS_PER_TILE)
        {
            result.mPolyOverflow = true;
            result.mPolys[avatar] = mem.pmesh->npolys;
            result.mError = "polys over the tile's budget";
            finish();
            return;
        }
        if (stamped)
        {
            stampGeometry(*mem.pmesh, *mem.chf, cfg.borderSize, raster.mSpanGap, *stamp_input, *stamp, result.mStamp);
            for (S32 i = 0; i < mem.chf->spanCount; ++i)
            {
                mem.chf->areas[i] = stamp->mPalette[mem.chf->areas[i]].mSlope;
            }
            for (S32 i = 0; i < mem.pmesh->npolys; ++i)
            {
                mem.pmesh->areas[i] = stamp->mPalette[mem.pmesh->areas[i]].mSlope;
            }
            clock.lap(ClassBuildStats::STAMP);
        }
        if (astats)
        {
            astats->mChfSpans = static_cast<U32>(mem.chf->spanCount);
            astats->mChfBytes = static_cast<size_t>(mem.chf->width) * mem.chf->height * sizeof(rcCompactCell) +
                                static_cast<size_t>(mem.chf->spanCount) * (sizeof(rcCompactSpan) + 1 + (mem.chf->dist ? 2 : 0));
            astats->mWalkableSpans = static_cast<U32>(mem.chf->spanCount - std::count(mem.chf->areas, mem.chf->areas + mem.chf->spanCount, RC_NULL_AREA));
            recordPolySpans(*mem.chf, *mem.pmesh, cfg.borderSize, cfg.tileSize, raster.mSpanGap, *stats);
            clock.restart();
        }
        {
            // each flight's polys by their code, then unfold the codes:
            // everything past here reads real areas
            std::vector<LLVector3> centroids(mem.pmesh->npolys);
            for (S32 i = 0; i < mem.pmesh->npolys; ++i)
            {
                const unsigned short* poly = &mem.pmesh->polys[i * mem.pmesh->nvp * 2];
                S32 nv = 0;
                for (; nv < mem.pmesh->nvp && poly[nv] != RC_MESH_NULL_IDX; ++nv)
                {
                    const unsigned short* v = &mem.pmesh->verts[poly[nv] * 3];
                    const F32 rc[3] = { mem.pmesh->bmin[0] + v[0] * mem.pmesh->cs, mem.pmesh->bmin[1] + v[1] * mem.pmesh->ch,
                                        mem.pmesh->bmin[2] + v[2] * mem.pmesh->cs };
                    centroids[i] += fromRecast(rc);
                }
                centroids[i] /= static_cast<F32>(llmax(nv, 1));
            }
            std::vector<U64> stood;
            for (size_t f = 0; f < flights.size(); ++f)
            {
                if (standing[f])
                {
                    stood.push_back(flights[f].mKey);
                }
            }
            mapFlightPolys(flights, centroids, std::vector<U8>(mem.pmesh->areas, mem.pmesh->areas + mem.pmesh->npolys), ap.mClimb);
            if (stats)
            {
                // The air's flights against the polys': region removal can take a standing flight's polys.
                for (const U64 key : stood)
                {
                    if (std::none_of(flights.begin(), flights.end(), [&](const Flight& f) { return f.mKey == key; }))
                    {
                        stats->mFlightStandOnly.push_back(key);
                    }
                }
                for (const Flight& f : flights)
                {
                    if (std::find(stood.begin(), stood.end(), f.mKey) == stood.end())
                    {
                        stats->mFlightPolysOnly.push_back(f.mKey);
                    }
                }
            }
            result.mFlights[avatar] = std::move(flights);
            for (S32 i = 0; i < mem.chf->spanCount; ++i)
            {
                mem.chf->areas[i] = real[mem.chf->areas[i]];
            }
            for (S32 i = 0; i < mem.pmesh->npolys; ++i)
            {
                mem.pmesh->areas[i] = real[mem.pmesh->areas[i]];
            }
        }
        clock.lap(ClassBuildStats::FLIGHTMAP);
        mem.dmesh = rcAllocPolyMeshDetail();
        if (!mem.dmesh || !rcBuildPolyMeshDetail(&ctx, *mem.pmesh, *mem.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *mem.dmesh))
        {
            result.mError = "detail";
            finish();
            return;
        }
        // Recast hands the detail vertices back one cell higher than the
        // polygon vertices they refine (RecastMeshDetail.cpp, "Is this
        // offset necessary?"), and Detour keeps the polygon's own corners
        // at their unshifted height, so every vertex the detail pass adds
        // would stand a cell proud of the corners around it and tilt the
        // triangles between them. Take the offset back out.
        for (int v = 0; v < mem.dmesh->nverts; ++v)
        {
            mem.dmesh->verts[v * 3 + 1] -= cfg.ch;
        }
        clock.lap(ClassBuildStats::DETAIL);
        result.mPolys[avatar] = mem.pmesh->npolys;
        if (astats)
        {
            astats->mPolys = static_cast<U32>(mem.pmesh->npolys);
            astats->mVerts = static_cast<U32>(mem.pmesh->nverts);
            astats->mDetailTris = static_cast<U32>(mem.dmesh->ntris);
            astats->mFlights = static_cast<U32>(result.mFlights[avatar].size());
            for (const Flight& f : result.mFlights[avatar])
            {
                astats->mFlightPolys += static_cast<U32>(f.mPolys.size());
                astats->mFlightCodes += static_cast<U32>(f.mCodes.size());
            }
            stats->mPolyFlight.assign(mem.pmesh->npolys, 0);
            for (const Flight& f : result.mFlights[avatar])
            {
                for (const U16 poly : f.mPolys)
                {
                    if (poly < stats->mPolyFlight.size())
                    {
                        stats->mPolyFlight[poly] = 1;
                    }
                }
            }
            clock.restart();
        }
        if (mem.pmesh->npolys == 0)
        {
            result.mStatus = TileBuildResult::SUCCESS;
            finish();
            return;
        }
        if (input.mEdges[avatar])
        {
            classifyEdges(*mem.pmesh, raster.mEdges, *mem.chf, ap, input.mDropMax[avatar], true, input.mDropSpacing, result.mSegments[avatar],
                          result.mLinks[avatar]);
        }
        clock.lap(ClassBuildStats::EDGES);
        for (int i = 0; i < mem.pmesh->npolys; ++i)
        {
            const U8 area = mem.pmesh->areas[i] == RC_WALKABLE_AREA ? AREA_OBJECT : mem.pmesh->areas[i];
            mem.pmesh->areas[i] = area;
            mem.pmesh->flags[i] = area ? static_cast<unsigned short>(1u << area) : 0;
        }
        // Flights are part of the geometry; where a poly is and what it touches are bound at publish.
        for (const Flight& f : result.mFlights[avatar])
        {
            for (const U16 p : f.mPolys)
            {
                if (p < mem.pmesh->npolys && mem.pmesh->flags[p])
                {
                    mem.pmesh->flags[p] |= POLY_FLIGHT;
                }
            }
        }

        // A drop link becomes a one-way off-mesh connection, so findPath
        // may step off a ledge onto the floor below.
        std::vector<F32> con_verts;
        std::vector<F32> con_rad;
        std::vector<U8> con_dir;
        std::vector<U8> con_areas;
        std::vector<unsigned short> con_flags;
        std::vector<unsigned int> con_ids;
        for (const DropLink& link : result.mLinks[avatar])
        {
            F32 from[3], to[3];
            toRecast(link.mFrom, from);
            toRecast(link.mTo, to);
            con_verts.insert(con_verts.end(), from, from + 3);
            con_verts.insert(con_verts.end(), to, to + 3);
            con_rad.push_back(1.f);
            con_dir.push_back(0);   // one way: you can drop, not climb
            con_areas.push_back(AREA_DROP);
            con_flags.push_back(static_cast<unsigned short>(1u << AREA_DROP));
            con_ids.push_back(static_cast<unsigned int>(con_ids.size()));
        }

        dtNavMeshCreateParams params;
        std::memset(&params, 0, sizeof(params));
        if (!con_rad.empty())
        {
            params.offMeshConVerts = con_verts.data();
            params.offMeshConRad = con_rad.data();
            params.offMeshConDir = con_dir.data();
            params.offMeshConAreas = con_areas.data();
            params.offMeshConFlags = con_flags.data();
            params.offMeshConUserID = con_ids.data();
            params.offMeshConCount = static_cast<int>(con_rad.size());
        }
        params.verts = mem.pmesh->verts;
        params.vertCount = mem.pmesh->nverts;
        params.polys = mem.pmesh->polys;
        params.polyAreas = mem.pmesh->areas;
        params.polyFlags = mem.pmesh->flags;
        params.polyCount = mem.pmesh->npolys;
        params.nvp = mem.pmesh->nvp;
        params.detailMeshes = mem.dmesh->meshes;
        params.detailVerts = mem.dmesh->verts;
        params.detailVertsCount = mem.dmesh->nverts;
        params.detailTris = mem.dmesh->tris;
        params.detailTriCount = mem.dmesh->ntris;
        params.walkableHeight = ap.mHeight;
        params.walkableRadius = ap.mRadius;
        params.walkableClimb = ap.mClimb;
        params.tileX = input.mTileY;    // Detour x runs along Recast x = SL y
        params.tileY = input.mTileX;
        params.tileLayer = 0;
        rcVcopy(params.bmin, mem.pmesh->bmin);
        rcVcopy(params.bmax, mem.pmesh->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;

        unsigned char* data = nullptr;
        int size = 0;
        if (!dtCreateNavMeshData(&params, &data, &size))
        {
            result.mError = "navmesh data";
            finish();
            return;
        }
        result.mData[avatar].assign(data, data + size);
        dtFree(data);
        clock.lap(ClassBuildStats::DETOUR);
        if (astats)
        {
            astats->mDetourBytes = static_cast<U32>(size);
        }
        result.mStatus = TileBuildResult::SUCCESS;
        finish();
    }

    TileBuildResult buildTile(const TileBuildInput& input, TileBuildStats* stats)
    {
        TileBuildResult result;
        if (const TileRasterPtr raster = rasteriseTile(input, result, stats))
        {
            polygoniseTile(input, *raster, result, stats);
        }
        return result;
    }
}
