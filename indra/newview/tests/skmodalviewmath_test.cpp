/**
 * @file skmodalviewmath_test.cpp
 * @brief The modal view's claim order and its follow-relative camera restore.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

TEST_CASE("move actions add up to one step a direction")
{
    Move move;
    CHECK(addMoveAction("push_forward", move));
    CHECK(addMoveAction("run_forward", move));
    CHECK(addMoveAction("turn_left", move));
    CHECK(addMoveAction("jump", move));
    CHECK(move.mForward == 1.f);
    CHECK(move.mLeft == 1.f);
    CHECK(move.mUp == 1.f);
    CHECK(move.mFast);

    CHECK(addMoveAction("slide_right", move));
    CHECK(move.mLeft == 0.f);
    CHECK_FALSE(addMoveAction("toggle_fly", move));
    CHECK_FALSE(Move().any());
}

TEST_CASE("the ground direction is screen-up on the ground, even looking straight down")
{
    LLVector3 dir;
    REQUIRE(groundDirection(LLVector3(0.f, 0.7071f, -0.7071f), LLVector3(0.f, 0.7071f, 0.7071f), 1.f, 0.f, dir));
    CHECK(dir.mV[VY] > 0.999f);
    REQUIRE(groundDirection(LLVector3(0.f, 1.f, 0.f), LLVector3::z_axis, 0.f, 1.f, dir));
    CHECK(dir.mV[VX] < -0.999f); // left of north is west
    REQUIRE(groundDirection(LLVector3(0.f, 0.f, -1.f), LLVector3(1.f, 0.f, 0.f), 1.f, 0.f, dir));
    CHECK(dir.mV[VX] > 0.999f);
    CHECK_FALSE(groundDirection(LLVector3(0.f, 0.f, -1.f), LLVector3(0.f, 0.f, 1.f), 1.f, 0.f, dir));
}

TEST_CASE("the flycam speeds up with height and eases onto its cap")
{
    CHECK(flycamSpeedScale(-50.0, 40.0) == 1.0);
    CHECK(flycamSpeedScale(FLYCAM_REFERENCE_HEIGHT, 40.0) == 1.0);
    CHECK(flycamSpeedScale(20.0, 40.0) > 1.9); // nearly linear just above the reference
    CHECK(flycamSpeedScale(500.0, 40.0) < 40.0);
    CHECK(flycamSpeedScale(1.0e6, 40.0) <= 40.0);
    CHECK(flycamSpeedScale(1.0e6, 40.0) > 39.9);
    CHECK(flycamSpeedScale(200.0, 40.0) > flycamSpeedScale(100.0, 40.0));
    CHECK(flycamSpeedScale(1.0e6, 1.0) == 1.0);
}
