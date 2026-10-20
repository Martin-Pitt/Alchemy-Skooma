/**
 * @file sknavpolys.h
 * @brief One Detour tile's polygons as plain data, for readers that must not hold Detour pointers.
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

#ifndef SK_NAVPOLYS_H
#define SK_NAVPOLYS_H

#include "stdtypes.h"
#include "v3math.h"

#include <memory>
#include <vector>

struct dtMeshTile;

namespace sknav
{
    /// One Detour tile's polygons as plain data, mesh-local SL metres.
    struct TilePolys
    {
        static constexpr S32 MAX_VERTS = 6;       ///< DT_VERTS_PER_POLYGON
        static constexpr S16 EDGE_NONE = -1;      ///< a boundary with nothing beyond
        static constexpr S16 EDGE_BORDER = -2;    ///< on the tile border; the consumer stitches it
        struct Poly
        {
            U8 mArea = 0;                         ///< AREA_GROUND / AREA_OBJECT / AREA_SUBMERGED, via sknav::polyArea(flags)
            U8 mVertCount = 0;
            U16 mVerts[MAX_VERTS] = {};
            S16 mNeighbour[MAX_VERTS] = {};       ///< per edge v -> v+1: internal index, EDGE_NONE or EDGE_BORDER
            U32 mFirstTri = 0;                    ///< into mTris, three corners per triangle
            U16 mTriCount = 0;
            F32 mZMin = 0.f;
            F32 mZMax = 0.f;
        };
        std::vector<LLVector3> mVerts;
        std::vector<Poly> mPolys;                 ///< Detour order, off-mesh connections excluded: index == Flight::mPolys entry
        std::vector<LLVector3> mTris;             ///< detail triangles
    };

    /// Copy a tile's ground polygons out. External links are left unresolved: they go stale when
    /// a neighbour republishes, and the copy must depend only on the tile's own content.
    std::shared_ptr<const TilePolys> extractTilePolys(const dtMeshTile& tile);
}

#endif // SK_NAVPOLYS_H
