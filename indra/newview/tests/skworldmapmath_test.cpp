/**
 * @file skworldmapmath_test.cpp
 * @brief The 3D world map's pan, tile levels and dot stacks, pinned to the Python reference.
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

#include "../skworldmapmath.h"

#include <cmath>
#include <map>
#include <set>
#include <utility>

using namespace skworldmap;

namespace
{
    constexpr F64 DEG = 3.141592653589793 / 180.0;

    View viewAt(const LLVector3d& eye, F64 yaw, F64 pitch)
    {
        View v;
        v.mEye = eye;
        v.mYaw = yaw;
        v.mPitch = pitch;
        v.mFovY = 45.0 * DEG;
        v.mAspect = 16.0 / 9.0;
        v.mScreenHeight = 1080.0;
        return v;
    }

    const SelectedTile* tileUnder(const std::vector<SelectedTile>& tiles, F64 x, F64 y)
    {
        for (const SelectedTile& t : tiles)
        {
            const F64 x0 = t.mKey.mGridX * REGION_METRES;
            const F64 y0 = t.mKey.mGridY * REGION_METRES;
            if (x >= x0 && x < x0 + t.mKey.sideMetres() && y >= y0 && y < y0 + t.mKey.sideMetres())
            {
                return &t;
            }
        }
        return nullptr;
    }
} // namespace

namespace
{

    // A pan keeps the grabbed ground point under the cursor, and never moves the camera up or down.
    TEST_CASE("skworldmapmath: A pan keeps the grabbed ground point under the cursor, and never moves the camera up or down", "[skworldmapmath]")
    {
        const LLVector3d eye(1000.0, 2000.0, GROUND_Z + 500.0);
        LLVector3d dir(0.3, 0.5, -0.8);
        dir.normalize();
        LLVector3d hit;
        CHECK(groundHit(eye, dir, hit)); // hits
        CHECK(std::fabs(hit.mdV[VZ] - GROUND_Z) < 1.0e-9); // on the plane
        const LLVector3d grabbed(1500.0, 2500.0, GROUND_Z);
        LLVector3d offset;
        CHECK(panOffset(eye, dir, grabbed, offset)); // offset
        CHECK(offset.mdV[VZ] == 0.0); // flat
        LLVector3d moved_hit;
        groundHit(eye + offset, dir, moved_hit);
        CHECK((moved_hit - grabbed).magVec() < 1.0e-6); // grabbed point under the cursor
    }

    // A ray at or above the horizon never meets the ground.
    TEST_CASE("skworldmapmath: A ray at or above the horizon never meets the ground", "[skworldmapmath]")
    {
        LLVector3d hit, offset;
        CHECK(!groundHit(LLVector3d(0.0, 0.0, 100.0), LLVector3d(1.0, 0.0, 0.0), hit)); // level
        CHECK(!groundHit(LLVector3d(0.0, 0.0, 100.0), LLVector3d(0.0, 0.6, 0.8), hit)); // up
        CHECK(!groundHit(LLVector3d(0.0, 0.0, GROUND_Z - 5.0), LLVector3d(0.0, 0.0, -1.0), hit)); // below the plane looking down
        CHECK(!panOffset(LLVector3d(0.0, 0.0, 100.0), LLVector3d(1.0, 0.0, 0.0), LLVector3d(), offset)); // no pan
    }

    // Straight down, the level under the camera rises with height (Python reference: 1, 1, 2, 6).
    TEST_CASE("skworldmapmath: Straight down, the level under the camera rises with height (Python reference: 1, 1, 2, 6)", "[skworldmapmath]")
    {
        const std::pair<F64, S32> expect[] = { { 300.0, 1 }, { 1000.0, 1 }, { 5000.0, 2 }, { 50000.0, 6 } };
        const std::pair<F64, size_t> counts[] = { { 300.0, 3 }, { 1000.0, 35 }, { 5000.0, 85 }, { 50000.0, 60 } };
        for (size_t i = 0; i < 4; ++i)
        {
            const LLVector3d eye(256128.0, 256128.0, GROUND_Z + expect[i].first);
            const std::vector<SelectedTile> tiles = selectTiles(viewAt(eye, 0.0, -90.0 * DEG + 1.0e-6));
            const SelectedTile* under = tileUnder(tiles, eye.mdV[VX], eye.mdV[VY]);
            REQUIRE(under != nullptr); // a tile under the camera
            CHECK(under->mKey.mLevel == expect[i].second); // level under camera
            CHECK(tiles.size() == counts[i].second); // tile count
        }
    }

    // A tilted view: levels coarsen with distance, the tiles never overlap, and the counts match the reference.
    TEST_CASE("skworldmapmath: A tilted view: levels coarsen with distance, the tiles never overlap, and the counts match the reference", "[skworldmapmath]")
    {
        const std::vector<SelectedTile> tiles = selectTiles(viewAt(LLVector3d(256000.0, 256000.0, GROUND_Z + 800.0), 90.0 * DEG, -25.0 * DEG));
        CHECK(tiles.size() == (size_t)268); // total
        std::map<S32, size_t> per_level;
        std::map<S32, F64> nearest;
        std::set<std::pair<S32, S32>> cells;
        for (const SelectedTile& t : tiles)
        {
            ++per_level[t.mKey.mLevel];
            auto it = nearest.find(t.mKey.mLevel);
            nearest[t.mKey.mLevel] = it == nearest.end() ? t.mDistance : std::min(it->second, t.mDistance);
            const S32 n = t.mKey.regionsPerSide();
            for (S32 i = 0; i < n; ++i)
            {
                for (S32 j = 0; j < n; ++j)
                {
                    CHECK(cells.insert({ t.mKey.mGridX + i, t.mKey.mGridY + j }).second); // no overlap
                }
            }
        }
        CHECK(per_level[1] == (size_t)84); // level 1
        CHECK(per_level[2] == (size_t)74); // level 2
        CHECK(per_level[3] == (size_t)66); // level 3
        CHECK(per_level[4] == (size_t)44); // level 4
        CHECK((nearest[1] < nearest[2] && nearest[2] < nearest[3] && nearest[3] < nearest[4])); // coarser is farther
    }

    // The cap holds, and splitting stops before it rather than past it.
    TEST_CASE("skworldmapmath: The cap holds, and splitting stops before it rather than past it", "[skworldmapmath]")
    {
        TileSelectParams p;
        p.mCap = 40;
        const std::vector<SelectedTile> tiles = selectTiles(viewAt(LLVector3d(256000.0, 256000.0, GROUND_Z + 800.0), 90.0 * DEG, -25.0 * DEG), p);
        CHECK((tiles.size() <= 40 && tiles.size() > 30)); // capped
    }

    // An ancestor and the part of its texture a finer tile covers.
    TEST_CASE("skworldmapmath: An ancestor and the part of its texture a finer tile covers", "[skworldmapmath]")
    {
        F32 uv[4];
        const TileKey a = ancestor({ 1, 1001, 1003 }, 2, uv);
        CHECK((a == TileKey{ 3, 1000, 1000 })); // key
        CHECK((uv[0] == 0.25f && uv[1] == 0.75f && uv[2] == 0.5f && uv[3] == 1.0f)); // uv
    }

    // Stacks grow up the screen from straight above, along world up when tilted, and keep to the budget.
    TEST_CASE("skworldmapmath: Stacks grow up the screen from straight above, along world up when tilted, and keep to the budget", "[skworldmapmath]")
    {
        const LLVector2 top = stackDirection(LLVector3d(0.0, 0.0, 1.0));
        CHECK((top.mV[VX] == 0.f && top.mV[VY] == 1.f)); // top-down is screen up
        const LLVector2 side = stackDirection(LLVector3d(0.6, 0.0, 0.8));
        CHECK(side.mV[VX] > 0.99f); // tilted follows world up
        const std::vector<S32> shown = stackBudget({ 5.0, 1.0, 9.0 }, { 3, 10, 4 }, 8, 12);
        CHECK((shown[0] == 3 && shown[1] == 8 && shown[2] == 1)); // nearest first within budget
    }

} // namespace
