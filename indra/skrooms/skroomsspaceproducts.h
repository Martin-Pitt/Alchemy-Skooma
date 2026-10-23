/**
 * @file skroomsspaceproducts.h
 * @brief What is read off the spaces: the space at a point and across a wall, walls with a space on each side, and the
 *        sheet's cells by space.
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

#ifndef SK_ROOMSSPACEPRODUCTS_H
#define SK_ROOMSSPACEPRODUCTS_H

#include "skroomsspaces.h"
#include "skairwalls.h"

#include <vector>

namespace skworld
{
    /// The id of the space a mesh-local point stands in: the gap holding it; 0 for none.
    U32 spaceIdAt(const EnclosureProduct& enc, const LLVector3& p);

    /// The first space found stepping from p along a horizontal unit direction, up to reach metres: what lies across a wall.
    U32 spaceAcross(const EnclosureProduct& enc, const LLVector3& p, const LLVector3& dir, F32 reach);

    constexpr F32 WALL_REACH_M = 1.5f;      ///< the thickest wall a side's space is looked for across
    constexpr F32 WALL_LOW_M = 0.25f;       ///< a segment's sides are read this high over its floor, then a metre higher over a low wall

    /// Each segment's spaces: mSpace[0] left of A to B in plan, mSpace[1] right of it.
    void bindWallSpaces(const EnclosureProduct& enc, std::vector<WallSegment>& segments);

    /// Each face's spaces at mid-face height: mSpace[0] the side it faces, mSpace[1] across the solid behind it.
    void bindWallSpaces(const EnclosureProduct& enc, std::vector<WallFace>& faces);

    /// Two spaces a climb joins, foot first: a link in the space graph with no geometry of its own.
    struct ClimbLink
    {
        U32 mSpace[2] = { 0, 0 };   ///< ids, the space at the climb's foot and at its top
        U64 mFlight = 0;            ///< the climb's flight key
        LLVector3 mFoot;            ///< the climb's own ends, mesh-local
        LLVector3 mTop;
    };

    /// A portal where two spaces' walkable floors share an edge and no portal joins them.
    struct SeamPortal
    {
        enum Kind : U8 { DOOR = 0, STAIR = 1, ADJACENT = 3 };
        U32 mSpace[2] = { 0, 0 };   ///< ids, the lesser first
        Kind mKind = ADJACENT;      ///< STAIR beside a flight, DOOR on a door's polys, else ADJACENT
        LLVector3 mA;               ///< the shared run's two farthest ends and its middle, mesh-local
        LLVector3 mB;
        LLVector3 mCentre;
        F32 mLength = 0.f;          ///< metres of shared edge
    };

    /// One cell of the sheet with the space of its gap: the highest gap whose
    /// floor lies in the band, sampled at the cell's centre.
    struct SheetCell
    {
        F32 mFloor = 0.f;
        U32 mSpace = 0;             ///< id, 0 for a gap in no space
        bool mHit = false;          ///< a gap's floor lies in the band here
    };

    /// The sheet over a mesh-local plan box, res metres a cell, row-major along y.
    void sheetSpaces(const EnclosureProduct& enc, const LLVector3& min, const LLVector3& max, F32 res, std::vector<SheetCell>& out);
}

#endif // SK_ROOMSSPACEPRODUCTS_H
