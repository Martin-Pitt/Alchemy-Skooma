/**
 * @file sknavspaceproducts.h
 * @brief What the navmesh makes of its spaces: walls with a space on each side, storeys and their connectors, the sheet's
 *        cells by space, and a space's floor from its polys.
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
        LLVector3 mEnd[2];          ///< where it meets each storey: the flight's own ends, else its stair portals, else where its polys meet
        U64 mFlight = 0;            ///< the Flight's key when the ends are the flight's
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

    /// The Flight a tile's poly belongs to, or null.
    typedef std::function<const Flight*(const dtMeshTile* tile, U32 poly)> PolyFlightFn;

    /// Storeys over a mesh's Detour polys: a flood across Detour's links that
    /// never steps onto a flight (a FLIGHT poly, or a poly of a flight space).
    /// Each flight's connected polys join the storeys they touch: the Flight
    /// most of them belong to joins the storeys nearest its two ends, at its
    /// ends; without one, each storey to the next one up, at the flight
    /// space's stair portals where it has them. Floors under min_area m² that no
    /// flight reaches make no storey. Positions mesh-local.
    void buildStoreys(const dtNavMesh& nav, const PolySpaceFn& space_of, const RoomsProduct& rooms, StoreyProduct& out,
                      const PolyFlightFn& flight_of = nullptr, F32 min_area = 10.f);

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

    /// A space's floor raster in Space::mRaster's form, from polys instead of
    /// air: each poly's 0.125 m cells, at its height, counted into 0.5 m by
    /// 0.5 m by 1 m cells. polys holds each poly's outline, mesh-local.
    std::vector<std::pair<U64, U32>> polyRaster(const std::vector<std::vector<LLVector3>>& polys);

    /// The outlines of a space's polys in a Detour mesh, mesh-local.
    std::vector<std::vector<LLVector3>> spacePolys(const dtNavMesh& nav, const PolySpaceFn& space_of, U32 space);
}

#endif // SK_NAVSPACEPRODUCTS_H
