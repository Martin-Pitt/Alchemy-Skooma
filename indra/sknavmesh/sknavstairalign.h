/**
 * @file sknavstairalign.h
 * @brief Stair portals squared to the flights they enter.
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

#ifndef SK_NAVSTAIRALIGN_H
#define SK_NAVSTAIRALIGN_H

#include "v2math.h"

#include <functional>
#include <vector>

namespace sknav
{
    constexpr F32 STAIR_FIT_REACH_M = 1.5f;     ///< an end's rise is fitted over the flight this far from it
    constexpr F32 STAIR_FIT_FAR_M = 3.f;        ///< or this far, where a footprint's apron leaves the first stretch level
    constexpr F32 STAIR_FIT_STEP_M = 0.125f;    ///< the fit's sample spacing, an air cell
    constexpr F32 STAIR_SNAP_DEG = 20.f;        ///< ends this close to a right-angle multiple of their flight's first end snap to it
    constexpr F32 STAIR_CURVE_DEG = 25.f;       ///< a rise turning this much, a little every step while it climbs, is a curved flight
    constexpr S32 STAIR_CURVE_STEPS = 3;        ///< over at least this many steps
    constexpr F32 STAIR_TREAD_DZ_M = 0.35f;     ///< along a tread the floor stays within this

    /// The floor of a flight under a point, near height z; false where no flight's floor is there.
    typedef std::function<bool(F32 x, F32 y, F32 z, F32& floor)> FlightFloor;

    /// The uphill direction of a flight by a plane fit through its floor,
    /// flooded from c at height z over a patch reaching out along dir and to
    /// either side, or round c when dir is zero; false where too little of
    /// it is flight or it is level.
    bool flightRise(const FlightFloor& floor, const LLVector2& c, F32 z, const LLVector2& dir, F32 reach, LLVector2& rise);

    /// Whether a flight's rise turns continuously walking from c along it,
    /// up or down: curved and spiral flights do, straight runs and turns at
    /// a landing do not.
    bool flightCurves(const FlightFloor& floor, const LLVector2& c, F32 z, const LLVector2& rise, bool up);

    /// Where a flight meets a floor, as the contact between them was cut.
    struct StairEnd
    {
        LLVector2 mA;
        LLVector2 mB;
        LLVector2 mPivot;       ///< the cut's middle, which the squared end turns about
        LLVector2 mSeed;        ///< a flight cell beside the cut, nearest its middle
        F32 mZ = 0.f;           ///< the flight's floor there
        LLVector2 mInto;        ///< horizontal, into the flight
        U32 mFlight = 0;        ///< ends of one flight are squared together
        // Results.
        LLVector2 mRise;        ///< uphill, zero where none was found
        bool mFree = false;     ///< a curved or spiral flight's end, which keeps its cut
        bool mAligned = false;  ///< mA and mB were replaced
    };

    /// Square each end to its flight: along the treads, perpendicular to the
    /// rise, across the flight's width at that end. A flight's ends take its
    /// first end's direction turned by the nearest right angle where they lie
    /// within STAIR_SNAP_DEG of one, so a straight run's ends are parallel and
    /// a quarter turn's exactly square; curved flights keep their cuts.
    void alignStairEnds(const FlightFloor& floor, std::vector<StairEnd>& ends);
}

#endif // SK_NAVSTAIRALIGN_H
