/**
 * @file skroomscensus_test.cpp
 * @brief The rooms layer's census section round trip.
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

#include "../skroomscensus.h"

#include "llsd.h"

#include <sstream>

namespace
{
    using namespace skworld;

    TEST_CASE("skroomscensus: the enclosure parameters round trip through their section", "[skroomscensus]")
    {
        EnclosureParams out;
        out.mSkyDecayM = 3.5f;
        out.mWalkHeadM = 2.f;
        out.mShelterOpen = 0.25f;
        out.mSealedIndoors = false;
        std::ostringstream text;
        writeCensus(text, CensusFile(), { enclosureParamsSection(out) });
        EnclosureParams back;
        CensusFile file;
        std::string error;
        REQUIRE(readCensus(text.str(), file, &error, { enclosureParamsSection(back) }));
        CHECK(back.mSkyDecayM == out.mSkyDecayM);
        CHECK(back.mWalkHeadM == out.mWalkHeadM);
        CHECK(back.mShelterOpen == out.mShelterOpen);
        CHECK(back.mSealedIndoors == out.mSealedIndoors);
        CHECK(back.mClimbM == out.mClimbM);
        CHECK_FALSE(readEnclosureParams(LLSD("not a map"), 7, back));
    }
}
