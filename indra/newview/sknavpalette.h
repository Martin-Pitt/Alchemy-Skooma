/**
 * @file sknavpalette.h
 * @brief One colour per thing the navmesh overlay draws, named so a legend cannot drift from it.
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

#ifndef SK_NAVPALETTE_H
#define SK_NAVPALETTE_H

#include "v4color.h"

/// Everything the navmesh and census overlays draw in a colour of its own. The
/// console's legend is built from this list, so a colour that changes here
/// changes there and nowhere else has to be told.
enum class SKNavSwatch : S32
{
    GROUND,         ///< walkable terrain
    OBJECT,         ///< walkable object surface
    SUBMERGED,      ///< walkable, but under water
    OTHER,          ///< walkable, with an area class the palette does not know
    TILE_JOINED,    ///< tile-boundary edge Detour linked to its neighbour
    TILE_OPEN,      ///< tile-boundary edge with nothing joined to it
    COVER_FULL,     ///< wall taller than the reader's eye
    COVER_HALF,     ///< wall that can be shot over
    COVER_LOW,      ///< wall too low to take cover behind
    WALL_FACE,      ///< the vertical surface itself, what Atmo paints and drains
    LEDGE,          ///< floor edge with a landable drop beyond it
    OVERHANG,       ///< floor edge with nothing below within reach
    DROP,           ///< where a ledge lands
    SHAPE_EXACT,    ///< census record built from the object's own triangles
    SHAPE_HULL,     ///< census record built from its convex hull
    SHAPE_BOX,      ///< census record standing in as its bounding box
    SHAPE_RENDER,   ///< census record taken from the rendered shape
    SHAPE_DYNAMIC,  ///< census record for something still moving
    CLUSTER,        ///< stands in for the per-structure colours, which are hashed
    COUNT
};

/// The swatch at full strength. Overlay passes scale the alpha; the x-ray pass
/// darkens the colour as well.
const LLColor4& skNavSwatchColor(SKNavSwatch swatch);

/// The name the console's string table knows it by, e.g. "sk_nav_ground".
const std::string& skNavSwatchName(SKNavSwatch swatch);

#endif // SK_NAVPALETTE_H
