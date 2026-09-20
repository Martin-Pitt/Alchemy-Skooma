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

        void add(const std::vector<LLVector3>& tris, U8 area, F32 water)
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
            }
        }
        S32 nverts() const { return static_cast<S32>(mVerts.size() / 3); }
        S32 ntris() const { return static_cast<S32>(mTris.size() / 3); }
    };

    // Mark walkable by slope, keeping the source area; non-walkable stays
    // RC_NULL_AREA and still rasterises as an obstacle.
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
            if (n[1] > threshold)
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
    void fillConvex(rcHeightfield& hf, const Soup& soup, S32 tri_begin, S32 tri_end, S32 climb)
    {
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
        const S32 x0 = std::max(0, static_cast<S32>(std::floor((bmin[0] - hf.bmin[0]) / hf.cs)));
        const S32 x1 = std::min(hf.width - 1, static_cast<S32>(std::floor((bmax[0] - hf.bmin[0]) / hf.cs)));
        const S32 z0 = std::max(0, static_cast<S32>(std::floor((bmin[2] - hf.bmin[2]) / hf.cs)));
        const S32 z1 = std::min(hf.height - 1, static_cast<S32>(std::floor((bmax[2] - hf.bmin[2]) / hf.cs)));
        if (x1 < x0 || z1 < z0)
        {
            return;
        }
        const S32 w = x1 - x0 + 1, h = z1 - z0 + 1;
        std::vector<F32> lo(static_cast<size_t>(w) * h, FLT_MAX), hi(static_cast<size_t>(w) * h, -FLT_MAX);

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
                    if (l0 < 0.f || l1 < 0.f || l2 < 0.f)
                    {
                        continue;
                    }
                    const F32 y = l0 * a[1] + l1 * b[1] + l2 * c[1];
                    const size_t i = static_cast<size_t>(cz - z0) * w + (cx - x0);
                    lo[i] = std::min(lo[i], y);
                    hi[i] = std::max(hi[i], y);
                }
            }
        }

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
                // The fill carries no area: it is the inside of something, never
                // a floor. Merging keeps the surfaces' own area, so the body's
                // roof stays walkable.
                rcAddSpan(&ctx, hf, cx, cz, static_cast<unsigned short>(smin),
                          static_cast<unsigned short>(smax), RC_NULL_AREA, climb);
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
            tris.clear();
            emitRecord(*r, input.mMeshOrigin, tris);
            const S32 before = soup.ntris();
            soup.add(tris, AREA_OBJECT, input.mTerrain.mWater);
            if (r->mShape != Shape::TRIANGLES)
            {
                convex.emplace_back(before, soup.ntris());
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
            soup.add(tris, AREA_GROUND, t.mWater);
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
            cfg.detailSampleDist = 6.f * cfg.cs;
            cfg.detailSampleMaxError = 2.f * cfg.ch;
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
            rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *mem.hf);
            rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *mem.hf);
            rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *mem.hf);

            mem.chf = rcAllocCompactHeightfield();
            if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *mem.hf, *mem.chf))
            {
                result.mError = "compact";
                return result;
            }
            if (cfg.walkableRadius > 0 && !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *mem.chf))
            {
                result.mError = "erode";
                return result;
            }
            if (!rcBuildDistanceField(&ctx, *mem.chf) || !rcBuildRegions(&ctx, *mem.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
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
                extractWallFaces(*mem.hf, cfg.borderSize, result.mFaces);
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
