/**
 * @file skairwalls_test.cpp
 * @brief Walls off a built tile: cover classes, wall edges, ledges, drop links, exposed faces.
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

#include "skairtestscene.h"

#include "../skairraster.h"
#include "../skairwalls.h"

#include "llformat.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    using namespace skworld;
    using namespace airscene;

    TileInput wallTile()
    {
        TileInput in = airTile(0, 0);
        in.mZMax = 24.f;
        in.mTerrain = flat(0.f);
        in.mTerrain.mWater = -100.f;
        return in;
    }

    TileAirResult build(const TileInput& in)
    {
        TileAirResult r;
        rasteriseTile(in, r);
        return r;
    }

    /// Total length of the segments of one kind, and the mean of their values.
    void summarise(const std::vector<WallSegment>& segs, EdgeKind kind, F32& length, F32& mean_value)
    {
        length = 0.f;
        F32 weighted = 0.f;
        for (const WallSegment& s : segs)
        {
            if (s.mKind != kind)
            {
                continue;
            }
            const F32 len = (s.mB - s.mA).magVec();
            length += len;
            weighted += len * s.mValue;
        }
        mean_value = length > 0.f ? weighted / length : 0.f;
    }

    /// The face area inside a plan box.
    F32 faceArea(const std::vector<WallFace>& faces, F32 x0, F32 y0, F32 x1, F32 y1, F32* per_side = nullptr, F32* highest = nullptr)
    {
        F32 area = 0.f;
        for (const WallFace& f : faces)
        {
            if (f.mMin.mV[VX] > x0 && f.mMax.mV[VX] < x1 && f.mMin.mV[VY] > y0 && f.mMax.mV[VY] < y1)
            {
                const LLVector3 size = f.mMax - f.mMin;
                const F32 a = std::max(size.mV[VX], size.mV[VY]) * size.mV[VZ];
                area += a;
                if (per_side)
                {
                    per_side[f.mSide] += a;
                }
                if (highest)
                {
                    *highest = std::max(*highest, f.mMax.mV[VZ]);
                }
            }
        }
        return area;
    }
}

TEST_CASE("skairwalls: cover is read against the reader's own eye height", "[skairwalls]")
{
    const F32 climb = 0.75f;
    CHECK(coverClass(0.5f, 1.7f, climb) == Cover::NONE);
    CHECK(coverClass(1.0f, 1.7f, climb) == Cover::HALF);
    CHECK(coverClass(1.6f, 1.7f, climb) == Cover::FULL);
    CHECK(coverClass(1.6f, 2.2f, climb) == Cover::HALF);
    CHECK(coverClass(2.2f, 2.2f, climb) == Cover::FULL);
    CHECK(coverClass(climb, 1.7f, climb) == Cover::HALF);
    CHECK(coverClass(climb - 0.01f, 1.7f, climb) == Cover::NONE);
}

TEST_CASE("skairwalls: a platform's top is a ledge all round with run-off down to the land", "[skairwalls]")
{
    TileInput in = wallTile();
    in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 3.f));
    const TileAirResult r = build(in);
    INFO(r.mError);
    REQUIRE(r.mStatus == TileAirResult::SUCCESS);
    F32 ledge_len = 0.f, drop = 0.f;
    summarise(r.mLedges, EdgeKind::LEDGE, ledge_len, drop);
    INFO(llformat("ledges %.1f m, drop %.2f", ledge_len, drop));
    CHECK(ledge_len > 40.f);
    CHECK(std::fabs(drop - 3.f) < 0.4f);
    CHECK(r.mRunoff.size() > 15);
    for (const DropLink& link : r.mRunoff)
    {
        CHECK(std::fabs(link.mHeight - 3.f) < 0.4f);
        CHECK(link.mFrom.mV[VZ] > 2.5f);
        CHECK(std::fabs(link.mTo.mV[VZ]) < 0.5f);
    }
}

TEST_CASE("skairwalls: run-off reaches as far down as its limit", "[skairwalls]")
{
    TileInput in = wallTile();
    in.mRunoffMax = 24.f;
    in.mRecords.push_back(box(8.f, 8.f, 0.f, 24.f, 24.f, 15.f));
    const TileAirResult r = build(in);
    REQUIRE(r.mStatus == TileAirResult::SUCCESS);
    F32 ledge_len = 0.f, drop = 0.f;
    summarise(r.mLedges, EdgeKind::LEDGE, ledge_len, drop);
    CHECK(ledge_len > 40.f);
    CHECK(std::fabs(drop - 15.f) < 0.6f);
    REQUIRE(!r.mRunoff.empty());
    for (const DropLink& link : r.mRunoff)
    {
        CHECK(std::fabs(link.mHeight - 15.f) < 0.6f);
    }
    in.mRunoffMax = 10.f;
    CHECK(build(in).mRunoff.empty());
}

TEST_CASE("skairwalls: a block on the land shows its four sides up to its top", "[skairwalls]")
{
    TileInput in = wallTile();
    in.mRecords.push_back(box(12.f, 12.f, 0.f, 20.f, 20.f, 2.f));
    const TileAirResult r = build(in);
    REQUIRE(r.mStatus == TileAirResult::SUCCESS);
    REQUIRE(!r.mFaces.empty());
    F32 area[4] = { 0.f, 0.f, 0.f, 0.f };
    F32 highest = 0.f;
    faceArea(r.mFaces, 10.f, 10.f, 22.f, 22.f, area, &highest);
    for (S32 side = 0; side < 4; ++side)
    {
        INFO("side " << side);
        CHECK(area[side] > 8.f);
    }
    CHECK(std::fabs(highest - 2.f) < 0.3f);
}

TEST_CASE("skairwalls: a block AVATAR leaves out still shows its sides", "[skairwalls]")
{
    TileInput in = wallTile();
    auto block = std::make_shared<Record>(*box(12.f, 12.f, 0.f, 20.f, 20.f, 2.f));
    block->mExclude = EXCLUDE_AVATAR;
    in.mRecords.push_back(block);
    in.mRecords.push_back(box(2.f, 2.f, 0.f, 6.f, 6.f, 2.f));
    const TileAirResult r = build(in);
    REQUIRE(r.mStatus == TileAirResult::SUCCESS);
    CHECK(faceArea(r.mFaces, 10.f, 10.f, 22.f, 22.f) > 32.f);
}
