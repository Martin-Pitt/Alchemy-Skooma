/**
 * @file sknavflightends.h
 * @brief Flight ends: the floor polys each end of a stair or ramp meets, and why an end meets none.
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


#ifndef SK_NAVFLIGHTENDS_H
#define SK_NAVFLIGHTENDS_H

#include "sknavflights.h"

#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>

class dtNavMesh;
struct dtMeshTile;

namespace sknav
{
    constexpr F32 FLIGHT_END_TOUCH_M = 0.35f;   ///< a poly this close in plan to an end edge meets it: the erosion radius and a little
    constexpr F32 FLIGHT_END_DZ_M = 0.5f;       ///< and this close to the end's height where it comes closest
    constexpr F32 FLIGHT_END_REACH_M = 1.f;     ///< a poly linked to the flight's own meets an end when their shared edge is this close to it
    constexpr F32 FLIGHT_END_PAST_M = 1.25f;    ///< with none touching, a poly out past the end this close in plan meets it, across the erosion gap

    /// Why an end meets no poly.
    enum class EndGap : U8 { NONE, ERODED, SHORT, TILE_EDGE, NO_FLOOR };

    /// The polys a flight's ends meet, by its key.
    struct FlightEnds
    {
        U64 mFlight = 0;
        std::vector<U64> mPolys[2];     ///< Detour refs of the ground polys outside it at its bottom and its top, ascending
        S32 mStorey[2] = { -1, -1 };    ///< the storey most of them stand in, -1 for none
        EndGap mGap[2] = { EndGap::NONE, EndGap::NONE };
    };

    /// The Flight a tile's poly belongs to, or null.
    typedef std::function<const Flight*(const dtMeshTile* tile, U32 poly)> PolyFlightFn;

    /// Plan boxes bucketed on a square grid, so a search near a point reads only the boxes there.
    class PlanBoxIndex
    {
    public:
        explicit PlanBoxIndex(F32 cell = 4.f) : mCell(cell) {}
        void add(U32 id, F32 x0, F32 y0, F32 x1, F32 y1);
        /// The ids of every box meeting the query box, and maybe a few near it: ascending, each once.
        void query(F32 x0, F32 y0, F32 x1, F32 y1, std::vector<U32>& out) const;

    private:
        U64 cellKey(S32 cx, S32 cy) const { return (static_cast<U64>(static_cast<U32>(cx)) << 32) | static_cast<U32>(cy); }
        S32 cellOf(F32 v) const { return static_cast<S32>(std::floor(v / mCell)); }

        F32 mCell;
        std::unordered_map<U64, std::vector<U32>> mCells;
    };

    /// Whether this copy of a flight is its home tile's: the one its footprint's middle lies in, or a flight with no polys kept there.
    bool homeCopy(const dtMeshTile* tile, const Flight& f);

    /// Each flight's end polys: ground polys that are not its own and meet an
    /// end, either linked to its own polys across an edge at that end's height
    /// or touching the end edge in plan at that height, else lying out past
    /// the end within FLIGHT_END_PAST_M at that height. Flights share a key
    /// across tiles and count once; flight_of names each poly's flight.
    /// An end with none says why: the flight has no polys (ERODED), its polys
    /// stop short of that end (SHORT), the end lies at the edge of the loaded
    /// tiles (TILE_EDGE), or there is no floor there (NO_FLOOR). Mesh-local.
    void flightEnds(const dtNavMesh& nav, const std::vector<const Flight*>& flights, const PolyFlightFn& flight_of, std::vector<FlightEnds>& out);

    const char* endGapName(EndGap g);
}

#endif // SK_NAVFLIGHTENDS_H
