/**
 * @file sknavtileclip_test.cpp
 * @brief Tests that a tile builds from the part of each record within its reach.
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

#include "../sknavbuild.h"

#include <algorithm>

namespace
{
    using namespace sknav;

    TileBuildInput tile()
    {
        TileBuildInput in;
        in.mZMin = -1.f;
        in.mZMax = 12.f;
        in.mCell = 0.25f;
        in.mTerrain.mOrigin.set(-2.f, -2.f, 0.f);
        in.mTerrain.mSpacing = 1.f;
        in.mTerrain.mWidth = 37;
        in.mTerrain.mHeight = 37;
        in.mTerrain.mZ.assign(37 * 37, 0.f);
        in.mTerrain.mWater = -10.f;
        return in;
    }

    // A record of 1 m floor quads at half a metre, from x0 to x1 and across the tile.
    RecordPtr floorRecord(F32 x0, F32 x1, bool solid = false)
    {
        auto tris = std::make_shared<std::vector<LLVector3>>();
        for (F32 x = x0; x < x1; x += 1.f)
        {
            for (F32 y = 0.f; y < 32.f; y += 1.f)
            {
                const F32 z = 0.5f;
                tris->insert(tris->end(), { LLVector3(x, y, z), LLVector3(x + 1, y, z), LLVector3(x + 1, y + 1, z),
                                            LLVector3(x, y, z), LLVector3(x + 1, y + 1, z), LLVector3(x, y + 1, z) });
            }
        }
        auto r = std::make_shared<Record>();
        r->mShape = Shape::TRIANGLES;
        r->mSolid = solid;
        r->mTriangles = tris;
        r->mMin.set(x0, 0.0, 0.5);
        r->mMax.set(x1, 32.0, 0.5);
        return r;
    }
}

TEST_CASE("a tile takes only the part of a big record within its reach")
{
    TileBuildInput in = tile();
    in.mRecords.push_back(floorRecord(-1000.f, 1032.f));
    const TileBuildResult whole = buildTile(in);
    in.mRecords[0] = floorRecord(-20.f, 52.f);
    const TileBuildResult cut = buildTile(in);
    REQUIRE(whole.mStatus == TileBuildResult::SUCCESS);
    REQUIRE(cut.mStatus == TileBuildResult::SUCCESS);
    // 2032 m of floor is 130 048 triangles; the reach is about 65 m of it.
    CHECK(whole.mTriangles < 8000);
    CHECK(whole.mTriangles == cut.mTriangles);
    CHECK(whole.mData[0] == cut.mData[0]);
}

TEST_CASE("a solid body reaching into the tile is kept whole")
{
    TileBuildInput in = tile();
    in.mRecords.push_back(floorRecord(-1000.f, 1032.f, true));
    const TileBuildResult r = buildTile(in);
    REQUIRE(r.mStatus == TileBuildResult::SUCCESS);
    CHECK(r.mTriangles > 130000);
}

TEST_CASE("a record wholly outside the reach leaves the tile as if it were not there")
{
    TileBuildInput in = tile();
    const TileBuildResult bare = buildTile(in);
    in.mRecords.push_back(floorRecord(100.f, 200.f));
    const TileBuildResult distant = buildTile(in);
    REQUIRE(distant.mStatus == TileBuildResult::SUCCESS);
    CHECK(distant.mTriangles == bare.mTriangles);
    CHECK(distant.mData[0] == bare.mData[0]);
}
