/**
 * @file skairwalls.h
 * @brief Walls off the solid raster: exposed faces, the ledges where floors end and the run-off down them.
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

#ifndef SK_AIRWALLS_H
#define SK_AIRWALLS_H

#include "skcensustypes.h"

#include <vector>

namespace skworld
{
    struct ColumnRaster;

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
        U32 mSpace[2] = { 0, 0 };   ///< space ids left and right of A to B, bound at publish (the rooms layer); not cached
    };

    /// An exposed side of solid: the surface Atmo paints and drains along.
    /// mSide is the outward direction: 0 +x, 1 -x, 2 +y, 3 -y in SL axes.
    ///
    /// mArea is the area of the span the face belongs to where the air stands on
    /// its top (TOP_WALK), else none: only up-facing triangles are given an
    /// area, so a genuine wall reports none, and telling an object's wall from
    /// a terrain cliff needs a second rasterisation this does not do. Do not
    /// gate behaviour on it.
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

    /// A stretch of a floor's edge over a drop, and what lies below it: the edge from mA to mB at the floor's
    /// height, the drop to the landing at each end, linear between. One stretch runs until the landing changes
    /// (another surface, or the drop leaves its line by more than a unit), the edge bends, or the drop starts or ends.
    struct RunoffSegment
    {
        LLVector3 mA;
        LLVector3 mB;
        F32 mDropA = 0.f;
        F32 mDropB = 0.f;
        U8 mSide = 0;           ///< the side the floor's edge faces at mA: 0 +x, 1 -x, 2 +y, 3 -y
    };

    /// Where a floor can be left for another: from the edge to the landing.
    struct DropLink
    {
        LLVector3 mFrom;
        LLVector3 mTo;
        F32 mHeight = 0.f;
        LinkKind mKind = LinkKind::DROP;
    };

    /// Cover as the map reads it, against the reader's own eye height.
    enum class Cover : U8 { NONE, HALF, FULL };
    Cover coverClass(F32 wall_height, F32 eye_height, F32 climb, F32 peek = 0.15f);

    /// Every exposed span side inside the tile proper (border columns left
    /// out), greedy-merged along the wall, mesh-local SL.
    /// `min_height` drops anything shorter, which on sloping ground is most
    /// of what a heightfield has: a smooth hillside rasterises into a
    /// staircase, and every riser of it is an exposed vertical face. A riser
    /// is the cell size times the tangent of the slope, so half a metre keeps
    /// everything steeper than about seventy-five degrees at a 0.125 m cell
    /// and throws the terracing away.
    void extractWallFaces(const ColumnRaster& raster, S32 border_cells, F32 min_height, std::vector<WallFace>& out);

    /// Where the air's floors end over a drop, before the rim rule took them (TOP_PRE_LEDGE): a side whose
    /// neighbour column has nothing within climb units of the top or in headroom units over it. Runs over one
    /// drop merge into LEDGE segments, valued by the drop to the highest solid below, < 0 with none. The edge
    /// is followed round as a polyline and laid as run-off where it has a landing no more than drop_max metres
    /// down (RunoffSegment); the border left out.
    void extractRunoff(const ColumnRaster& raster, S32 border_cells, S32 climb, S32 headroom, F32 drop_max, std::vector<WallSegment>& ledges,
                       std::vector<RunoffSegment>& runoff);
}

#endif // SK_AIRWALLS_H
