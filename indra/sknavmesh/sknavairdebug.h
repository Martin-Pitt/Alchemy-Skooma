/**
 * @file sknavairdebug.h
 * @brief What the air overlay draws of a tile's air, as floors and edges in mesh-local metres.
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

#ifndef SK_NAVAIRDEBUG_H
#define SK_NAVAIRDEBUG_H

#include "stdtypes.h"
#include "v3math.h"
#include "v4color.h"

namespace sknav
{
    struct TileAir;

    /// What SKNavOverlayAir shows; the values are the setting's.
    enum class AirDebugMode : U32
    {
        OFF = 0,
        LEAVES = 1,     ///< leaf outlines on their floors, by level
        GAPS = 2,       ///< floors by flags, floor and ceiling outlines, walls and steps at leaf corners
        SPACE = 3,      ///< floors by the space their gap is in
        STAMP = 4,      ///< AVATAR polygons by the space they were stamped with; drawn by the viewer, not here
        CLEARANCE = 5,  ///< floors by chamfer clearance
        FACES = 6,      ///< true wall faces over the cell edges they correct
        INDOOR = 7,     ///< floors by how indoors their gap is; the colours are the viewer's
        COUNT = 8
    };

    /// Where the gathered geometry goes. Each call returns false once the
    /// sink's budget is spent, and the gather stops there.
    struct AirDebugSink
    {
        virtual ~AirDebugSink() = default;
        virtual bool quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d, const LLColor4& color) = 0;
        virtual bool line(const LLVector3& a, const LLVector3& b, const LLColor4& color) = 0;
    };

    struct AirDebugInput
    {
        AirDebugMode mMode = AirDebugMode::OFF;
        bool mCoarse = false;           ///< far air: drawn dimmer
        /// Gaps whose floor lies outside these metres, mesh-local z, are left out.
        F32 mSliceLo = -1e30f;
        F32 mSliceHi = 1e30f;
        /// Leaves whose centre lies further than this in plan from mCentre are left out; none when not positive.
        LLVector3 mCentre;
        F32 mReach = 0.f;
        S32 mClimb = 6;                 ///< height units within which a neighbour's floor or ceiling is the same
        F32 mOpenTick = 0.5f;           ///< metres an open gap's tick stands over its floor
        const U32* mGapSpace = nullptr; ///< per gap of the air, its space id, 0 for none; the space mode's colours
        const LLColor4* mGapColour = nullptr; ///< per gap of the air, the indoor mode's colour; grey for none
    };

    struct AirDebugStats
    {
        U32 mGaps = 0;          ///< gaps drawn
        U32 mFaces = 0;         ///< face offsets drawn
        bool mCut = false;      ///< the sink refused something
    };

    /// Gather one tile's air for a mode, mesh-local. False when the sink cut it short.
    bool gatherAirDebug(const TileAir& air, const AirDebugInput& in, AirDebugSink& sink, AirDebugStats* stats = nullptr);

    /// The highest floor at or below at, mesh-local, in metres, among the tile's gaps over its cell.
    bool airFloorUnder(const TileAir& air, const LLVector3& at, F32& floor);

    /// A leaf level's colour, 0 (one cell) to AIR_MAX_LEVEL.
    LLColor4 airLevelColour(S32 level);
    /// A gap's colour by its flags: flight, AVATAR floor, other floor, no floor.
    LLColor4 airFlagsColour(U8 flags);
    /// Clearance on a heat ramp: red at a wall to cyan at CLEAR_MAX.
    LLColor4 airClearColour(U8 clear);
    /// The pastel the spaces overlay gives a space id; grey for none.
    LLColor4 spaceColour(U32 id);
    /// A face offset, in FACE_OFFSET_STEPS, on a ramp from amber (small) to magenta (a cell).
    LLColor4 faceOffsetColour(U32 offset);
}

#endif // SK_NAVAIRDEBUG_H
