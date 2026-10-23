/**
 * @file skairskymap_test.cpp
 * @brief The sky map's blocks are copied before a write while a snapshot reads them.
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

#include "skairdistil.h"
#include "skairskymap.h"

#include <catch2/catch_test_macros.hpp>

using namespace skworld;

namespace
{
    /// A 32 m tile's metre tops, every column solid from bottom to top, in 0.125 m units.
    TileTops solid(U16 bottom, U16 top)
    {
        TileTops t;
        t.mCell = 0.125f;
        t.mUnit = 0.125f;
        t.mTopCell = 8;
        t.mTopWidth = t.mTopHeight = 32;
        t.mTopStart.assign(32 * 32 + 1, 0);
        for (S32 c = 0; c < 32 * 32; ++c)
        {
            t.mTop.push_back({ bottom, top });
            t.mTopStart[c + 1] = static_cast<U32>(t.mTop.size());
        }
        return t;
    }
}

TEST_CASE("skairskymap: a write while a snapshot reads copies the block", "[skairskymap]")
{
    SkyMap sky;
    sky.set(1, LLVector3d(0.0, 0.0, 0.0), solid(0, 8));     // ground, 1 m

    // The worker's copy, as syncSkyMap hands it one: blocks shared until written.
    const SkyMap snapshot = sky;
    std::vector<std::shared_ptr<const void>> keep;
    const U64 before = snapshot.windowKey(0.0, 0.0, 31.0, 31.0, keep);
    CHECK(sky.windowKey(0.0, 0.0, 31.0, 31.0, keep) == before);

    // A roof over the same metres: the writer copies the block rather than write under the reader.
    sky.set(2, LLVector3d(0.0, 0.0, 0.0), solid(80, 88));
    F64 top = 0.0;
    REQUIRE(sky.topAt(5.5, 5.5, top));
    CHECK(top == 11.0);
    REQUIRE(snapshot.topAt(5.5, 5.5, top));
    CHECK(top == 1.0);
    CHECK(snapshot.windowKey(0.0, 0.0, 31.0, 31.0, keep) == before);
    CHECK(sky.windowKey(0.0, 0.0, 31.0, 31.0, keep) != before);

    // With no reader left, a second write goes into the block in place.
    keep.clear();
    const U64 written = sky.windowKey(0.0, 0.0, 31.0, 31.0, keep);
    keep.clear();
    sky.remove(2);
    CHECK(sky.windowKey(0.0, 0.0, 31.0, 31.0, keep) == written);
    REQUIRE(sky.topAt(5.5, 5.5, top));
    CHECK(top == 1.0);
}
