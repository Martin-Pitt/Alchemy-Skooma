/**
 * @file skroadland.h
 * @brief Land the road driver and mission read: parcels, Linden land, region edges, ban lines.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef SK_ROADLAND_H
#define SK_ROADLAND_H

#include "stdtypes.h"

#include <array>
#include <string>
#include <vector>

class LLParcel;
class LLVector3d;
class LLViewerRegion;

namespace skroad::land
{
    const char* ownershipName(S32 own);             // a parcel overlay ownership class, for the log

    // Each edge (0 south, 1 east, 2 north, 3 west) of a region, a 4 m overlay cell at a time: safe where Linden
    // land meets Linden land across it, the parcels' owners asked for as the map is made.
    constexpr S32 EDGE_CELLS = 64;
    enum class Road : S8 { Unknown, Linden, Other };
    struct EdgeCell
    {
        bool safe = false;
        S32 inside = -1, across = -1;               // overlay ownership; -1 not loaded, -2 no neighbour
        Road road_in = Road::Unknown, road_across = Road::Unknown;   // the parcels' owners: Linden road or not
    };
    struct EdgeMap
    {
        LLViewerRegion* region = nullptr;
        F64 made = -1e9;
        std::array<std::array<EdgeCell, EDGE_CELLS>, 4> cells;
        std::string counts;
    };
    const EdgeMap& edgeMap(LLViewerRegion* region, F64 now);

    bool lindenLandAt(F64 x, F64 y);                // the parcel overlay's public land
    void watchBanLines();                           // keeps the ban lines the simulator sends, per region
    bool banLineAt(F64 x, F64 y);
    bool privateLandAt(F64 x, F64 y);               // not Linden land, not the agent's nor its group's
    const char* refusedAt(F64 x, F64 y, F64 z);     // a ban line, or private land off the road; null where allowed

    // A parcel's box covers a global point (a metre's slack).
    bool parcelCovers(const LLParcel* p, LLViewerRegion* region, const LLVector3d& at);

    // A parcel by its lines: the region's 4 m overlay cells reached from a point without crossing a property line.
    struct LineCells
    {
        LLViewerRegion* region = nullptr;
        S32 n = 0;
        std::vector<U8> in;                         // n * n, row major (row = y)
        bool has(S32 r, S32 c) const { return r >= 0 && c >= 0 && r < n && c < n && in[r * n + c]; }
    };
    bool parcelCellsAt(const LLVector3d& at, LineCells& out);
    bool cellsCover(const LineCells& cells, const LLVector3d& at);
    // The point of the cells nearest `to` at least `margin` from their lines; false when there is none.
    bool insidePoint(const LineCells& cells, const LLVector3d& to, F64 margin, LLVector3d& out);

    // Each 4 m overlay cell of a region labelled with its parcel, cached per region.
    const std::vector<S32>& parcelLabels(LLViewerRegion* region);
    S32 parcelLabelAt(const LLVector3d& at, U64& handle);   // -1 where the overlay is not in
    void forgetParcelLabels();
}

#endif
