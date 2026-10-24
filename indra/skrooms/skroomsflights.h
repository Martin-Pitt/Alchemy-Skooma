/**
 * @file skroomsflights.h
 * @brief Stairs, ramps and climbs found on the spaces' floor cells: the rooms layer's flights, read off the air.
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

#ifndef SK_ROOMSFLIGHTS_H
#define SK_ROOMSFLIGHTS_H

#include "stdtypes.h"
#include "v2math.h"
#include "v3math.h"

#include <iosfwd>
#include <istream>
#include <vector>

namespace skworld
{
    enum class FlightKind : U8 { STAIR = 0, RAMP = 1, CLIMB = 2 };
    /// What told a flight's kind: its name, its grade, or a narrow decked wedge a storey high.
    enum class FlightRule : U8 { NAME = 0, GRADE = 1, NARROW = 2 };

    constexpr S32 FLIGHT_FIT_HOPS = 4;             ///< a cell's grade is the rise between the cells this many links out along x and along y
    constexpr S32 FLIGHT_COMPLETE_DZ_UNITS = 1;    ///< sloped floor linked to a region this close to its plane along the run joins it
    constexpr F64 FLIGHT_MIN_GRADE_DEG = 5.;       ///< gentler is a floor
    constexpr F64 FLIGHT_MAX_GRADE_DEG = 80.;      ///< steeper fits come from a riser seen edge on, not a slope
    constexpr F64 FLIGHT_JOIN_TURN_DEG = 30.;      ///< neighbours rising in directions further apart are two flights
    constexpr F64 FLIGHT_JOIN_GRADE_DEG = 20.;     ///< and so are neighbours whose grades differ more
    constexpr F64 FLIGHT_END_BAND_M = 0.25;        ///< an end's height is read over this much of the run
    constexpr F64 FLIGHT_END_REACH_M = 0.375;      ///< a deck is looked for from cells this close to an end
    constexpr F64 FLIGHT_FLOOR_DZ_M = 0.75;        ///< a deck lies within this of its end's height
    constexpr F64 FLIGHT_FLOOR_MIN_M2 = 4.;        ///< without a name, a smaller floor at an end is a prop top
    constexpr F64 FLIGHT_MIN_RISE_M = 0.5;         ///< a flight climbs at least this
    constexpr F64 FLIGHT_MIN_RISE_UNCUED_M = 1.;   ///< and without a name, at least this: less is a step
    constexpr F64 FLIGHT_MIN_RUN_M = 0.5;
    constexpr F64 FLIGHT_MAX_RUN_M = 12.;
    constexpr F64 FLIGHT_MIN_WIDTH_M = 0.8;        ///< narrower is a rail, a plank or a wall top; a ship's stair is 0.88 m
    constexpr F64 FLIGHT_MIN_AREA_M2 = 1.5;        ///< without a name, smaller is a prop
    constexpr F64 FLIGHT_MAX_AREA_UNCUED_M2 = 100.;    ///< and bigger is a roof or a hillside
    constexpr F64 FLIGHT_MAX_GOING_M = 0.6;        ///< treads stepping two units or more apart, deeper on average than this and a cell, are seating
    constexpr S32 FLIGHT_OPEN_CEIL = 0x1fffffff;     ///< a cell's ceiling at or over this is open sky, as the spaces' OPEN_TOP
    constexpr U32 FLIGHT_MIN_REGION_CELLS = 8;     ///< a grown region of fewer cells is not measured
    constexpr F64 FLIGHT_CLIMB_GRADE_DEG = 52.;    ///< steeper is a climb, unless a narrow decked wedge a storey high
    constexpr F64 FLIGHT_STAIR_GRADE_DEG = 22.;    ///< without a name, steeper is a stair and gentler a ramp
    constexpr F64 FLIGHT_NARROW_WIDTH_M = 1.5;     ///< a wedge this narrow, decked at both ends ...
    constexpr F64 FLIGHT_NARROW_RISE_MIN_M = 1.2;  ///< ... and rising a storey, is a stair however steep
    constexpr F64 FLIGHT_NARROW_RISE_MAX_M = 2.5;
    constexpr F64 FLIGHT_CLIMB_FACE_M2 = 7.;       ///< a climb's face is at least this big: one facet of a rock is not one
    constexpr F64 FLIGHT_REACH_M = 16.;            ///< past a flight's longest run and its deck search: a pass sees whole every flight this close to its tiles

    /// One floor cell as the detection reads it: the spaces' own cells, mesh-local.
    struct FlightCell
    {
        F32 mX = 0.f;               ///< the cell's centre
        F32 mY = 0.f;
        S32 mFloor = 0;             ///< mesh height units
        S32 mCeil = 0;
        U8 mFlags = 0;              ///< the gap's AirGap flags
    };

    /// The floor cells and their links: per cell, the cell across each of its four sides that it walks onto, or ~0u.
    struct FlightGraph
    {
        F32 mCell = 0.125f;         ///< metres a cell
        F32 mUnit = 0.125f;         ///< metres a height unit; mesh z = units * mUnit
        std::vector<FlightCell> mCells;
        std::vector<U32> mLink;     ///< 4 per cell: +x, -x, +y, -y
    };

    /// A stair, ramp or climb between two floors. Positions are mesh-local metres.
    struct AirFlight
    {
        U32 mId = 0;                ///< from its key, never 0: the same flight has it in every pass
        U64 mKey = 0;               ///< its ends to the quarter metre
        FlightKind mKind = FlightKind::STAIR;
        FlightRule mRule = FlightRule::GRADE;
        LLVector3 mBottom;          ///< the middle of where it steps off onto its lower floor
        LLVector3 mTop;             ///< and onto its upper one
        LLVector3 mBottomA;         ///< the bottom's extent across the run
        LLVector3 mBottomB;
        LLVector3 mTopA;
        LLVector3 mTopB;
        F32 mRise = 0.f;            ///< floor to floor
        F32 mRun = 0.f;             ///< bottom to top in plan
        F32 mWidth = 0.f;           ///< across the run, the median of its slices
        F32 mArea = 0.f;            ///< m² in plan
        F32 mHeadroomMin = 0.f;     ///< the least air over any of its cells under a ceiling; -1 open to the sky over all
        F32 mAvatarShare = 0.f;     ///< of its cells, those an avatar stands on before erosion
        F32 mPlane[3] = { 0.f, 0.f, 0.f };  ///< z = [0] x + [1] y + [2] along the run through both ends
        F32 mZMin = 0.f;
        F32 mZMax = 0.f;
        std::vector<LLVector2> mFootprint;  ///< convex hull of its cells in plan, counter-clockwise
        bool mDecked = false;       ///< a floor of its own at both ends
        U8 mCue = 0;                ///< the NameCue most of its cells carry
        U32 mCells = 0;
        std::vector<U32> mSpaces;   ///< ids of the spaces its stair portals join, ascending; set by the spaces pass
    };

    /// What one detection did, for the profile and the stats line.
    struct FlightStats
    {
        U32 mCells = 0;             ///< floor cells read
        U32 mFitted = 0;            ///< of those, object floor whose link walks rise, fitted
        U32 mCandidates = 0;        ///< of those, sloped
        U32 mRegions = 0;           ///< grown regions
        U32 mFlights = 0;           ///< kept
        F32 mFitMs = 0.f;
        F32 mGrowMs = 0.f;
        F32 mRegionsMs = 0.f;
    };

    /// One measured region and what became of it, for the sweep against the oracle.
    struct FlightRegionTrace
    {
        U32 mCells = 0;
        F64 mUx = 0.0, mUy = 0.0, mS0 = 0.0, mS1 = 0.0;
        F64 mRise = 0.0, mRun = 0.0, mGrade = 0.0, mWidth = 0.0, mArea = 0.0, mGoing = 0.0;
        bool mDeckBottom = false, mDeckTop = false;
        F64 mFloorBottom = 0.0, mFloorTop = 0.0;
        U8 mCue = 0;
        const char* mVerdict = "";  ///< its kind, or why it was refused
    };

    /// Find the flights on a graph of floor cells: a grade per cell from the rise along its links, object floor
    /// at a grade grown into regions rising one way, each region measured along its run, its decks flooded at both
    /// ends, then judged. cell_flight gets, per cell, its flight's index into out plus one, or 0. Pure; any thread.
    void detectFlights(const FlightGraph& graph, std::vector<AirFlight>& out, std::vector<U16>& cell_flight, FlightStats* stats = nullptr,
                       std::vector<FlightRegionTrace>* trace = nullptr);

    /// The graph back from writeFlightGraph's form; false when it is not one.
    bool readFlightGraph(std::istream& in, FlightGraph& graph);

    const char* flightKindName(FlightKind kind);
    const char* flightRuleName(FlightRule rule);

    /// The graph in the oracle's binary form ("SKFG", version 1: cell, unit, count, then per cell x, y, floor, ceil,
    /// flags and four links), and the flights as one JSON object per line, for the sweep against it.
    void writeFlightGraph(std::ostream& out, const FlightGraph& graph);
    void writeFlightsJson(std::ostream& out, const std::vector<AirFlight>& flights);
}

#endif // SK_ROOMSFLIGHTS_H
