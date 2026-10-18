/**
 * @file skmapmarkers_test.cpp
 * @brief Mini-map marker codes: the sequence, lowest free, holds, stability, and the radius.
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

#include "../skmapmarkers.h"

#include <cmath>

namespace
{
    LLUUID idOf(S32 i)
    {
        LLUUID id;
        id.mData[0] = static_cast<U8>(i + 1);
        id.mData[1] = static_cast<U8>((i + 1) >> 8);
        return id;
    }
}


TEST_CASE("1-9, A-Z, then AA", "[skmapmarkers]")
{
    CHECK((skmap::codeString(0)) == (std::string("1")));
    CHECK((skmap::codeString(8)) == (std::string("9")));
    CHECK((skmap::codeString(9)) == (std::string("A")));
    CHECK((skmap::codeString(34)) == (std::string("Z")));
    CHECK((skmap::codeString(35)) == (std::string("AA")));
    CHECK((skmap::codeString(36)) == (std::string("AB")));
    CHECK((skmap::codeString(60)) == (std::string("AZ")));
    CHECK((skmap::codeString(61)) == (std::string("BA")));
}

TEST_CASE("The lowest free index; a code holds for the whole hold, then is reused lowest first", "[skmapmarkers]")
{
    skmap::MarkerCodes codes;
    codes.setHold(120.f);
    CHECK((codes.codeFor(idOf(0), 0.0)) == (0u));
    CHECK((codes.codeFor(idOf(1), 0.0)) == (1u));
    CHECK((codes.codeFor(idOf(2), 0.0)) == (2u));
    // The first two go out of sight; the third stays.
    for (F64 t = 1.0; t <= 119.0; t += 1.0)
    {
        codes.codeFor(idOf(2), t);
    }
    CHECK((codes.codeFor(idOf(3), 119.5)) == (3u));
    CHECK((codes.has(idOf(0)) && codes.has(idOf(1))));
    codes.codeFor(idOf(2), 121.0);
    codes.codeFor(idOf(3), 121.0);
    CHECK((codes.codeFor(idOf(4), 121.0)) == (0u));
    CHECK((codes.codeFor(idOf(5), 121.0)) == (1u));
    CHECK((!codes.has(idOf(0)) && !codes.has(idOf(1))));
}

TEST_CASE("A code is stable while others come and go", "[skmapmarkers]")
{
    skmap::MarkerCodes codes;
    codes.setHold(5.f);
    const U32 mine = codes.codeFor(idOf(7), 0.0);
    for (S32 i = 0; i < 40; ++i)
    {
        codes.codeFor(idOf(100 + i), i * 2.0);
        CHECK((codes.codeFor(idOf(7), i * 2.0)) == (mine));
    }
}

TEST_CASE("The marker radius, all four cases, and stacked chevrons", "[skmapmarkers]")
{
    CHECK((skmap::markerRadius(0, 1.4f, false, 0, 0.f, 8.f, 0.5f)) == (3.f));
    CHECK((skmap::markerRadius(1, 1.4f, true, 2, 20.f, 8.f, 0.5f)) == (4.5f));
    CHECK((skmap::markerRadius(0, 1.4f, true, 1, 5.f, 8.f, 0.5f)) == (4.5f));
    const F32 wide = skmap::markerRadius(0, 1.4f, true, 2, 12.f, 8.f, 0.5f);
    CHECK(std::fabs(wide - (std::hypot(6.f, 8.f * 0.36f) + 0.5f)) < 1e-5f);
    CHECK(skmap::markerRadius(0, 1.4f, true, 2, 1.f, 8.f, 0.5f) == 4.5f);
    const F32 gap = 1.4f + 8.f * 0.12f;
    CHECK(std::fabs(skmap::markerRadius(2, 1.4f, false, 0, 0.f, 8.f, 0.5f) - (4.5f + gap)) < 1e-5f);
    CHECK(std::fabs(skmap::markerRadius(3, 1.4f, false, 0, 0.f, 8.f, 0.5f) - (4.5f + 2.f * gap)) < 1e-5f);
}

TEST_CASE("Chevron count, steepness and stroke follow the floor bands", "[skmapmarkers]")
{
    const F32 bands[5] = { 2.5f, 6.f, 12.f, 24.f, 48.f };
    CHECK(skmap::chevronStyle(0.f, bands, 1.4f).mCount == 0);
    CHECK(skmap::chevronStyle(2.49f, bands, 1.4f).mCount == 0);
    CHECK(skmap::chevronStyle(2.5f, bands, 1.4f).mCount == 1);
    CHECK(skmap::chevronStyle(5.99f, bands, 1.4f).mCount == 1);
    CHECK(skmap::chevronStyle(6.f, bands, 1.4f).mCount == 2);
    CHECK(skmap::chevronStyle(11.99f, bands, 1.4f).mCount == 2);
    CHECK(skmap::chevronStyle(12.f, bands, 1.4f).mCount == 3);
    CHECK(skmap::chevronStyle(500.f, bands, 1.4f).mCount == 3);

    const skmap::ChevronStyle flat = skmap::chevronStyle(24.f, bands, 1.4f);
    CHECK(std::fabs(flat.mSteep - 0.55f) < 1e-6f);
    CHECK(std::fabs(flat.mStroke - 1.4f) < 1e-6f);
    const skmap::ChevronStyle mid = skmap::chevronStyle(36.f, bands, 1.4f);
    CHECK(std::fabs(mid.mSteep - (0.55f + 0.35f * 0.5f)) < 1e-6f);
    CHECK(std::fabs(mid.mStroke - 1.4f * 1.3f) < 1e-6f);
    const skmap::ChevronStyle steep = skmap::chevronStyle(48.f, bands, 1.4f);
    CHECK(std::fabs(steep.mSteep - 0.9f) < 1e-6f);
    CHECK(std::fabs(steep.mStroke - 1.4f * 1.6f) < 1e-6f);
    const skmap::ChevronStyle past = skmap::chevronStyle(1000.f, bands, 1.4f);
    CHECK(past.mSteep == steep.mSteep);
    CHECK(past.mStroke == steep.mStroke);
}

TEST_CASE("Inverted chevron bands cannot invert the count", "[skmapmarkers]")
{
    // The second band set under the first is raised to it: 2.5 m gives straight to two chevrons.
    const F32 bands[5] = { 2.5f, 1.f, 12.f, 6.f, 3.f };
    CHECK(skmap::chevronStyle(2.f, bands, 1.4f).mCount == 0);
    CHECK(skmap::chevronStyle(2.5f, bands, 1.4f).mCount == 2);
    CHECK(skmap::chevronStyle(12.f, bands, 1.4f).mCount == 3);
    // Bands 4 and 5 collapse onto band 3: past it everything is at its steepest.
    CHECK(std::fabs(skmap::chevronStyle(11.f, bands, 1.4f).mSteep - 0.55f) < 1e-6f);
    CHECK(std::fabs(skmap::chevronStyle(12.f, bands, 1.4f).mSteep - 0.9f) < 1e-6f);
    for (F32 d = 0.f; d < 60.f; d += 0.25f)
    {
        CHECK(skmap::chevronStyle(d, bands, 1.4f).mCount <= skmap::chevronStyle(d + 0.25f, bands, 1.4f).mCount);
    }
}
