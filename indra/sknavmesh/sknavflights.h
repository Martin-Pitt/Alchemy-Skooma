/**
 * @file sknavflights.h
 * @brief The slope partition by smoothing groups, and the stair flights it finds.
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

#ifndef SK_NAVFLIGHTS_H
#define SK_NAVFLIGHTS_H

#include "sknavair.h"
#include "v2math.h"
#include "v3math.h"

#include <vector>

namespace sknav
{
    constexpr F32 SLOPE_LEVEL_DEG = 8.f;    ///< below this a surface is a floor, not a climb
    constexpr F32 SLOPE_TURN_DEG = 30.f;    ///< the crease angle: neighbours further apart are different slopes
    constexpr F32 SLOPE_REACH_M = 0.75f;    ///< a level group shallower than this between two risers is a tread
    constexpr F32 FLIGHT_MIN_RISE_M = 0.5f; ///< a chain climbing less than this is level ground the agent's step covers

    /// One tread of a flight, mesh-local SL metres.
    struct FlightTread
    {
        F32 mZ = 0.f;           ///< the tread's height, area weighted
        LLVector3 mNosingA;     ///< the edge it drops from, toward the tread below
        LLVector3 mNosingB;
        F32 mRise = 0.f;        ///< down to the tread or landing below
        F32 mGoing = 0.f;       ///< between its two risers
    };

    /// A run of treads between landings: the map's stair product.
    struct Flight
    {
        std::vector<FlightTread> mTreads;   ///< bottom to top
        LLVector3 mTopNosingA;              ///< where the top tread meets the riser above it
        LLVector3 mTopNosingB;
        F32 mRise = 0.f;                    ///< mean rise per step, the top one included
        F32 mPlane[3] = { 0.f, 0.f, 0.f };  ///< z = [0] x + [1] y + [2], fitted to the nosings
        std::vector<LLVector2> mFootprint;  ///< convex hull of the treads in plan, counter-clockwise
        F32 mZMin = 0.f;
        F32 mZMax = 0.f;
        U64 mKey = 0;                       ///< from its bottom and top nosings: the same in every tile it crosses
        U8 mCode = 0;                       ///< the area code its treads rasterise as
        std::vector<U16> mPolys;            ///< the tile's polys the flight produced
    };

    /// Per triangle, the area code it rasterises as, so that Recast cuts
    /// between regions; codes are ordered by the area they stand for, so a
    /// span merge that keeps the higher code keeps the higher area as well.
    struct SlopePartition
    {
        std::vector<U8> mCodes;     ///< per input triangle; 0 where it is not walkable
        std::vector<U8> mReal;      ///< code -> area; code 0 is null
        std::vector<S32> mRegion;   ///< per input triangle, -1 where it is not walkable
        std::vector<Flight> mFlights;
        S32 mSlopes = 0;            ///< groups by kind
        S32 mLandings = 0;
        S32 mTreads = 0;
        S32 mFlat = 0;              ///< chains dropped as staircases: they rise less than FLIGHT_MIN_RISE_M
    };

    /// Partition a class soup (mesh-local SL metres, three indices per
    /// triangle). areas holds each triangle's area, 0 where it is not
    /// walkable; only triangles of the partitioned area are split into
    /// regions, the rest keep one code per area. A two-sided triangle is
    /// walkable whichever way it faces. Codes run from 1 to below max_codes;
    /// every area keeps one of its own however many colours want one.
    SlopePartition partitionSlopes(const std::vector<LLVector3>& verts, const std::vector<S32>& tris, const std::vector<U8>& areas,
                                   const std::vector<U8>& two_sided, U8 partitioned_area, F32 climb, S32 max_codes = 63);

    /// Give each flight the polys that its code produced over its footprint.
    /// centroids and codes are per poly, in the poly mesh's order; flights
    /// that got none are dropped.
    void mapFlightPolys(std::vector<Flight>& flights, const std::vector<LLVector3>& centroids, const std::vector<U8>& codes, F32 climb);

    /// Mark FLIGHT on the walkable gaps a flight's treads stand for: inside
    /// its footprint, with the floor on its plane. The grid and the flights
    /// share a frame.
    void markFlights(AirGrid& grid, const std::vector<Flight>& flights);
}

#endif // SK_NAVFLIGHTS_H
