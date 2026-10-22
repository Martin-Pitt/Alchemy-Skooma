/**
 * @file skmodalviewmath_test.cpp
 * @brief The modal view's claim order and its follow-relative camera restore.
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

#include "../skmodalviewmath.h"

using namespace skmodalview;

namespace
{
    int MAP = 1;      // writable, so the linker cannot fold the two keys into one address
    int OVERVIEW = 2;
    const void* map_key = &MAP;
    const void* overview_key = &OVERVIEW;
    constexpr S32 MAP_PRIORITY = 0;
    constexpr S32 OVERVIEW_PRIORITY = 100;
} // namespace

TEST_CASE("an empty claim set has no top")
{
    Claims claims;
    CHECK(claims.empty());
    CHECK(claims.top() == nullptr);
    CHECK_FALSE(claims.release(map_key));
}

TEST_CASE("the higher priority is on top whatever the claim order")
{
    Claims claims;
    claims.claim(map_key, MAP_PRIORITY);
    claims.claim(overview_key, OVERVIEW_PRIORITY);
    CHECK(claims.top() == overview_key);

    Claims reversed;
    reversed.claim(overview_key, OVERVIEW_PRIORITY);
    reversed.claim(map_key, MAP_PRIORITY);
    CHECK(reversed.top() == overview_key);
    CHECK(reversed.ordered() == std::vector<const void*>{ overview_key, map_key });

    reversed.release(overview_key);
    CHECK(reversed.top() == map_key);
    CHECK(reversed.size() == 1);
}

TEST_CASE("among equal priorities the newest claim is on top")
{
    Claims claims;
    claims.claim(map_key, 5);
    claims.claim(overview_key, 5);
    CHECK(claims.top() == overview_key);
    claims.claim(map_key, 5);
    CHECK(claims.top() == map_key);
    CHECK(claims.size() == 2);
}

TEST_CASE("claiming again replaces the priority rather than adding a claim")
{
    Claims claims;
    claims.claim(map_key, 200);
    claims.claim(overview_key, OVERVIEW_PRIORITY);
    claims.claim(map_key, MAP_PRIORITY);
    CHECK(claims.size() == 2);
    CHECK(claims.top() == overview_key);
    CHECK(claims.holds(map_key));
    CHECK(claims.release(map_key));
    CHECK_FALSE(claims.holds(map_key));
}

TEST_CASE("a following camera is restored at the same offset from the moved agent")
{
    const LLVector3d agent_then(256100.0, 256200.0, 30.0);
    const LLVector3d agent_now(256400.0, 255900.0, 42.0);
    const LLVector3d eye(256096.0, 256197.0, 33.0);

    const LLVector3d stored = storePoint(eye, agent_then, true);
    CHECK(stored == LLVector3d(-4.0, -3.0, 3.0));
    CHECK(restorePoint(stored, agent_then, true) == eye);
    CHECK(restorePoint(stored, agent_now, true) == LLVector3d(256396.0, 255897.0, 45.0));
}

TEST_CASE("a camera that did not follow is restored where it was")
{
    const LLVector3d agent_then(256100.0, 256200.0, 30.0);
    const LLVector3d agent_now(256400.0, 255900.0, 42.0);
    const LLVector3d eye(256096.0, 256197.0, 33.0);

    const LLVector3d stored = storePoint(eye, agent_then, false);
    CHECK(stored == eye);
    CHECK(restorePoint(stored, agent_now, false) == eye);
}
