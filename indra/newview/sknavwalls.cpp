/**
 * @file sknavwalls.cpp
 * @brief Walls off the navmesh heightfield: exposed faces, classified floor edges, drop links.
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

#include "sknavwalls.h"

#include "sknavbuild.h"

#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace
{
    using namespace sknav;

    const F32 WALL_CAP_M = 4.f;         ///< a wall taller than this is "tall"
    const F32 EPS_M = 0.02f;

    struct Interval
    {
        F32 mBottom;
        F32 mTop;
    };

    /// Solid intervals of a heightfield column, ascending, in Recast y metres.
    void column(const rcHeightfield& hf, S32 cx, S32 cz, std::vector<Interval>& out)
    {
        out.clear();
        if (cx < 0 || cz < 0 || cx >= hf.width || cz >= hf.height)
        {
            return;
        }
        for (const rcSpan* s = hf.spans[cx + cz * hf.width]; s; s = s->next)
        {
            out.push_back({ hf.bmin[1] + s->smin * hf.ch, hf.bmin[1] + s->smax * hf.ch });
        }
    }

    /// Whether the compact heightfield has a walkable floor at this column near y.
    bool walkableAt(const rcCompactHeightfield& chf, S32 cx, S32 cz, F32 y)
    {
        if (cx < 0 || cz < 0 || cx >= chf.width || cz >= chf.height)
        {
            return false;
        }
        const rcCompactCell& cell = chf.cells[cx + cz * chf.width];
        for (unsigned int i = cell.index, n = cell.index + cell.count; i < n; ++i)
        {
            const F32 floor = chf.bmin[1] + chf.spans[i].y * chf.ch;
            if (chf.areas[i] != RC_NULL_AREA && std::fabs(floor - y) <= chf.ch * 2.f)
            {
                return true;
            }
        }
        return false;
    }

    struct Sample
    {
        EdgeKind mKind = EdgeKind::OPEN;
        F32 mValue = 0.f;
        F32 mLanding = 0.f;     ///< Recast y of the landing for a ledge
        bool mLandingOk = false;
    };

    Sample classify(const rcHeightfield& hf, S32 cx, S32 cz, F32 floor_y, const ClassParams& p)
    {
        Sample s;
        std::vector<Interval> solids;
        column(hf, cx, cz, solids);

        // The first solid whose top is above the floor.
        size_t above = solids.size();
        for (size_t i = 0; i < solids.size(); ++i)
        {
            if (solids[i].mTop > floor_y + EPS_M)
            {
                above = i;
                break;
            }
        }
        if (above < solids.size())
        {
            const Interval& a = solids[above];
            if (a.mBottom <= floor_y + p.mClimb)
            {
                // Solid from the floor up: a step, or a wall. Stack solids
                // over gaps too short to fit through, so a wall built of two
                // boxes with a hand's breadth between them is one wall.
                F32 top = a.mTop;
                for (size_t i = above + 1; i < solids.size() && solids[i].mBottom - top < p.mHeight; ++i)
                {
                    top = solids[i].mTop;
                }
                const F32 height = top - floor_y;
                if (height <= p.mClimb)
                {
                    s.mKind = EdgeKind::OPEN;   // a step the class walks up; the edge is erosion
                    return s;
                }
                s.mKind = EdgeKind::WALL;
                s.mValue = std::min(height, WALL_CAP_M);
                return s;
            }
            if (a.mBottom < floor_y + p.mHeight)
            {
                s.mKind = EdgeKind::OVERHANG;
                s.mValue = a.mBottom - floor_y;
                return s;
            }
        }

        // Nothing rises from the floor here: is the floor gone?
        F32 top_below = -1e9f;
        for (size_t i = 0; i < solids.size(); ++i)
        {
            if (solids[i].mTop <= floor_y + p.mClimb)
            {
                top_below = std::max(top_below, solids[i].mTop);
            }
        }
        if (top_below >= floor_y - p.mClimb)
        {
            s.mKind = EdgeKind::OPEN;       // the floor continues; erosion or the border
            return s;
        }
        s.mKind = EdgeKind::LEDGE;
        if (top_below > -1e8f)
        {
            s.mValue = floor_y - top_below;
            s.mLanding = top_below;
            s.mLandingOk = true;    // geometry only; the caller decides landability
        }
        else
        {
            s.mValue = -1.f;
        }
        return s;
    }

    /// Is there floor to land on around here? The column directly under a
    /// ledge is inside the erosion skirt of the wall it fell from, so walk
    /// outward a few cells before giving up.
    bool landingWalkable(const rcCompactHeightfield& chf, F32 ox, F32 oz, F32 nx, F32 nz, F32 y, S32 steps)
    {
        for (S32 i = 0; i <= steps; ++i)
        {
            const F32 px = ox + nx * chf.cs * i, pz = oz + nz * chf.cs * i;
            const S32 cx = static_cast<S32>(std::floor((px - chf.bmin[0]) / chf.cs));
            const S32 cz = static_cast<S32>(std::floor((pz - chf.bmin[2]) / chf.cs));
            if (walkableAt(chf, cx, cz, y))
            {
                return true;
            }
        }
        return false;
    }

    struct FaceRun
    {
        S32 mAlong0, mAlong1;   // inclusive cell range along the wall
        S32 mAcross;            // the column across the wall
        F32 mBottom, mTop;
        U8 mArea;
    };
}

namespace sknav
{
    Cover coverClass(F32 wall_height, F32 eye_height, F32 climb, F32 peek)
    {
        if (wall_height >= eye_height - peek)
        {
            return Cover::FULL;
        }
        if (wall_height >= climb)
        {
            return Cover::HALF;
        }
        return Cover::NONE;
    }

    void extractWallFaces(const rcHeightfield& hf, S32 border_cells, std::vector<WallFace>& out)
    {
        // Sides in Recast: 0 +x, 1 -x, 2 +z, 3 -z. Faces of one side, one
        // across-column and one z-interval merge along the wall's direction.
        static const S32 DX[4] = { 1, -1, 0, 0 };
        static const S32 DZ[4] = { 0, 0, 1, -1 };
        std::vector<Interval> theirs;
        for (S32 side = 0; side < 4; ++side)
        {
            const bool alongZ = (side < 2);     // ±x faces run along z
            std::map<std::pair<S32, S32>, std::vector<FaceRun>> runs;  // key: (across, bottom cells)
            const S32 x0 = border_cells, x1 = hf.width - border_cells, z0 = border_cells, z1 = hf.height - border_cells;
            for (S32 cz = z0; cz < z1; ++cz)
            {
                for (S32 cx = x0; cx < x1; ++cx)
                {
                    const rcSpan* first = hf.spans[cx + cz * hf.width];
                    if (!first)
                    {
                        continue;
                    }
                    column(hf, cx + DX[side], cz + DZ[side], theirs);
                    for (const rcSpan* s = first; s; s = s->next)
                    {
                        // Subtract the neighbour's solids from this span's interval.
                        F32 lo = hf.bmin[1] + s->smin * hf.ch;
                        const F32 hi = hf.bmin[1] + s->smax * hf.ch;
                        for (const Interval& t : theirs)
                        {
                            if (t.mTop <= lo || t.mBottom >= hi)
                            {
                                continue;
                            }
                            if (t.mBottom > lo)
                            {
                                const S32 along = alongZ ? cz : cx, across = alongZ ? cx : cz;
                                runs[{ across, static_cast<S32>(std::lround(lo / hf.ch)) }].push_back({ along, along, across, lo, t.mBottom, static_cast<U8>(s->area) });
                            }
                            lo = std::max(lo, t.mTop);
                        }
                        if (lo < hi)
                        {
                            const S32 along = alongZ ? cz : cx, across = alongZ ? cx : cz;
                            runs[{ across, static_cast<S32>(std::lround(lo / hf.ch)) }].push_back({ along, along, across, lo, hi, static_cast<U8>(s->area) });
                        }
                    }
                }
            }
            for (auto& entry : runs)
            {
                std::vector<FaceRun>& list = entry.second;
                std::sort(list.begin(), list.end(), [](const FaceRun& a, const FaceRun& b) { return a.mAlong0 < b.mAlong0; });
                size_t i = 0;
                while (i < list.size())
                {
                    FaceRun run = list[i];
                    size_t j = i + 1;
                    while (j < list.size() && list[j].mAlong0 == run.mAlong1 + 1 && std::fabs(list[j].mTop - run.mTop) < hf.ch * 0.5f && list[j].mArea == run.mArea)
                    {
                        run.mAlong1 = list[j].mAlong0;
                        ++j;
                    }
                    i = j;
                    // Recast cells back to SL mesh-local metres: x = Recast z, y = Recast x, z = Recast y.
                    WallFace f;
                    const F32 rx0 = hf.bmin[0] + (alongZ ? run.mAcross : run.mAlong0) * hf.cs;
                    const F32 rx1 = hf.bmin[0] + (alongZ ? run.mAcross + 1 : run.mAlong1 + 1) * hf.cs;
                    const F32 rz0 = hf.bmin[2] + (alongZ ? run.mAlong0 : run.mAcross) * hf.cs;
                    const F32 rz1 = hf.bmin[2] + (alongZ ? run.mAlong1 + 1 : run.mAcross + 1) * hf.cs;
                    // The face sits on the side's boundary of the column.
                    const F32 fx0 = side == 0 ? rx1 : side == 1 ? rx0 : rx0, fx1 = side == 0 ? rx1 : side == 1 ? rx0 : rx1;
                    const F32 fz0 = side == 2 ? rz1 : side == 3 ? rz0 : rz0, fz1 = side == 2 ? rz1 : side == 3 ? rz0 : rz1;
                    f.mMin.set(std::min(fz0, fz1), std::min(fx0, fx1), run.mBottom);
                    f.mMax.set(std::max(fz0, fz1), std::max(fx0, fx1), run.mTop);
                    // Recast +x is SL +y, Recast +z is SL +x.
                    f.mSide = side == 0 ? 2 : side == 1 ? 3 : side == 2 ? 0 : 1;
                    f.mArea = run.mArea;
                    out.push_back(f);
                }
            }
        }
    }

    void classifyEdges(const rcPolyMesh& pmesh, const rcHeightfield& hf, const rcCompactHeightfield& chf, const ClassParams& params,
                       F32 drop_max, bool walkable_landing, F32 link_spacing,
                       std::vector<WallSegment>& segments, std::vector<DropLink>& links)
    {
        const S32 nvp = pmesh.nvp;
        // Erosion pulled the floor back from whatever bounds it by the agent
        // radius in whole cells, so the wall or the drop is that far beyond the
        // polygon's edge, not just outside it.
        // Erosion pulls the floor back by the agent radius in whole cells, and
        // the ledge filter takes another cell wherever the floor ends in air,
        // so what bounds the floor sits somewhere in that skirt. Probe outward
        // until something answers; nothing but the skirt means an open edge.
        const F32 inset = std::ceil(params.mRadius / hf.cs) * hf.cs;
        const F32 probes[3] = { inset + hf.cs * 0.75f, inset + hf.cs * 1.75f, inset + hf.cs * 2.75f };
        const S32 landing_steps = static_cast<S32>(std::ceil(params.mRadius / hf.cs)) + 2;
        F32 sinceLink = link_spacing;   // a link at the first landable sample, then one per spacing
        auto vert = [&](unsigned short v, F32* out) {
            out[0] = pmesh.bmin[0] + pmesh.verts[v * 3] * pmesh.cs;
            out[1] = pmesh.bmin[1] + pmesh.verts[v * 3 + 1] * pmesh.ch;
            out[2] = pmesh.bmin[2] + pmesh.verts[v * 3 + 2] * pmesh.cs;
        };

        for (S32 i = 0; i < pmesh.npolys; ++i)
        {
            const unsigned short* poly = &pmesh.polys[i * nvp * 2];
            S32 n = 0;
            while (n < nvp && poly[n] != RC_MESH_NULL_IDX)
            {
                ++n;
            }
            if (n < 3)
            {
                continue;
            }
            // Winding in xz decides which side is outside.
            F32 area2 = 0.f;
            for (S32 j = 0; j < n; ++j)
            {
                F32 a[3], b[3];
                vert(poly[j], a);
                vert(poly[(j + 1) % n], b);
                area2 += a[0] * b[2] - b[0] * a[2];
            }
            const F32 outward = area2 > 0.f ? -1.f : 1.f;

            for (S32 j = 0; j < n; ++j)
            {
                const unsigned short nei = poly[nvp + j];
                if (nei != RC_MESH_NULL_IDX)
                {
                    continue;       // shared with a neighbour, or a portal to the next tile
                }
                F32 a[3], b[3];
                vert(poly[j], a);
                vert(poly[(j + 1) % n], b);
                const F32 dx = b[0] - a[0], dz = b[2] - a[2];
                const F32 len = std::sqrt(dx * dx + dz * dz);
                if (len < hf.cs * 0.5f)
                {
                    continue;
                }
                // Left-hand normal of a->b, flipped by the winding, points out.
                const F32 nx = -dz / len * outward, nz = dx / len * outward;
                const S32 steps = std::max(1, static_cast<S32>(len / hf.cs));

                WallSegment current;
                bool open = false;
                for (S32 k = 0; k <= steps; ++k)
                {
                    const F32 t = std::min(1.f, k / static_cast<F32>(steps));
                    const F32 px = a[0] + dx * t, pz = a[2] + dz * t, py = a[1] + (b[1] - a[1]) * t;
                    Sample s;
                    F32 ox = px, oz = pz;
                    for (const F32 reach : probes)
                    {
                        ox = px + nx * reach;
                        oz = pz + nz * reach;
                        const S32 cx = static_cast<S32>(std::floor((ox - hf.bmin[0]) / hf.cs));
                        const S32 cz = static_cast<S32>(std::floor((oz - hf.bmin[2]) / hf.cs));
                        s = classify(hf, cx, cz, py, params);
                        if (s.mKind != EdgeKind::OPEN)
                        {
                            break;
                        }
                    }
                    if (s.mKind == EdgeKind::LEDGE && s.mLandingOk)
                    {
                        s.mLandingOk = s.mValue <= drop_max &&
                                       (!walkable_landing || landingWalkable(chf, ox, oz, nx, nz, s.mLanding, landing_steps));
                    }
                    const F32 rc[3] = { px, py, pz };
                    const LLVector3 point = fromRecast(rc);

                    const bool same = open && s.mKind == current.mKind && std::fabs(s.mValue - current.mValue) < 0.25f;
                    if (!same)
                    {
                        if (open && current.mKind != EdgeKind::OPEN && current.mA != current.mB)
                        {
                            segments.push_back(current);
                        }
                        current.mA = point;
                        current.mKind = s.mKind;
                        current.mValue = s.mValue;
                        current.mArea = 0;
                        open = true;
                    }
                    current.mB = point;

                    sinceLink += len / steps;
                    if (s.mKind == EdgeKind::LEDGE && s.mLandingOk && sinceLink >= link_spacing)
                    {
                        sinceLink = 0.f;
                        DropLink link;
                        const F32 from[3] = { px - nx * hf.cs, py, pz - nz * hf.cs };
                        const F32 to[3] = { ox, s.mLanding, oz };
                        link.mFrom = fromRecast(from);
                        link.mTo = fromRecast(to);
                        link.mHeight = s.mValue;
                        links.push_back(link);
                    }
                }
                if (open && current.mKind != EdgeKind::OPEN && current.mA != current.mB)
                {
                    segments.push_back(current);
                }
            }
        }
    }
}
