/**
 * @file sknavwalls.h
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

#ifndef SK_NAVWALLS_H
#define SK_NAVWALLS_H

#include "sknavtypes.h"

#include <vector>

struct rcHeightfield;
struct rcCompactHeightfield;
struct rcPolyMesh;

namespace sknav
{
    struct ClassParams;

    /// What lies beyond a floor edge. Positions are mesh-local SL metres.
    enum class EdgeKind : U8
    {
        OPEN,       ///< nothing solid to the ceiling and no drop: erosion or a tile border
        WALL,       ///< solid rises from the floor; mValue is its height above the floor
        LEDGE,      ///< the floor ends and air is below; mValue is the drop to the next solid, < 0 when there is none
        OVERHANG,   ///< a ceiling too low to stand under; mValue is the clearance
    };

    /// A run of floor boundary with one answer: the map's wall line.
    struct WallSegment
    {
        LLVector3 mA;           ///< at floor height
        LLVector3 mB;
        EdgeKind mKind = EdgeKind::OPEN;
        F32 mValue = 0.f;       ///< height, drop or clearance, metres
        U8 mArea = 0;           ///< the solid's area beyond the edge, when known
        U32 mSpace[2] = { 0, 0 };   ///< space ids left and right of A to B, bound at publish (sknavspaceproducts.h); not cached
    };

    /// An exposed side of solid: the surface Atmo paints and drains along.
    /// mSide is the outward direction: 0 +x, 1 -x, 2 +y, 3 -y in SL axes.
    ///
    /// mArea is the Recast area of the span the face belongs to, which is null
    /// for anything steep: only up-facing triangles are given an area, because
    /// that is how Recast decides walkability. So a genuine wall reports no
    /// area, and telling an object's wall from a terrain cliff needs a second
    /// rasterisation this does not do. Do not gate behaviour on it.
    struct WallFace
    {
        LLVector3 mMin;         ///< a thin axis-aligned box; the thin axis is the side's
        LLVector3 mMax;
        U8 mSide = 0;
        U8 mArea = 0;
        U32 mSpace[2] = { 0, 0 };   ///< space ids of the side it faces and behind it, bound at publish; not cached
    };

    /// How a link is travelled.
    enum class LinkKind : U8
    {
        DROP,       ///< off a ledge, down
        LADDER,     ///< climbed, either way
        ELEVATOR,   ///< ridden, either way
    };

    /// Where a floor can be left for another: from the edge to the landing.
    struct DropLink
    {
        LLVector3 mFrom;
        LLVector3 mTo;
        F32 mHeight = 0.f;
        LinkKind mKind = LinkKind::DROP;
    };

    /// Recast area and Detour flag for the off-mesh polygon of a drop link.
    constexpr U8 AREA_DROP = 4;

    /// Cover as the map reads it, against the reader's own eye height.
    enum class Cover : U8 { NONE, HALF, FULL };
    Cover coverClass(F32 wall_height, F32 eye_height, F32 climb, F32 peek = 0.15f);

    /// Every exposed span side inside the tile proper (border columns left
    /// out), greedy-merged along the wall. Recast space in, SL mesh-local out.
    /// `min_height` drops anything shorter, which on sloping ground is most
    /// of what a heightfield has: a smooth hillside rasterises into a
    /// staircase, and every riser of it is an exposed vertical face. A riser
    /// is the cell size times the tangent of the slope, so half a metre keeps
    /// everything steeper than about seventy-five degrees at a 0.125 m cell
    /// and throws the terracing away.
    /// A raster whose spans carry codes passes the code -> area table as area_lut.
    void extractWallFaces(const rcHeightfield& hf, S32 border_cells, F32 min_height, std::vector<WallFace>& out,
                          const U8* area_lut = nullptr);

    /// Where the walkable tops of a raster end over a drop: a side whose
    /// neighbour column has nothing within climb cells of the top or in
    /// headroom cells over it. Runs over one drop merge into LEDGE segments,
    /// valued by the drop to the highest solid below, < 0 with none; every
    /// spacing metres along a run a link runs off down to it, drop_max at
    /// most. Recast space in, SL mesh-local out; the border left out.
    void extractRunoff(const rcHeightfield& hf, S32 border_cells, S32 climb, S32 headroom, F32 drop_max, F32 spacing,
                       std::vector<WallSegment>& ledges, std::vector<DropLink>& links);

    /// What classifyEdges reads of a heightfield: each column's solid runs,
    /// and nothing of their areas. Recast space, as the heightfield's.
    struct EdgeField
    {
        S32 mWidth = 0;
        S32 mHeight = 0;
        F32 mBmin[3] = { 0.f, 0.f, 0.f };
        F32 mCs = 0.f;
        F32 mCh = 0.f;
        std::vector<U32> mStart;    ///< per column, where its runs begin; columns + 1
        std::vector<U16> mRuns;     ///< smin, smax pairs, lowest first

        size_t bytes() const;
    };
    void takeEdgeField(const rcHeightfield& hf, EdgeField& out);

    /// Walk every boundary edge of the polymesh, sample the column just
    /// outside, classify, and merge runs into segments. Ledge samples with a
    /// landing become drop links: the landing must be walkable in the class
    /// when walkable_landing, else any solid top will do (the weather class).
    void classifyEdges(const rcPolyMesh& pmesh, const EdgeField& field, const rcCompactHeightfield& chf, const ClassParams& params,
                       F32 drop_max, bool walkable_landing, F32 link_spacing,
                       std::vector<WallSegment>& segments, std::vector<DropLink>& links);
    void classifyEdges(const rcPolyMesh& pmesh, const rcHeightfield& hf, const rcCompactHeightfield& chf, const ClassParams& params,
                       F32 drop_max, bool walkable_landing, F32 link_spacing,
                       std::vector<WallSegment>& segments, std::vector<DropLink>& links);
}

#endif // SK_NAVWALLS_H
