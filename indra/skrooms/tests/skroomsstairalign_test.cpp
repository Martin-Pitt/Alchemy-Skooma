/**
 * @file skroomsstairalign_test.cpp
 * @brief Stair portals squared to their flights: straight runs, quarter turns, spirals.
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


#include "../skroomsstairalign.h"

#include "llmath.h"

#include <cmath>

namespace
{
    using namespace skworld;

    constexpr F32 RISER = 0.18f;
    constexpr F32 GOING = 0.25f;
    constexpr F32 TOL = 0.4f;   // the flight floor a sample finds lies within this of the height it looks near

    F32 steps(F32 run) { return std::floor(run / GOING) * RISER; }

    bool within(F32 found, F32 z, F32& out)
    {
        if (std::fabs(found - z) > TOL)
        {
            return false;
        }
        out = found;
        return true;
    }

    // A 2 m wide flight climbing +y from y = 0 to y = 4.
    bool straight(F32 x, F32 y, F32 z, F32& out)
    {
        return x >= 0.f && x <= 2.f && y >= 0.f && y <= 4.f && within(steps(y), z, out);
    }

    // Up +y over x 0-1.5 to a landing at y 3-4.5, then up from x = 1.5 to 4.5 on treads laid 3 degrees off square.
    bool quarter(F32 x, F32 y, F32 z, F32& out)
    {
        const F32 h1 = steps(3.f - 0.001f);
        if (x >= 0.f && x <= 1.5f && y >= 0.f && y < 3.f)
        {
            return within(steps(y), z, out);
        }
        if (x >= 0.f && x <= 1.5f && y >= 3.f && y <= 4.5f)
        {
            return within(h1, z, out);
        }
        if (x > 1.5f && x <= 4.5f && y >= 3.f && y <= 4.5f)
        {
            const F32 run = (x - 1.5f) * std::cos(3.f * DEG_TO_RAD) + (y - 3.75f) * std::sin(3.f * DEG_TO_RAD);
            return within(h1 + RISER + steps(std::max(run, 0.f)), z, out);
        }
        return false;
    }

    // Three quarters of a turn about the origin between radii 1 and 3, a riser every 15 degrees.
    bool spiral(F32 x, F32 y, F32 z, F32& out)
    {
        const F32 r = std::sqrt(x * x + y * y);
        F32 a = std::atan2(y, x);
        if (a < 0.f)
        {
            a += F_TWO_PI;
        }
        if (r < 1.f || r > 3.f || a > 1.5f * F_PI)
        {
            return false;
        }
        return within(std::floor(a / (15.f * DEG_TO_RAD)) * RISER, z, out);
    }

    StairEnd end(const LLVector2& a, const LLVector2& b, const LLVector2& pivot, F32 z, const LLVector2& into, U32 flight)
    {
        StairEnd e;
        e.mA = a;
        e.mB = b;
        e.mPivot = pivot;
        e.mZ = z;
        e.mInto = into;
        e.mInto.normVec();
        e.mSeed = pivot + e.mInto * 0.1f;
        e.mFlight = flight;
        return e;
    }

    LLVector2 dirOf(const StairEnd& e)
    {
        LLVector2 d = e.mB - e.mA;
        d.normVec();
        return d;
    }
}

namespace
{

    // A contact cut 17 degrees askew across a straight flight's foot, and
    // one askew the other way across its top, both come out square to the
    // rise, parallel and as wide as the flight.
    TEST_CASE("skroomsstairalign: A contact cut 17 degrees askew across a straight flight's foot, and one askew the other way across its top, both...", "[skroomsstairalign]")
    {
        std::vector<StairEnd> ends;
        ends.push_back(end(LLVector2(0.f, 0.3f), LLVector2(2.f, -0.3f), LLVector2(1.f, 0.02f), 0.f, LLVector2(0.29f, 0.96f), 7));
        ends.push_back(end(LLVector2(0.f, 3.7f), LLVector2(2.f, 4.3f), LLVector2(1.f, 3.98f), steps(3.98f), LLVector2(0.29f, -0.96f), 7));
        alignStairEnds(straight, ends);
        for (const StairEnd& e : ends)
        {
            { INFO("the end was squared"); REQUIRE((e.mAligned && !e.mFree)); }
            { INFO("it climbs +y"); REQUIRE(std::fabs((e.mRise.mV[VY]) - (1.f)) <= (0.01f)); }
            { INFO("it runs along the treads"); REQUIRE(std::fabs((std::fabs(dirOf(e).mV[VX])) - (1.f)) <= (2e-4f)); }
            { INFO("across the flight's width"); REQUIRE(std::fabs((dist_vec(e.mA, e.mB)) - (2.f)) <= (0.3f)); }
        }
        { INFO("the ends are parallel"); REQUIRE(std::fabs((std::fabs(dirOf(ends[0]) * dirOf(ends[1]))) - (1.f)) <= (1e-6f)); }
    }

    // A quarter turn's ends, each cut 10 degrees askew, square to their own
    // runs and turn by exactly a right angle, though the upper run's treads
    // lie 3 degrees off: the wider foot leads.
    TEST_CASE("skroomsstairalign: A quarter turn's ends, each cut 10 degrees askew, square to their own runs and turn by exactly a right angle, though...", "[skroomsstairalign]")
    {
        const F32 top = steps(3.f - 0.001f) + RISER + steps(2.999f);
        std::vector<StairEnd> ends;
        ends.push_back(end(LLVector2(-0.05f, 0.14f), LLVector2(1.55f, -0.14f), LLVector2(0.75f, 0.02f), 0.f, LLVector2(0.17f, 0.98f), 3));
        ends.push_back(end(LLVector2(4.37f, 3.f), LLVector2(4.63f, 4.5f), LLVector2(4.48f, 3.75f), top, LLVector2(-0.98f, 0.17f), 3));
        alignStairEnds(quarter, ends);
        { INFO("both ends were squared"); REQUIRE((ends[0].mAligned && ends[1].mAligned)); }
        { INFO("the foot runs along x"); REQUIRE(std::fabs((std::fabs(dirOf(ends[0]).mV[VX])) - (1.f)) <= (2e-4f)); }
        { INFO("the top runs along y"); REQUIRE(std::fabs((std::fabs(dirOf(ends[1]).mV[VY])) - (1.f)) <= (2e-4f)); }
        { INFO("they meet at a right angle"); REQUIRE(std::fabs((dirOf(ends[0]) * dirOf(ends[1])) - (0.f)) <= (1e-5f)); }
    }

    // A spiral's rise turns all the way up, so its ends keep their cuts,
    // however askew; a straight flight's do not count as curving.
    TEST_CASE("skroomsstairalign: A spiral's rise turns all the way up, so its ends keep their cuts, however askew; a straight flight's do not count...", "[skroomsstairalign]")
    {
        const LLVector2 a(1.f, 0.05f), b(3.f, 0.35f);
        std::vector<StairEnd> ends;
        ends.push_back(end(a, b, LLVector2(2.f, 0.02f), 0.f, LLVector2(-0.15f, 0.99f), 1));
        alignStairEnds(spiral, ends);
        { INFO("the rise was found"); REQUIRE((!ends[0].mRise.isExactlyZero())); }
        { INFO("the spiral curves"); REQUIRE((ends[0].mFree)); }
        { INFO("and keeps its cut"); REQUIRE((!ends[0].mAligned && ends[0].mA == a && ends[0].mB == b)); }
        { INFO("a straight flight does not curve"); REQUIRE((!flightCurves(straight, LLVector2(1.f, 0.1f), 0.f, LLVector2(0.f, 1.f), true))); }
    }
}
