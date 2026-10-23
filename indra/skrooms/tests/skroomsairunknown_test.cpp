/**
 * @file skroomsairunknown_test.cpp
 * @brief Tests that air with no enclosure answer is never read as open: depth, wind and the sky map.
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

#include "skroomstestscene.h"
#include "skairskymap.h"
#include "../skroomswind.h"

namespace
{
    using namespace skrooms_test;

    Enclosure at(const EnclosureProductPtr& p, F32 x, F32 y, F32 z = 0.5f)
    {
        Enclosure e;
        p->enclosureAt(LLVector3(x, y, z), e);
        return e;
    }

    /// A closed hollow block: unwalkable floor, closed lid, so its air has no way out and no answer.
    Scene hollowBlock()
    {
        Scene s;
        s.box(4.f, 4.f, 12.f, 12.f, 0.f, 2.f, false);
        s.box(4.f, 4.f, 12.f, 12.f, 4.f, 4.5f, false);
        s.box(4.f, 4.f, 12.f, 4.5f, 2.f, 4.f, false);
        s.box(4.f, 11.5f, 12.f, 12.f, 2.f, 4.f, false);
        s.box(4.f, 4.f, 4.5f, 12.f, 2.f, 4.f, false);
        s.box(11.5f, 4.f, 12.f, 12.f, 2.f, 4.f, false);
        return s;
    }

    /// One 32 m tile's metre tops, every column holding the given interval in air units, or none.
    TileAir tops(U16 bottom, U16 top, bool solid = true)
    {
        TileAir air;
        air.mCell = CELL;
        air.mUnit = UNIT;
        auto t = std::make_shared<TileTops>();
        t->mCell = CELL;
        t->mUnit = UNIT;
        t->mTopCell = static_cast<S32>(1.f / CELL);
        t->mTopWidth = t->mTopHeight = 32;
        t->mTopStart.assign(32 * 32 + 1, 0);
        for (S32 c = 0; c < 32 * 32 && solid; ++c)
        {
            t->mTop.push_back({ bottom, top });
            t->mTopStart[c + 1] = static_cast<U32>(t->mTop.size());
        }
        air.mTops = t;
        return air;
    }
}

TEST_CASE("skroomsairunknown: no answer has no depth", "[skroomsairunknown]")
{
    CHECK_FALSE(Enclosure().depthKnown());
    CHECK(Enclosure().mBucket == EnclosureBucket::UNKNOWN);
}

TEST_CASE("skroomsairunknown: a gap with no walk to the outside reads its depth unknown", "[skroomsairunknown]")
{
    const EnclosureProductPtr p = computeEnclosure(hollowBlock().input(1));
    const Enclosure e = at(p, 8.f, 8.f, 3.f);
    CHECK(e.mBucket == EnclosureBucket::UNKNOWN);
    CHECK_FALSE(e.depthKnown());
    const Enclosure field = at(p, 20.f, 20.f);
    CHECK(field.mBucket == EnclosureBucket::OUTDOORS);
    CHECK(field.depthKnown());
    CHECK(field.mD == 0.f);
}

TEST_CASE("skroomsairunknown: air whose enclosure is UNKNOWN is calm and never a courtyard", "[skroomsairunknown]")
{
    // A closed room with sealing off: no way in, so its air has no answer.
    Scene s;
    s.wall(4.f, 4.f, 16.f, 4.25f);
    s.wall(4.f, 15.75f, 16.f, 16.f);
    s.wall(4.f, 4.f, 4.25f, 16.f);
    s.wall(15.75f, 4.f, 16.f, 16.f);
    s.box(4.f, 4.f, 16.f, 16.f, 3.f, 3.25f);
    auto in = s.input(1);
    in->mParams.mSealedIndoors = false;
    EnclosurePass pass(in);
    while (!pass.step(1u << 30))
    {
    }
    const std::shared_ptr<EnclosureProduct> enc = pass.takeProduct();
    const RoomsProductPtr rooms = computeSpaces(std::make_shared<RoomsInput>(), enc);
    const WindFieldPtr field = buildWindField(*enc, rooms.get(), WindLevel(), WindParams());
    Enclosure e;
    REQUIRE(enc->enclosureAt(LLVector3(10.f, 10.f, 0.5f), e));
    REQUIRE(e.mBucket == EnclosureBucket::UNKNOWN);
    for (F32 heading : { 0.f, 135.f })
    {
        const WindSlabPtr slab = computeWindSlab(*field, rooms.get(), heading, WindParams());
        const WindCell* inside = slab->at(10.f, 10.f);
        const WindCell* outside = slab->at(25.f, 25.f);
        REQUIRE(inside);
        REQUIRE(outside);
        CHECK((inside->mFlags & WindCell::UNKNOWN) != 0);
        CHECK((inside->mFlags & WindCell::INDOORS) == 0);
        CHECK((inside->mFlags & WindCell::COURTYARD) == 0);
        CHECK(inside->factor() < 0.1f);
        CHECK((outside->mFlags & WindCell::UNKNOWN) == 0);
        CHECK(outside->factor() > 0.5f);
    }
}

TEST_CASE("skroomsairunknown: the sky map reads air no tile published as not open, and known bare ground as open", "[skroomsairunknown]")
{
    SkyMap sky;
    CHECK_FALSE(sky.skyOpenAt(LLVector3d(5.0, 5.0, 1.0)));

    // Ground only, its top at the air's height 1 m: every column has a solid, none above.
    sky.set(1, LLVector3d(0.0, 0.0, -1.0), tops(0, static_cast<U16>(1.f / UNIT)));
    CHECK(sky.skyOpenAt(LLVector3d(5.0, 5.0, 1.0)));
    CHECK_FALSE(sky.skyOpenAt(LLVector3d(37.0, 5.0, 1.0)));

    // A tile that saw no solid at all is known open air.
    const TileAir empty = tops(0, 0, false);
    sky.set(2, LLVector3d(32.0, 0.0, -1.0), empty);
    CHECK(sky.has(2));
    CHECK(sky.skyOpenAt(LLVector3d(37.0, 5.0, 1.0)));

    // A roof is still not open, and a removed tile is unknown again.
    sky.set(3, LLVector3d(0.0, 32.0, 0.0), tops(80, 88));
    CHECK_FALSE(sky.skyOpenAt(LLVector3d(5.0, 37.0, 1.0)));
    sky.remove(2);
    CHECK_FALSE(sky.has(2));
    CHECK_FALSE(sky.skyOpenAt(LLVector3d(37.0, 5.0, 1.0)));

    // A snapshot keeps its coverage.
    const SkyMap snapshot = sky;
    sky.remove(1);
    CHECK_FALSE(sky.skyOpenAt(LLVector3d(5.0, 5.0, 1.0)));
    CHECK(snapshot.skyOpenAt(LLVector3d(5.0, 5.0, 1.0)));
}
