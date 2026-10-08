/**
 * @file sknavspaceproducts.h
 * @brief What the navmesh makes of its spaces: walls with a space on each side, and storeys and their connectors.
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

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class dtNavMesh;
struct dtMeshTile;

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

    /// A tile's poly's space id by its index in the tile; 0 for none.
    typedef std::function<U32(const dtMeshTile* tile, U32 poly)> PolySpaceFn;

    /// One level of a mesh: the polys reached from each other without climbing a flight.
    struct Storey
    {
        U32 mId = 0;                ///< its least space id, so it holds while its spaces do; 0 with none
        F32 mFloorZ = 0.f;          ///< area-weighted median of its polys' heights, mesh-local
        F32 mArea = 0.f;            ///< m² of its polys in plan
        LLVector3 mMin;
        LLVector3 mMax;
        std::vector<U32> mSpaces;   ///< ids of the spaces most of whose polys are in it, ascending
    };

    /// A flight between two storeys, by index into the product's storeys, lower first.
    struct StoreyConnector
    {
        U32 mStorey[2] = { 0, 0 };
        U32 mSpace = 0;             ///< the flight's space, 0 when its polys have none
        LLVector3 mEnd[2];          ///< where it meets each storey: the stair portal, else where its polys meet the storey's
        bool mPortals = false;      ///< both ends are stair portals
    };

    struct StoreyProduct
    {
        std::vector<Storey> mStoreys;               ///< lowest floor first
        std::vector<StoreyConnector> mConnectors;
        std::vector<std::pair<U32, U32>> mSpaceStorey;  ///< space id -> storey index, by id
        std::vector<std::pair<U64, U32>> mPolyStorey;   ///< Detour poly ref -> storey index, by ref; flights have none
        U32 mFlightPolys = 0;
        U32 mPolys = 0;

        /// The storey index of a space, or -1.
        S32 storeyOf(U32 space) const;
        /// The storey index of a Detour poly, or -1 for a flight or a poly it does not know.
        S32 storeyOfPoly(U64 ref) const;
    };
    typedef std::shared_ptr<const StoreyProduct> StoreyProductPtr;

    /// Storeys over a mesh's Detour polys: a flood across Detour's links that
    /// never steps onto a flight (a FLIGHT poly, or a poly of a flight space),
    /// then each flight's connected polys join the storeys they touch, at the
    /// flight space's stair portals where it has them. Positions mesh-local.
    void buildStoreys(const dtNavMesh& nav, const PolySpaceFn& space_of, const RoomsProduct& rooms, StoreyProduct& out);

}

#endif // SK_NAVSPACEPRODUCTS_H
