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

#include <string>
#include <vector>

struct rcCompactHeightfield;

namespace sknav
{
    constexpr F32 SLOPE_LEVEL_DEG = 8.f;    ///< below this a surface is a floor, not a climb
    constexpr F32 SLOPE_TURN_DEG = 30.f;    ///< the crease angle: neighbours further apart are different slopes
    constexpr F32 SLOPE_REACH_M = 0.75f;    ///< a level group shallower than this between two risers is a tread
    constexpr F32 FLIGHT_MIN_RISE_M = 0.5f; ///< a connector climbs at least this
    constexpr F32 STAIR_GRADE_DEG = 22.f;   ///< without a name or treads, steeper is a stair: walkways stay under it, stairs simplified to a slope do not

    enum SlopeGroupKind : U8 { SLOPE_GROUP, LANDING_GROUP, TREAD_GROUP };

    enum class ConnectorKind : U8 { STAIR, RAMP };
    enum class ConnectorRule : U8 { NAME, TREADS, GRADE };     ///< what told stair from ramp
    enum NameCue : U8 { CUE_NONE = 0, CUE_STAIR = 1, CUE_RAMP = 2 };

    /// A stair or ramp between two floors, built from the partition's slope
    /// and tread groups where they step off onto a floor at each end.
    struct Flight
    {
        LLVector3 mBottom;                  ///< the middle of where it steps off onto the lower floor
        LLVector3 mTop;                     ///< and onto the upper one
        LLVector3 mBottomA;                 ///< the bottom's extent across the run, at the floor
        LLVector3 mBottomB;
        LLVector3 mTopA;
        LLVector3 mTopB;
        F32 mRise = 0.f;                    ///< floor to floor
        F32 mRun = 0.f;                     ///< bottom to top, in plan
        F32 mPlane[3] = { 0.f, 0.f, 0.f };  ///< z = [0] x + [1] y + [2], through both ends along the run
        std::vector<LLVector2> mFootprint;  ///< convex hull of its groups in plan, counter-clockwise
        F32 mZMin = 0.f;
        F32 mZMax = 0.f;
        U64 mKey = 0;                       ///< from its ends, to the quarter metre
        ConnectorKind mKind = ConnectorKind::STAIR;
        ConnectorRule mRule = ConnectorRule::GRADE;
        U8 mTreads = 0;                     ///< tread groups among its groups
        std::vector<U8> mCodes;             ///< the area codes its groups rasterise as
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
        std::vector<S32> mGroup;    ///< per input triangle, its smoothing group; -1 where it is not walkable
        std::vector<U8> mKind;      ///< per group: SLOPE_GROUP, LANDING_GROUP or TREAD_GROUP
        std::vector<Flight> mFlights;
        S32 mSlopes = 0;            ///< groups by kind
        S32 mLandings = 0;
        S32 mTreads = 0;
    };

    /// Partition a class soup (mesh-local SL metres, three indices per
    /// triangle). areas holds each triangle's area, 0 where it is not
    /// walkable; only triangles of the partitioned area are split into
    /// regions, the rest keep one code per area. A two-sided triangle is
    /// walkable whichever way it faces. Codes run from 1 to below max_codes;
    /// every area keeps one of its own however many colours want one.
    /// cues holds a NameCue per triangle from its object's name, or is empty.
    SlopePartition partitionSlopes(const std::vector<LLVector3>& verts, const std::vector<S32>& tris, const std::vector<U8>& areas,
                                   const std::vector<U8>& two_sided, U8 partitioned_area, F32 climb, S32 max_codes = 63,
                                   const std::vector<U8>& cues = {});

    /// The NameCue an object's name and description give: stair words, else ramp.
    U8 nameCue(const std::string& name, const std::string& description);

    /// Give each flight the polys that its codes produced over its footprint.
    /// centroids and codes are per poly, in the poly mesh's order; flights
    /// that got none are dropped.
    void mapFlightPolys(std::vector<Flight>& flights, const std::vector<LLVector3>& centroids, const std::vector<U8>& codes, F32 climb);

    /// Narrow each flight's ends to the walkable treads: the lateral extent of
    /// its polys, grown back by the erosion radius. Polys are outlines in the
    /// poly mesh's order; a flight whose polys cover less than half its run is
    /// left as it is, and so is a side whose polys stop at the tile's edge, lo to hi.
    void fitFlightEdges(std::vector<Flight>& flights, const std::vector<std::vector<LLVector3>>& polys, F32 radius, const LLVector2& lo,
                        const LLVector2& hi);

    /// Whether a flight still stands on the compact heightfield before its
    /// regions are built: a span of the tile proper carrying one of its codes
    /// inside its footprint, within climb of its heights. The air marks the
    /// flights that do, before any polys exist.
    bool flightSurvives(const rcCompactHeightfield& chf, S32 border, const Flight& flight, F32 climb);

    /// Mark FLIGHT on the walkable gaps a flight stands for: inside
    /// its footprint, with the floor on its plane. The grid and the flights
    /// share a frame.
    void markFlights(AirGrid& grid, const std::vector<Flight>& flights);
}

#endif // SK_NAVFLIGHTS_H
