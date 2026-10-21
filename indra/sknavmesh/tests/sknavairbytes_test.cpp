/**
 * @file sknavairbytes_test.cpp
 * @brief The live air and tops byte counts follow what is alive.
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

#include "sknavairdistil.h"
#include "sknavskymap.h"
#include "sknavtilecache.h"

#include <catch2/catch_test_macros.hpp>

using namespace sknav;

namespace
{
    /// A tile's air as a build finishes it: leaves, gaps and its own tops, counted.
    std::shared_ptr<TileAir> builtAir(size_t leaves, size_t gaps)
    {
        auto air = std::make_shared<TileAir>();
        air->mCell = 0.125f;
        air->mUnit = 0.125f;
        air->mWidth = air->mHeight = 256;
        air->mLeaves.resize(leaves);
        air->mGaps.resize(gaps);
        auto tops = std::make_shared<TileTops>();
        tops->mCell = air->mCell;
        tops->mUnit = air->mUnit;
        tops->mTopCell = 8;
        tops->mTopWidth = tops->mTopHeight = 32;
        tops->mTopStart.assign(32 * 32 + 1, 0);
        for (S32 c = 0; c < 32 * 32; ++c)
        {
            tops->mTop.push_back({ 0, 8 });
            tops->mTopStart[c + 1] = static_cast<U32>(tops->mTop.size());
        }
        tops->countLive();
        air->mTops = tops;
        air->countLive();
        return air;
    }
}

TEST_CASE("sknavairbytes: a built tile's bytes are counted and released", "[sknavairbytes]")
{
    const size_t live = gAirLiveBytes.load();
    const size_t tops = gAirTopsBytes.load();
    {
        const auto air = builtAir(1000, 3000);
        CHECK(gAirLiveBytes.load() == live + air->bytes());
        CHECK(gAirTopsBytes.load() == tops + air->mTops->bytes());

        // A copy is not a second build: it starts uncounted, and going counts nothing off.
        {
            const TileAir copy = *air;
            CHECK(gAirLiveBytes.load() == live + air->bytes());
        }
        CHECK(gAirLiveBytes.load() == live + air->bytes());

        // Counted again after a late addition, the count is replaced, not added to.
        auto& open = const_cast<TileAir&>(*air);
        open.mFaceOffsets.resize(500);
        open.countLive();
        CHECK(gAirLiveBytes.load() == live + air->bytes());
    }
    CHECK(gAirLiveBytes.load() == live);
    CHECK(gAirTopsBytes.load() == tops);
}

TEST_CASE("sknavairbytes: the tops outlive the air they came from", "[sknavairbytes]")
{
    const size_t live = gAirLiveBytes.load();
    const size_t tops = gAirTopsBytes.load();
    auto air = builtAir(4000, 12000);
    const size_t detail = air->bytes();
    const size_t top_bytes = air->mTops->bytes();

    // The coarse air an evicted tile keeps shares the tops and costs its header.
    TileAirPtr coarse = coarseAir(*air);
    REQUIRE(coarse);
    CHECK(coarse->mTops == air->mTops);
    CHECK(sameTops(*coarse, *air));
    CHECK(coarse->bytes() < detail / 10);
    CHECK(gAirLiveBytes.load() == live + detail + coarse->bytes());
    CHECK(gAirTopsBytes.load() == tops + top_bytes);

    // Evicted: the detail goes, the tops stay with the coarse air.
    air.reset();
    CHECK(gAirLiveBytes.load() == live + coarse->bytes());
    CHECK(gAirTopsBytes.load() == tops + top_bytes);

    // And the sky map holds the tops alone.
    SkyMap sky;
    sky.set(1, LLVector3d(0.0, 0.0, 0.0), *coarse->mTops);
    F64 top = 0.0;
    CHECK(sky.topAt(5.5, 5.5, top));

    coarse.reset();
    CHECK(gAirLiveBytes.load() == live);
    CHECK(gAirTopsBytes.load() == tops);
}

TEST_CASE("sknavairbytes: the counter is zero after every tile is gone", "[sknavairbytes]")
{
    const size_t live = gAirLiveBytes.load();
    const size_t tops = gAirTopsBytes.load();
    {
        std::vector<TileAirPtr> tiles;
        for (S32 i = 0; i < 16; ++i)
        {
            const auto air = builtAir(100 + 50 * i, 300 + 100 * i);
            tiles.push_back(air);
            tiles.push_back(coarseAir(*air));
        }
        CHECK(gAirLiveBytes.load() > live);
        CHECK(gAirTopsBytes.load() > tops);
        // Retired in any order, as the workers retire them.
        for (size_t i = 0; i < tiles.size(); i += 2)
        {
            tiles[i].reset();
        }
        CHECK(gAirTopsBytes.load() > tops);
    }
    CHECK(gAirLiveBytes.load() == live);
    CHECK(gAirTopsBytes.load() == tops);
}
