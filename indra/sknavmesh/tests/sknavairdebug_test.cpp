/**
 * @file sknavairdebug_test.cpp
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

#include "../test/lltut.h"

#include "../sknavairdebug.h"
#include "../sknavairdistil.h"
#include "../sknavenclosure.h"
#include "../sknavspaces.h"

#include <cmath>
#include <vector>

using namespace sknav;

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

namespace tut
{
    struct sknavairdebug_data
    {
    };
    typedef test_group<sknavairdebug_data> sknavairdebug_group;
    typedef sknavairdebug_group::object sknavairdebug_object;
    tut::sknavairdebug_group sknavairdebug_tests("sknavairdebug");

    template<> template<>
    void sknavairdebug_object::test<1>()
    {
        set_test_name("gaps draw floors and edges, with verticals only where a neighbour's air differs");
        const TileAir air = stepped();
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        in.mClimb = 2;
        Counter sink;
        AirDebugStats stats;
        ensure("not cut", gatherAirDebug(air, in, sink, &stats));
        ensure_equals("gaps", stats.mGaps, 4u);
        ensure_equals("one floor per gap", sink.mQuads, 4u);
        ensure_equals("no volumes: every fill is a floor", sink.mTiltedQuads, 0u);
        // The step's corners: three on itself, five on the ground leaves round it.
        ensure_equals("verticals", sink.mVerticals, 8u);
        ensure_equals("floor and ceiling outlines plus the verticals", sink.mLines, 4u * 8u + 8u);
    }

    template<> template<>
    void sknavairdebug_object::test<2>()
    {
        set_test_name("an open gap gets a tick and no ceiling outline");
        TileAir air;
        air.mCell = air.mUnit = 0.125f;
        air.mWidth = air.mHeight = 2;
        addLeaf(air, 0, 0, 1, 0, AIR_CEILING_OPEN, AirGap::TOP | AirGap::OPEN | AirGap::OWN_SKY);
        AirDebugInput in;
        in.mMode = AirDebugMode::GAPS;
        Counter sink;
        gatherAirDebug(air, in, sink);
        ensure_equals("floor outline and a tick", sink.mLines, 5u);
        ensure_equals("the tick is the one vertical", sink.mVerticals, 1u);
        const auto& tick = sink.mSegs.back();
        ensure("tick height", std::fabs(tick.second.mV[VZ] - in.mOpenTick) < 1e-4f);
    }

    template<> template<>
    void sknavairdebug_object::test<8>()
    {
        set_test_name("a step between open gaps stands a tick high");
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
        ensure_equals("two ticks and two step corners each side", sink.mVerticals, 6u);
        for (const auto& s : sink.mSegs)
        {
            if (s.first.mV[VX] == s.second.mV[VX] && s.first.mV[VY] == s.second.mV[VY])
            {
                const F32 rise = s.second.mV[VZ] - s.first.mV[VZ];
                ensure("a tick high", std::fabs(rise - in.mOpenTick) < 0.05f);
            }
        }
    }

    template<> template<>
    void sknavairdebug_object::test<9>()
    {
        set_test_name("a straight step stands verticals at its ends only, not at every cell along it");
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
        ensure_equals("the step's two ends on each side of it", sink.mVerticals, 4u);
    }

    template<> template<>
    void sknavairdebug_object::test<10>()
    {
        set_test_name("a ceiling that steps over a level floor stands no vertical");
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
        ensure_equals("no wall, no step", sink.mVerticals, 0u);
    }

    template<> template<>
    void sknavairdebug_object::test<3>()
    {
        set_test_name("the slice keeps only floors within it");
        const TileAir air = stepped();
        AirDebugInput in;
        in.mMode = AirDebugMode::CLEARANCE;
        in.mSliceLo = 0.5f;
        in.mSliceHi = 2.f;
        Counter sink;
        AirDebugStats stats;
        gatherAirDebug(air, in, sink, &stats);
        ensure_equals("only the step at 1 m", stats.mGaps, 1u);
        ensure_equals("one floor", sink.mQuads, 1u);
    }

    template<> template<>
    void sknavairdebug_object::test<4>()
    {
        set_test_name("a sink out of budget stops the gather and marks it cut");
        const TileAir air = stepped();
        AirDebugInput in;
        in.mMode = AirDebugMode::LEAVES;
        Counter sink;
        sink.mBudget = 7;
        AirDebugStats stats;
        ensure("cut", !gatherAirDebug(air, in, sink, &stats));
        ensure("marked", stats.mCut);
        ensure_equals("nothing past the budget", sink.mQuads + sink.mLines, 7u);
    }

    template<> template<>
    void sknavairdebug_object::test<5>()
    {
        set_test_name("a face offset draws its true face that far into the solid from the cell edge");
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
        ensure_equals("one face", stats.mFaces, 1u);
        ensure_equals("boundary and true face", sink.mLines, 2u);
        ensure("boundary on the cell edge", std::fabs(sink.mSegs[0].first.mV[VX] - 0.25f) < 1e-5f);
        ensure("true face half a cell in", std::fabs(sink.mSegs[1].first.mV[VX] - 0.3125f) < 1e-5f);
        ensure("one cell edge long", std::fabs(sink.mSegs[1].second.mV[VY] - sink.mSegs[1].first.mV[VY] - 0.125f) < 1e-5f);
    }

    template<> template<>
    void sknavairdebug_object::test<6>()
    {
        set_test_name("the floor under a point is the highest at or below it");
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
        ensure("below the upper storey", airFloorUnder(air, LLVector3(0.1f, 0.1f, 3.f), floor));
        ensure("ground floor", std::fabs(floor) < 1e-5f);
        ensure("on the upper storey", airFloorUnder(air, LLVector3(0.1f, 0.1f, 5.f), floor));
        ensure("upper floor", std::fabs(floor - 4.f) < 1e-5f);
        ensure("off the tile", !airFloorUnder(air, LLVector3(1.f, 0.1f, 5.f), floor));
    }

    template<> template<>
    void sknavairdebug_object::test<7>()
    {
        set_test_name("a space's colour is the one the spaces overlay draws it in");
        EnclosureProduct enclosure;
        enclosure.mSpaceIds = { 7, 42, 90001 };
        RoomsProduct rooms;
        const SpacesDrawPtr draw = buildSpacesDraw(enclosure, rooms);
        ensure_equals("spaces", draw->mSpaces.size(), size_t(3));
        for (size_t i = 0; i < 3; ++i)
        {
            const LLColor4 c = spaceColour(enclosure.mSpaceIds[i]);
            for (S32 k = 0; k < 3; ++k)
            {
                ensure("channel", std::fabs(c.mV[k] - draw->mSpaces[i].mColor.mV[k]) < 1e-6f);
            }
        }
    }
}
