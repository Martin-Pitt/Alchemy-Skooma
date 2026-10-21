/**
 * @file sknavroomgraph.cpp
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

#include "linden_common.h"

#include "sknavroomgraph.h"

#include "sknavtypes.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr F32 BAND_QUANTUM_M = 0.125f;  ///< the band is hashed to this, the air's height unit

    S32 rasterIndex(F32 metres)
    {
        return static_cast<S32>(std::lround(metres / sknav::RASTER_XY_M));
    }

    U64 bandKey(F32 metres)
    {
        return static_cast<U64>(static_cast<S64>(std::lround(metres / BAND_QUANTUM_M)));
    }
}

namespace sknav
{
    bool Room::covers(S32 cell_x, S32 cell_y) const
    {
        const S32 x = cell_x - mCellX0, y = cell_y - mCellY0;
        if (x < 0 || y < 0 || x >= mCellW || y >= mCellH)
        {
            return false;
        }
        return mTruncated || mMask[static_cast<size_t>(x) + static_cast<size_t>(y) * mCellW] != 0;
    }

    const Room* RoomGraph::byId(U32 room_id) const
    {
        const auto it = std::lower_bound(mRooms.begin(), mRooms.end(), room_id, [](const Room& r, U32 id) { return r.mId < id; });
        return it != mRooms.end() && it->mId == room_id ? &*it : nullptr;
    }

    const Room* RoomGraph::ofSpace(U32 space_index) const
    {
        if (space_index >= mRoomOfSpace.size() || mRoomOfSpace[space_index] == ~0u)
        {
            return nullptr;
        }
        return &mRooms[mRoomOfSpace[space_index]];
    }

    const Room* RoomGraph::roomOfId(U32 space_id) const
    {
        const auto it = std::lower_bound(mSpaceIndex.begin(), mSpaceIndex.end(), space_id,
                                         [](const std::pair<U32, U32>& e, U32 id) { return e.first < id; });
        return it != mSpaceIndex.end() && it->first == space_id ? ofSpace(it->second) : nullptr;
    }

    size_t RoomGraph::bytes() const
    {
        size_t n = sizeof(RoomGraph) + mRooms.capacity() * sizeof(Room) + mRoomOfSpace.capacity() * sizeof(U32) +
                   mSpaceIndex.capacity() * sizeof(std::pair<U32, U32>);
        for (const Room& r : mRooms)
        {
            n += (r.mSpaces.capacity() + r.mPortals.capacity()) * sizeof(U32) + r.mMask.capacity();
        }
        return n;
    }

    bool roomMember(const Space& space)
    {
        return space.mBuilding != 0 && !space.mFrozen && !space.mRoof && !space.mFlight &&
               (space.mBucket == EnclosureBucket::INDOORS || space.mBucket == EnclosureBucket::SHELTERED);
    }

    RoomGraphPtr buildRoomGraph(const RoomsProduct& rooms, const RoomGraphParams& params)
    {
        auto graph = std::make_shared<RoomGraph>();
        graph->mGeneration = rooms.mGeneration;
        const std::vector<Space>& spaces = rooms.mSpaces;
        const U32 count = static_cast<U32>(spaces.size());
        graph->mSpaceIndex.reserve(count);
        for (U32 i = 0; i < count; ++i)
        {
            graph->mSpaceIndex.emplace_back(spaces[i].mId, i);
        }
        std::sort(graph->mSpaceIndex.begin(), graph->mSpaceIndex.end());

        // The walk: members joined across a door or opening whose sill stands on both of their storeys, ramps left alone.
        const auto onFloor = [&params](const Space& s, F32 sill) {
            return sill >= s.mFloorMin - params.mSillSlackM && sill <= s.mFloorMax + params.mSillSlackM;
        };
        const auto ramp = [&params](const Space& s) { return s.mFloorMax - s.mFloorMin > params.mRampBandM; };
        std::vector<U32> component(count, ~0u);
        std::vector<std::vector<U32>> members;
        std::vector<U32> stack;
        for (U32 seed = 0; seed < count; ++seed)
        {
            if (component[seed] != ~0u || !roomMember(spaces[seed]))
            {
                continue;
            }
            const U32 c = static_cast<U32>(members.size());
            members.emplace_back();
            component[seed] = c;
            stack.assign(1, seed);
            while (!stack.empty())
            {
                const U32 s = stack.back();
                stack.pop_back();
                members[c].push_back(s);
                if (ramp(spaces[s]))
                {
                    continue;
                }
                for (U32 pi : spaces[s].mPortals)
                {
                    if (pi >= rooms.mPortals.size())
                    {
                        continue;
                    }
                    const Portal& p = rooms.mPortals[pi];
                    if ((p.mKind != PortalKind::DOOR && p.mKind != PortalKind::OPENING) || p.mCrossMesh || p.mSpaces[0] >= count ||
                        p.mSpaces[1] >= count)
                    {
                        continue;
                    }
                    if (p.mSpaces[0] != s && p.mSpaces[1] != s)
                    {
                        continue;
                    }
                    const U32 n = p.mSpaces[0] == s ? p.mSpaces[1] : p.mSpaces[0];
                    if (component[n] != ~0u || !roomMember(spaces[n]) || ramp(spaces[n]) || !onFloor(spaces[s], p.mSill) ||
                        !onFloor(spaces[n], p.mSill))
                    {
                        continue;
                    }
                    component[n] = c;
                    stack.push_back(n);
                }
            }
        }

        graph->mRooms.resize(members.size());
        std::vector<S32> cells;
        for (size_t c = 0; c < members.size(); ++c)
        {
            Room& room = graph->mRooms[c];
            std::vector<U32>& member = members[c];
            std::sort(member.begin(), member.end(), [&spaces](U32 a, U32 b) { return spaces[a].mId < spaces[b].mId; });
            room.mId = spaces[member.front()].mId;
            room.mFloorMin = spaces[member.front()].mFloorMin;
            room.mFloorMax = spaces[member.front()].mFloorMax;
            room.mMin = spaces[member.front()].mMin;
            room.mMax = spaces[member.front()].mMax;
            cells.clear();
            for (U32 s : member)
            {
                const Space& sp = spaces[s];
                room.mSpaces.push_back(sp.mId);
                room.mFloorMin = llmin(room.mFloorMin, sp.mFloorMin);
                room.mFloorMax = llmax(room.mFloorMax, sp.mFloorMax);
                update_min_max(room.mMin, room.mMax, sp.mMin);
                update_min_max(room.mMin, room.mMax, sp.mMax);
                for (const auto& r : sp.mRaster)
                {
                    const PlanCell cell = rasterCell(r.first);
                    cells.push_back(rasterIndex(cell.mX));
                    cells.push_back(rasterIndex(cell.mY));
                }
            }

            // Canonical order makes the hash independent of how the walk met them.
            U64 hash = SEED;
            for (U32 s : member)
            {
                hash = mix(hash, spaces[s].mId);
                for (const auto& r : spaces[s].mRaster)
                {
                    hash = mix(mix(hash, r.first), r.second);
                }
            }
            room.mHash = mix(mix(hash, bandKey(room.mFloorMin)), bandKey(room.mFloorMax));

            // Portals with one end in the room: its ways out, of any kind.
            for (U32 s : member)
            {
                for (U32 pi : spaces[s].mPortals)
                {
                    if (pi >= rooms.mPortals.size())
                    {
                        continue;
                    }
                    const Portal& p = rooms.mPortals[pi];
                    if (p.mCrossMesh)
                    {
                        continue;
                    }
                    const bool in0 = p.mSpaces[0] < count && component[p.mSpaces[0]] == c;
                    const bool in1 = p.mSpaces[1] < count && component[p.mSpaces[1]] == c;
                    if (in0 != in1)
                    {
                        room.mPortals.push_back(pi);
                    }
                }
            }
            std::sort(room.mPortals.begin(), room.mPortals.end());
            room.mPortals.erase(std::unique(room.mPortals.begin(), room.mPortals.end()), room.mPortals.end());

            // The footprint: every member raster cell, whatever its band, as each member stands on the storey.
            if (cells.empty())
            {
                continue;
            }
            S32 x0 = cells[0], y0 = cells[1], x1 = x0, y1 = y0;
            for (size_t i = 0; i < cells.size(); i += 2)
            {
                x0 = llmin(x0, cells[i]);
                x1 = llmax(x1, cells[i]);
                y0 = llmin(y0, cells[i + 1]);
                y1 = llmax(y1, cells[i + 1]);
            }
            room.mCellX0 = x0;
            room.mCellY0 = y0;
            room.mCellW = x1 - x0 + 1;
            room.mCellH = y1 - y0 + 1;
            const U64 area = static_cast<U64>(room.mCellW) * static_cast<U64>(room.mCellH);
            if (area > params.mMaxMaskCells)
            {
                room.mTruncated = true;
                continue;
            }
            room.mMask.assign(static_cast<size_t>(area), 0);
            for (size_t i = 0; i < cells.size(); i += 2)
            {
                room.mMask[static_cast<size_t>(cells[i] - x0) + static_cast<size_t>(cells[i + 1] - y0) * room.mCellW] = 1;
            }
        }

        // Rooms by id; members of one product have distinct ids, so rooms do too.
        std::vector<U32> order(graph->mRooms.size());
        for (U32 i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&graph](U32 a, U32 b) { return graph->mRooms[a].mId < graph->mRooms[b].mId; });
        std::vector<Room> sorted;
        sorted.reserve(order.size());
        std::vector<U32> rank(order.size());
        for (U32 i = 0; i < order.size(); ++i)
        {
            rank[order[i]] = i;
            sorted.push_back(std::move(graph->mRooms[order[i]]));
        }
        graph->mRooms.swap(sorted);
        graph->mRoomOfSpace.assign(count, ~0u);
        for (U32 s = 0; s < count; ++s)
        {
            if (component[s] != ~0u)
            {
                graph->mRoomOfSpace[s] = rank[component[s]];
            }
        }
        return graph;
    }
}
