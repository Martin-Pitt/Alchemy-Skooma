/**
 * @file sknavspaceproducts.h
 * @brief What the navmesh makes of its spaces: walls with a space on each side.
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

#ifndef SK_NAVSPACEPRODUCTS_H
#define SK_NAVSPACEPRODUCTS_H

#include "sknavspaces.h"
#include "sknavwalls.h"

#include <vector>

namespace sknav
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

}

#endif // SK_NAVSPACEPRODUCTS_H
