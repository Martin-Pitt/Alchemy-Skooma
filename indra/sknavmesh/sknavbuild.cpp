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

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"
#include "sknavwalls.h"

#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
    using namespace sknav;

    const F32 VOID_Z = -900.f;      ///< terrain samples below this have no region
    const S32 CYLINDER_SIDES = 16;
    const S32 SPHERE_RINGS = 6;
    const S32 SPHERE_SEGMENTS = 12;

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

        void add(const std::vector<LLVector3>& tris, U8 area, F32 water, bool two_sided)
        {
            const size_t base = mVerts.size() / 3;
            for (size_t i = 0; i + 2 < tris.size(); i += 3)
            {
                F32 p[3];
                bool under = true;
                for (S32 k = 0; k < 3; ++k)
                {
                    toRecast(tris[i + k], p);
                    mVerts.insert(mVerts.end(), p, p + 3);
                    under = under && tris[i + k].mV[VZ] < water;
                }
                const S32 v = static_cast<S32>(base + i);
                mTris.insert(mTris.end(), { v, v + 1, v + 2 });
                mAreas.push_back(under && area == AREA_GROUND ? AREA_SUBMERGED : area);
                mTwoSided.push_back(two_sided);
            }
        }
        S32 nverts() const { return static_cast<S32>(mVerts.size() / 3); }
        S32 ntris() const { return static_cast<S32>(mTris.size() / 3); }
    };

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

    /// Recast rasterises surfaces, so a closed body arrives hollow: the floor
    /// it stands on stays walkable inside it with the body's own roof as
    /// headroom, and the navmesh offers a storey inside every solid object.
    /// A convex body is exactly the span between its lowest and highest
    /// surface in each column, so fill that in and the inside is gone.
    ///
    /// A column counts as covered when the body's surface passes over its
    /// centre. A body too narrow to cover any centre is therefore not filled,
    /// which costs nothing: such a body has no interior column to be hollow,
    /// and the side faces that rasterise across its cells already make it
    /// solid there.
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

    void fillConvex(rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end, S32 climb)
    {
        // Havok pads every convex shape by 5 cm, so two solids leave a 10 cm
        // gap; the census's hulls carry none and read small at their edges,
        // which leaves a navmesh gap the sim does not have.
        const F32 pad = 0.05f;
        BodyExtent body;
        bodyExtent(hf, soup, tri_begin, tri_end, body, pad);
        if (body.mW <= 0 || body.mH <= 0)
        {
            return;
        }
        const S32 x0 = body.mX0, z0 = body.mZ0, w = body.mW, h = body.mH;
        const S32 x1 = x0 + w - 1, z1 = z0 + h - 1;
        const std::vector<F32>& lo = body.mLo;
        const std::vector<F32>& hi = body.mHi;

        rcContext ctx(false);
        for (S32 cz = z0; cz <= z1; ++cz)
        {
            for (S32 cx = x0; cx <= x1; ++cx)
            {
                const size_t i = static_cast<size_t>(cz - z0) * w + (cx - x0);
                if (lo[i] > hi[i])
                {
                    continue;
                }
                const F32 top = std::min(hi[i], hf.bmax[1]);
                const F32 bottom = std::max(lo[i], hf.bmin[1]);
                if (top <= bottom)
                {
                    continue;
                }
                // Spans are cell counts in an unsigned short and Recast caps
                // them; a megaprim reaching a kilometre would otherwise wrap.
                S32 smin = static_cast<S32>(std::floor((bottom - hf.bmin[1]) / hf.ch));
                S32 smax = static_cast<S32>(std::ceil((top - hf.bmin[1]) / hf.ch));
                smin = llclamp(smin, 0, RC_SPAN_MAX_HEIGHT);
                smax = llclamp(smax, 0, RC_SPAN_MAX_HEIGHT);
                if (smax <= smin)
                {
                    continue;
                }
                // A body whose inside holds a walkable surface is not solid
                // there: that surface is what the agent stands on, and one
                // fill over it takes both away - the surface itself and the
                // one below it that should have been dropped in its favour.
                // Leave the column to the stacked-surface rule, which keeps
                // the higher surface and discards the one within the agent's
                // height below it.
                bool holds_surface = false;
                for (const rcSpan* s = hf.spans[cx + cz * hf.width]; s; s = s->next)
                {
                    if (s->area != RC_NULL_AREA && static_cast<S32>(s->smin) > smin && static_cast<S32>(s->smax) < smax)
                    {
                        holds_surface = true;
                        break;
                    }
                }
                if (holds_surface)
                {
                    continue;
                }
                // The fill carries no area: it is the inside of something, never
                // a floor. Merging keeps the surfaces' own area, so the body's
                // roof stays walkable.
                rcAddSpan(&ctx, hf, cx, cz, static_cast<unsigned short>(smin),
                          static_cast<unsigned short>(smax), RC_NULL_AREA, climb);
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

    /// A walkable span in the column already within the agent's climb of both
    /// surfaces: they are joined through it and need no bridge.
    bool columnJoins(const rcHeightfield& hf, S32 x, S32 z, S32 lo, S32 hi, S32 climb)
    {
        for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
        {
            if (s->area != RC_NULL_AREA && rcAbs(static_cast<S32>(s->smax) - lo) <= climb &&
                rcAbs(static_cast<S32>(s->smax) - hi) <= climb)
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

    /// Recast joins a span to its neighbours only, so a crevice that leaves a
    /// column without a span at the surfaces' level splits them, and the
    /// erosion that follows widens it: the top of a staircase, solid in the
    /// sim, read as a gap. Join the two sides across one such column when
    /// their tops are within the agent's climb and nothing stands at that
    /// level between them.
    void bridgeCrevices(rcHeightfield& hf, S32 climb)
    {
        struct Bridge
        {
            S32 mX = 0;
            S32 mZ = 0;
            unsigned short mSMin = 0;
            unsigned short mSMax = 0;
            U8 mArea = RC_NULL_AREA;
        };
        std::vector<Bridge> bridges;
        // Collect first, then add: a span laid while scanning would be read as
        // a neighbour by a later column and by the column's other axis, which
        // must judge the heightfield they were given.
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
                        if (a->area == RC_NULL_AREA)
                        {
                            continue;
                        }
                        for (const rcSpan* b = hf.spans[(x + dx) + (z + dz) * hf.width]; b; b = b->next)
                        {
                            if (b->area == RC_NULL_AREA)
                            {
                                continue;
                            }
                            const S32 atop = a->smax, btop = b->smax;
                            if (rcAbs(atop - btop) > climb)
                            {
                                continue;
                            }
                            const S32 lo = std::min(atop, btop), hi = std::max(atop, btop);
                            if (columnJoins(hf, x, z, lo, hi, climb) || creviceBlocked(hf, x, z, lo, hi))
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
        rcContext ctx(false);
        for (const Bridge& bridge : bridges)
        {
            rcAddSpan(&ctx, hf, bridge.mX, bridge.mZ, bridge.mSMin, bridge.mSMax, bridge.mArea, climb);
        }
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

    TileBuildResult buildTile(const TileBuildInput& input)
    {
        const auto started = std::chrono::steady_clock::now();
        TileBuildResult result;

        // Gather the soup in Recast space.
        Soup soup;
        std::vector<LLVector3> tris;
        std::vector<std::pair<S32, S32>> convex;    // triangle ranges of closed bodies
        for (const RecordPtr& r : input.mRecords)
        {
            if (!r)
            {
                continue;
            }
            // A hull is a convex solid the sim collides with from either side:
            // mark it walkable whichever way its triangles wind, and fill it,
            // because the sim collides with the whole body and not just the
            // surface the decomposition happened to hand back.
            const bool hull = r->mProvenance == Provenance::HULL || r->mProvenance == Provenance::BASE_HULL;
            tris.clear();
            emitRecord(*r, input.mMeshOrigin, tris);
            const S32 before = soup.ntris();
            soup.add(tris, AREA_OBJECT, input.mTerrain.mWater, hull);
            // A decomposition is a set of convex bodies, not one: a door frame
            // is a sill, two posts and a lintel, and their union filled from
            // its lowest to its highest surface walls off the opening they
            // exist to leave. Fill each body on its own. A record whose
            // bodies are not told apart arrives as one range and is filled as
            // one, which is what its whole shape has always meant.
            S32 pieces = 0;
            for (const S32 count : r->mConvexTris)
            {
                pieces += count;
            }
            if (hull && r->mConvexTris.size() > 1 && pieces == soup.ntris() - before)
            {
                S32 at = before;
                for (const S32 count : r->mConvexTris)
                {
                    if (count > 0)
                    {
                        convex.emplace_back(at, at + count);
                    }
                    at += count;
                }
            }
            else if (r->mShape != Shape::TRIANGLES || hull)
            {
                convex.emplace_back(before, soup.ntris());
            }
        }
        // An exclusion volume is a cut, not a shape: its triangles never enter
        // the soup, they only say where the navmesh is taken out of.
        Soup cut_soup;
        std::vector<std::pair<S32, S32>> cuts;
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
        if (soup.ntris() == 0)
        {
            result.mStatus = TileBuildResult::EMPTY;
            return result;
        }

        rcContext ctx(false);
        for (S32 cls = 0; cls < NUM_CLASSES; ++cls)
        {
            if (!input.mClasses[cls])
            {
                continue;
            }
            const ClassParams& p = input.mParams[cls];
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
            cfg.borderSize = cfg.walkableRadius + 3;
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

            Freed mem;
            mem.hf = rcAllocHeightfield();
            if (!rcCreateHeightfield(&ctx, *mem.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
            {
                result.mError = "heightfield";
                return result;
            }
            std::vector<U8> areas;
            markWalkable(soup, cfg.walkableSlopeAngle, areas);
            if (!rcRasterizeTriangles(&ctx, soup.mVerts.data(), soup.nverts(), soup.mTris.data(), areas.data(), soup.ntris(), *mem.hf, cfg.walkableClimb))
            {
                result.mError = "rasterize";
                return result;
            }
            for (const auto& range : convex)
            {
                fillConvex(*mem.hf, soup, range.first, range.second, cfg.walkableClimb);
            }
            for (const auto& range : cuts)
            {
                cutVolume(*mem.hf, cut_soup, range.first, range.second);
            }
            bridgeCrevices(*mem.hf, cfg.walkableClimb);
            cullUnderground(*mem.hf, input.mTerrain);
            // The ledge filter marks a span a ledge when the height range of
            // its accessible neighbours exceeds the climb it is given, which
            // reads every slope steeper than atan(climb / two cells) as a
            // ledge: at 0.75 m of climb that is 72 degrees, below both
            // classes' slope limits, so it cuts lines and holes out of exactly
            // the steep ground the class is meant to walk. Give it a climb
            // that matches the class's own slope, and the rule only fires
            // beyond the slope limit, where the slope test has already said
            // no.
            const F32 ledge_climb = llmax(p.mClimb, 2.f * cfg.cs * std::tan(p.mSlope * DEG_TO_RAD));
            rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *mem.hf);
            rcFilterLedgeSpans(&ctx, cfg.walkableHeight, static_cast<int>(std::ceil(ledge_climb / cfg.ch)), *mem.hf);
            rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *mem.hf);

            mem.chf = rcAllocCompactHeightfield();
            if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *mem.hf, *mem.chf))
            {
                result.mError = "compact";
                return result;
            }
            flattenNarrowSteps(*mem.chf, 2 * cfg.walkableRadius, cfg.walkableClimb);
            if (cfg.walkableRadius > 0 && !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *mem.chf))
            {
                result.mError = "erode";
                return result;
            }
            bool regions = false;
            switch (input.mPartition)
            {
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
                return result;
            }
            mem.cset = rcAllocContourSet();
            if (!rcBuildContours(&ctx, *mem.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *mem.cset))
            {
                result.mError = "contours";
                return result;
            }
            mem.pmesh = rcAllocPolyMesh();
            if (!rcBuildPolyMesh(&ctx, *mem.cset, cfg.maxVertsPerPoly, *mem.pmesh))
            {
                result.mError = "polymesh";
                return result;
            }
            mem.dmesh = rcAllocPolyMeshDetail();
            if (!rcBuildPolyMeshDetail(&ctx, *mem.pmesh, *mem.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *mem.dmesh))
            {
                result.mError = "detail";
                return result;
            }
            result.mPolys[cls] = mem.pmesh->npolys;
            if (mem.pmesh->npolys == 0)
            {
                continue;
            }

            // Walls come off the same heightfield, before it goes: the faces
            // once (span geometry is the same for every class), the floor
            // edges and the drops per class.
            if (input.mWallFaces && result.mFaces.empty())
            {
                extractWallFaces(*mem.hf, cfg.borderSize, input.mWallMinHeight, result.mFaces);
            }
            if (input.mEdges[cls])
            {
                classifyEdges(*mem.pmesh, *mem.hf, *mem.chf, p, input.mDropMax[cls],
                              cls == static_cast<S32>(AgentClass::AVATAR), input.mDropSpacing,
                              result.mSegments[cls], result.mLinks[cls]);
            }
            for (int i = 0; i < mem.pmesh->npolys; ++i)
            {
                const U8 area = mem.pmesh->areas[i] == RC_WALKABLE_AREA ? AREA_OBJECT : mem.pmesh->areas[i];
                mem.pmesh->areas[i] = area;
                mem.pmesh->flags[i] = area ? static_cast<unsigned short>(1u << area) : 0;
            }

            // A drop link becomes a one-way off-mesh connection, so findPath
            // may step off a ledge onto the floor below. The weather class's
            // links are drainage only and stay out of the mesh.
            std::vector<F32> con_verts;
            std::vector<F32> con_rad;
            std::vector<U8> con_dir;
            std::vector<U8> con_areas;
            std::vector<unsigned short> con_flags;
            std::vector<unsigned int> con_ids;
            if (cls == static_cast<S32>(AgentClass::AVATAR))
            {
                for (const DropLink& link : result.mLinks[cls])
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
            params.walkableHeight = p.mHeight;
            params.walkableRadius = p.mRadius;
            params.walkableClimb = p.mClimb;
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
                return result;
            }
            result.mData[cls].assign(data, data + size);
            dtFree(data);
        }
        result.mStatus = TileBuildResult::SUCCESS;
        result.mMillis = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - started).count();
        return result;
    }
}
