/**
 * @file sknavroomgraph.h
 * @brief A mesh's spaces grouped into rooms across the doors and openings of one storey.
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

#ifndef SK_NAVROOMGRAPH_H
#define SK_NAVROOMGRAPH_H

#include "sknavspaces.h"

#include "v3math.h"

#include <memory>
#include <utility>
#include <vector>

namespace sknav
{
    struct RoomGraphParams
    {
        F32 mSillSlackM = 0.6f;         ///< a joining portal's sill lies within both spaces' floor bands, widened by this
        U32 mMaxMaskCells = 1u << 20;   ///< a footprint box holding more cells than this keeps no mask
    };

    /// One storey of a building as the spaces pass cut it: the indoor and
    /// sheltered spaces of a building joined across its doors and openings.
    /// Doors cross, closed or not, as a storey runs on behind them; stairs and
    /// the outdoors never do, so a flight, a roof and the street are in no
    /// room. A ramped space spans its own band, and a building straddling two
    /// meshes is one room in each. Positions are mesh-local.
    struct Room
    {
        U32 mId = 0;                ///< its least member space id: stable while that member stands
        U64 mHash = 0;              ///< its members' ids and rasters and its band: moves only where its air moved
        std::vector<U32> mSpaces;   ///< member space ids, ascending
        std::vector<U32> mPortals;  ///< indices of the portals with exactly one end in it, of any kind
        F32 mFloorMin = 0.f;        ///< its storey band, over its members' floors
        F32 mFloorMax = 0.f;
        LLVector3 mMin;             ///< the union of its members' boxes
        LLVector3 mMax;
        S32 mCellX0 = 0;            ///< its footprint's box, in cells of RASTER_XY_M from the mesh origin
        S32 mCellY0 = 0;
        S32 mCellW = 0;
        S32 mCellH = 0;
        std::vector<U8> mMask;      ///< row-major mCellW x mCellH, 1 where a member's raster has a cell; empty when truncated
        bool mTruncated = false;    ///< the box held more than mMaxMaskCells cells, so only the box is kept

        /// Whether the footprint holds a cell, by mesh-local cell index; true anywhere in the box when truncated.
        bool covers(S32 cell_x, S32 cell_y) const;
    };

    /// A mesh's rooms, immutable once built.
    struct RoomGraph
    {
        U64 mGeneration = 0;                            ///< the rooms product's
        std::vector<Room> mRooms;                       ///< by id, ascending
        std::vector<U32> mRoomOfSpace;                  ///< per space of the product, its room's index or ~0u
        std::vector<std::pair<U32, U32>> mSpaceIndex;   ///< each space's id and index, by id

        /// A room by id, or null.
        const Room* byId(U32 room_id) const;
        /// The room holding the space at this index of the product, or null.
        const Room* ofSpace(U32 space_index) const;
        /// The room holding the space with this id, or null.
        const Room* roomOfId(U32 space_id) const;
        size_t bytes() const;
    };
    typedef std::shared_ptr<const RoomGraph> RoomGraphPtr;

    /// Whether a space may be a room's member: in a building, indoors or sheltered, and neither a roof, a flight nor frozen.
    bool roomMember(const Space& space);

    /// The rooms of a product's spaces, from its spaces, portals and rasters alone.
    RoomGraphPtr buildRoomGraph(const RoomsProduct& rooms, const RoomGraphParams& params = {});
}

#endif // SK_NAVROOMGRAPH_H
