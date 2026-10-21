/**
 * @file sknavfloors_test.cpp
 * @brief Tests for the footprint land maximum and the built floor pick.
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

#include "sknavtestscene.h"
#include "../sknavfloors.h"

#include <cmath>

namespace
{
    using namespace sknav;
    using namespace sknav_test;

    Enclosure gap(F32 floor, bool fine, bool avatar)
    {
        Enclosure e;
        e.mFloor = floor;
        e.mFine = fine;
        e.mAvatarFloor = avatar;
        return e;
    }
}

TEST_CASE("a footprint on a slope stands on its high corner")
{
    // Land rising 0.5 m per metre along x.
    const auto slope = [](const LLVector3d& p) { return static_cast<F32>(p.mdV[VX] * 0.5); };
    CHECK(footprintMax(LLVector3d(2.0, 0.0, 9.0), LLVector3d(6.0, 4.0, 11.0), slope) == 3.f);
}

TEST_CASE("an L of land under a footprint is read at the corner it raises")
{
    // A 1 m step over the quarter x > 5, y > 5, which only the far corner sees.
    const auto ell = [](const LLVector3d& p) { return p.mdV[VX] > 5.0 && p.mdV[VY] > 5.0 ? 1.f : 0.f; };
    CHECK(footprintMax(LLVector3d(0.0, 0.0, 0.0), LLVector3d(6.0, 6.0, 1.0), ell) == 1.f);
    // The middle alone sees a block under the footprint's centre.
    const auto centre = [](const LLVector3d& p) { return std::abs(p.mdV[VX] - 3.0) < 1.0 && std::abs(p.mdV[VY] - 3.0) < 1.0 ? 2.f : 0.f; };
    CHECK(footprintMax(LLVector3d(0.0, 0.0, 0.0), LLVector3d(6.0, 6.0, 1.0), centre) == 2.f);
}

TEST_CASE("the floor pick prefers an avatar floor, then fine, then coarse")
{
    F32 floor = -1.f;
    CHECK(pickFloor({ gap(4.f, false, false), gap(2.f, true, false), gap(1.f, true, true) }, 0.f, 5.f, floor));
    CHECK(floor == 1.f);
    CHECK(pickFloor({ gap(4.f, false, false), gap(2.f, true, false), gap(3.f, true, false) }, 0.f, 5.f, floor));
    CHECK(floor == 3.f);
    CHECK(pickFloor({ gap(4.f, false, false), gap(2.f, false, false) }, 0.f, 5.f, floor));
    CHECK(floor == 4.f);
}

TEST_CASE("the floor pick keeps to its band")
{
    F32 floor = -1.f;
    const std::vector<Enclosure> gaps = { gap(0.f, true, true), gap(3.f, true, true), gap(8.f, true, true) };
    CHECK(pickFloor(gaps, 2.25f, 3.75f, floor));
    CHECK(floor == 3.f);
    CHECK(pickFloor(gaps, 0.f, 3.f, floor));
    CHECK(floor == 3.f);
    CHECK_FALSE(pickFloor(gaps, 4.f, 7.f, floor));
    CHECK_FALSE(pickFloor({}, -1e9f, 1e9f, floor));
}

TEST_CASE("a walkable floor reads as an avatar floor, from fine air only")
{
    Scene s;
    s.mAvatarFloors = true;
    s.wall(36.f, 4.f, 48.f, 4.5f);
    s.wall(36.f, 15.5f, 48.f, 16.f);
    s.wall(36.f, 4.f, 36.5f, 16.f);
    s.wall(47.5f, 4.f, 48.f, 16.f);
    s.box(36.f, 4.f, 48.f, 16.f, 3.f, 3.25f);
    auto in = s.input(2);
    const EnclosureProductPtr first = computeEnclosure(in);
    U32 read = 0, fine = 0, avatar = 0;
    first->gapsAt(46.f, 14.f, [&](const Enclosure& e) { ++read; fine += e.mFine; avatar += e.mAvatarFloor; });
    REQUIRE(read >= 2);
    CHECK(fine == read);
    CHECK(avatar == read);

    // Tile 1's detail goes; the pass runs again on tile 0 alone.
    in->mPrevious = first;
    in->mKept.push_back(*first->tile(in->mTiles[1].mKey));
    in->mTiles.pop_back();
    const EnclosureProductPtr second = computeEnclosure(in);
    read = fine = avatar = 0;
    second->gapsAt(46.f, 14.f, [&](const Enclosure& e) { ++read; fine += e.mFine; avatar += e.mAvatarFloor; });
    REQUIRE(read > 0);
    CHECK(fine == 0);
    CHECK(avatar == 0);
    Enclosure live;
    REQUIRE(second->enclosureAt(LLVector3(10.f, 10.f, 0.5f), live));
    CHECK(live.mFine);
    CHECK(live.mAvatarFloor);
}
