/**
 * @file skroomsairdebug_test.cpp
 * @brief Tests for what the air overlay gathers of a tile's air.
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

#include <catch2/catch_test_macros.hpp>


#include "skairdebug.h"
#include "skairdistil.h"
#include "../skroomsenclosure.h"
#include "../skroomsspaces.h"

#include <cmath>
#include <vector>

using namespace skworld;

namespace
{
    struct Counter : AirDebugSink
    {
        U32 mQuads = 0;
        U32 mLines = 0;
        U32 mVerticals = 0;
        U32 mTiltedQuads = 0;
        U32 mBudget = 0xffffffff;   ///< primitives accepted before refusing
        std::vector<std::pair<LLVector3, LLVector3>> mSegs;

        bool take()
        {
            if (mQuads + mLines >= mBudget)
            {
                return false;
            }
            return true;
        }
        bool quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d, const LLColor4&) override
        {
            if (!take())
            {
                return false;
            }
            ++mQuads;
            if (a.mV[VZ] != b.mV[VZ] || a.mV[VZ] != c.mV[VZ] || a.mV[VZ] != d.mV[VZ])
            {
                ++mTiltedQuads;
            }
            return true;
        }
        bool line(const LLVector3& a, const LLVector3& b, const LLColor4&) override
        {
            if (!take())
            {
                return false;
            }
            ++mLines;
            mSegs.emplace_back(a, b);
            if (a.mV[VX] == b.mV[VX] && a.mV[VY] == b.mV[VY])
            {
                ++mVerticals;
            }
            return true;
        }
    };

    void addLeaf(TileAir& air, U16 x, U16 y, U8 level, U16 floor, U16 ceiling, U8 flags = AirGap::TOP | AirGap::AVATAR_FLOOR)
    {
        AirLeaf l;
        l.mX = x;
        l.mY = y;
        l.mLevel = level;
        l.mGapCount = 1;
        l.mFirstGap = static_cast<U32>(air.mGaps.size());
        air.mLeaves.push_back(l);
        AirGap g;
        g.mFloor = floor;
        g.mCeiling = ceiling;
        g.mFlags = flags;
        air.mGaps.push_back(g);
    }

    /// Four 2x2 leaves in key order; the one at (0, 2) a step up from the rest.
    TileAir stepped()
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = air.mHeight = 4;
        addLeaf(air, 0, 0, 1, 0, 20);
        addLeaf(air, 2, 0, 1, 0, 20);
        addLeaf(air, 0, 2, 1, 8, 28);
        addLeaf(air, 2, 2, 1, 0, 20);
        return air;
    }
}

namespace
{

    TEST_CASE("skroomsairdebug: gaps draw floors and edges, with verticals only where a neighbour's air differs", "[skroomsairdebug]")
    {
        const TileAir air = stepped();
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        in.mClimb = 2;
        Counter sink;
        AirDebugStats stats;
        { INFO("not cut"); REQUIRE((gatherAirDebug(air, in, sink, &stats))); }
        { INFO("gaps"); REQUIRE((stats.mGaps) == (4u)); }
        { INFO("one floor per gap"); REQUIRE((sink.mQuads) == (4u)); }
        { INFO("no volumes: every fill is a floor"); REQUIRE((sink.mTiltedQuads) == (0u)); }
        // The step's corners: three on itself, five on the ground leaves round it.
        { INFO("verticals"); REQUIRE((sink.mVerticals) == (8u)); }
        { INFO("floor and ceiling outlines plus the verticals"); REQUIRE((sink.mLines) == (4u * 8u + 8u)); }
    }

    TEST_CASE("skroomsairdebug: an open gap gets a tick and no ceiling outline", "[skroomsairdebug]")
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = air.mHeight = 2;
        addLeaf(air, 0, 0, 1, 0, AIR_CEILING_OPEN, AirGap::TOP | AirGap::OPEN | AirGap::OWN_SKY);
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        Counter sink;
        gatherAirDebug(air, in, sink);
        { INFO("floor outline and a tick"); REQUIRE((sink.mLines) == (5u)); }
        { INFO("the tick is the one vertical"); REQUIRE((sink.mVerticals) == (1u)); }
        const auto& tick = sink.mSegs.back();
        { INFO("tick height"); REQUIRE((std::fabs(tick.second.mV[VZ] - in.mOpenTick) < 1e-4f)); }
    }

    TEST_CASE("skroomsairdebug: a step between open gaps stands a tick high", "[skroomsairdebug]")
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = 4;
        air.mHeight = 2;
        const U8 flags = AirGap::TOP | AirGap::OPEN;
        addLeaf(air, 0, 0, 1, 0, AIR_CEILING_OPEN, flags);
        addLeaf(air, 2, 0, 1, 8, AIR_CEILING_OPEN, flags);
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        in.mClimb = 2;
        Counter sink;
        gatherAirDebug(air, in, sink);
        { INFO("two ticks and two step corners each side"); REQUIRE((sink.mVerticals) == (6u)); }
        for (const auto& s : sink.mSegs)
        {
            if (s.first.mV[VX] == s.second.mV[VX] && s.first.mV[VY] == s.second.mV[VY])
            {
                const F32 rise = s.second.mV[VZ] - s.first.mV[VZ];
                { INFO("a tick high"); REQUIRE((std::fabs(rise - in.mOpenTick) < 0.05f)); }
            }
        }
    }

    TEST_CASE("skroomsairdebug: a straight step stands verticals at its ends only, not at every cell along it", "[skroomsairdebug]")
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = 4;
        air.mHeight = 2;
        // Single cells in key order: the row at y 0 on the ground, the row at y 1 two metres up.
        for (U32 key = 0; key < 8; ++key)
        {
            U16 x = 0, y = 0;
            for (U16 cx = 0; cx < 4; ++cx)
            {
                for (U16 cy = 0; cy < 2; ++cy)
                {
                    if (airKey(cx, cy) == key)
                    {
                        x = cx;
                        y = cy;
                    }
                }
            }
            addLeaf(air, x, y, 0, y ? 16 : 0, y ? 36 : 20);
        }
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        in.mClimb = 2;
        Counter sink;
        gatherAirDebug(air, in, sink);
        { INFO("the step's two ends on each side of it"); REQUIRE((sink.mVerticals) == (4u)); }
    }

    TEST_CASE("skroomsairdebug: a ceiling that steps over a level floor stands no vertical", "[skroomsairdebug]")
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = 4;
        air.mHeight = 2;
        addLeaf(air, 0, 0, 1, 0, 20);
        addLeaf(air, 2, 0, 1, 0, 40);
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        in.mClimb = 2;
        Counter sink;
        gatherAirDebug(air, in, sink);
        { INFO("no wall, no step"); REQUIRE((sink.mVerticals) == (0u)); }
    }

    TEST_CASE("skroomsairdebug: the slice keeps only floors within it", "[skroomsairdebug]")
    {
        const TileAir air = stepped();
        AirDebugInput in;
        in.mMode = AirDebugMode::CLEARANCE;
        in.mSliceLo = 0.5f;
        in.mSliceHi = 2.f;
        Counter sink;
        AirDebugStats stats;
        gatherAirDebug(air, in, sink, &stats);
        { INFO("only the step at 1 m"); REQUIRE((stats.mGaps) == (1u)); }
        { INFO("one floor"); REQUIRE((sink.mQuads) == (1u)); }
    }

    TEST_CASE("skroomsairdebug: a sink out of budget stops the gather and marks it cut", "[skroomsairdebug]")
    {
        const TileAir air = stepped();
        AirDebugInput in;
        in.mMode = AirDebugMode::LEAVES;
        Counter sink;
        sink.mBudget = 7;
        AirDebugStats stats;
        { INFO("cut"); REQUIRE((!gatherAirDebug(air, in, sink, &stats))); }
        { INFO("marked"); REQUIRE((stats.mCut)); }
        { INFO("nothing past the budget"); REQUIRE((sink.mQuads + sink.mLines) == (7u)); }
    }

    TEST_CASE("skroomsairdebug: a face offset draws its true face that far into the solid from the cell edge", "[skroomsairdebug]")
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = air.mHeight = 4;
        // Air in the left half, solid in column 2 facing -x with its face 8 steps in.
        addLeaf(air, 0, 0, 1, 0, 20);
        addLeaf(air, 0, 2, 1, 0, 20);
        air.mFaceOffsets.emplace_back(faceKey(2, 1, 4, TileAir::SIDE_NEG_X), 8u);
        AirDebugInput in;
        in.mMode = AirDebugMode::FACES;
        Counter sink;
        AirDebugStats stats;
        gatherAirDebug(air, in, sink, &stats);
        { INFO("one face"); REQUIRE((stats.mFaces) == (1u)); }
        { INFO("boundary and true face"); REQUIRE((sink.mLines) == (2u)); }
        { INFO("boundary on the cell edge"); REQUIRE((std::fabs(sink.mSegs[0].first.mV[VX] - 0.25f) < 1e-5f)); }
        { INFO("true face half a cell in"); REQUIRE((std::fabs(sink.mSegs[1].first.mV[VX] - 0.3125f) < 1e-5f)); }
        { INFO("one cell edge long"); REQUIRE((std::fabs(sink.mSegs[1].second.mV[VY] - sink.mSegs[1].first.mV[VY] - 0.125f) < 1e-5f)); }
    }

    TEST_CASE("skroomsairdebug: the floor under a point is the highest at or below it", "[skroomsairdebug]")
    {
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = air.mHeight = 2;
        AirLeaf l;
        l.mLevel = 1;
        l.mGapCount = 2;
        air.mLeaves.push_back(l);
        AirGap a, b;
        a.mFloor = 0;
        a.mCeiling = 20;
        b.mFloor = 32;
        b.mCeiling = 60;
        air.mGaps = { a, b };
        F32 floor = -1.f;
        { INFO("below the upper storey"); REQUIRE((airFloorUnder(air, LLVector3(0.1f, 0.1f, 3.f), floor))); }
        { INFO("ground floor"); REQUIRE((std::fabs(floor) < 1e-5f)); }
        { INFO("on the upper storey"); REQUIRE((airFloorUnder(air, LLVector3(0.1f, 0.1f, 5.f), floor))); }
        { INFO("upper floor"); REQUIRE((std::fabs(floor - 4.f) < 1e-5f)); }
        { INFO("off the tile"); REQUIRE((!airFloorUnder(air, LLVector3(1.f, 0.1f, 5.f), floor))); }
    }

    TEST_CASE("skroomsairdebug: a space's colour is the one the spaces overlay draws it in", "[skroomsairdebug]")
    {
        EnclosureProduct enclosure;
        enclosure.mSpaceIds = { 7, 42, 90001 };
        RoomsProduct rooms;
        const SpacesDrawPtr draw = buildSpacesDraw(enclosure, rooms);
        { INFO("spaces"); REQUIRE((draw->mSpaces.size()) == (size_t(3))); }
        for (size_t i = 0; i < 3; ++i)
        {
            const LLColor4 c = spaceColour(enclosure.mSpaceIds[i]);
            for (S32 k = 0; k < 3; ++k)
            {
                { INFO("channel"); REQUIRE((std::fabs(c.mV[k] - draw->mSpaces[i].mColor.mV[k]) < 1e-6f)); }
            }
        }
    }
}
