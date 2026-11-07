/**
 * @file skterraincell_test.cpp
 * @brief The region file's round trip, the pyramid's decimation and its masks.
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

#include "skterraincell.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    using namespace SKTerrain;

    // Rolling hills with a cliff, heights not on whole centimetres.
    std::vector<F32> hills()
    {
        std::vector<F32> h(CELL_SAMPLES);
        for (U32 y = 0; y < CELL_SIZE; ++y)
            for (U32 x = 0; x < CELL_SIZE; ++x)
                h[y * CELL_SIZE + x] = 21.f + 9.f * std::sin(x * 0.05f) * std::cos(y * 0.031f) + (x > 200 ? 40.f : 0.f) + 0.0037f * x;
        return h;
    }

    TEST_CASE("skterraincell: a cell round-trips through its file to the centimetre", "[skterraincell]")
    {
        const std::vector<F32> metres = hills();
        std::vector<U8>        valid(CELL_SAMPLES, 1);
        for (U32 i = 0; i < CELL_SAMPLES; i += 7)
            valid[i] = 0;

        SKTerrainCell cell;
        cell.mHandle      = handleFromGrid(1000, 1035);
        cell.mWater       = 19.5f;
        cell.mCaptureDate = 1793900000;
        cell.mSidecar     = "look";
        cell.setLayer(LAYER_TERRAIN, SKTerrainLayerSamples::fromMetres(metres.data(), valid.data()), 42);

        const std::vector<U8> bytes = cell.encode();
        REQUIRE(!bytes.empty());

        SKTerrainCell back;
        REQUIRE(back.decode(bytes.data(), bytes.size()) == SKTerrainCell::DECODE_OK);
        CHECK(back.mHandle == cell.mHandle);
        CHECK(back.mWater == cell.mWater);
        CHECK(back.mCaptureDate == cell.mCaptureDate);
        CHECK(back.mSidecar == "look");
        CHECK(back.mInfo[LAYER_TERRAIN].mRevision == 1);
        CHECK(back.mInfo[LAYER_TERRAIN].mStamp == 42);
        CHECK_FALSE(back.hasLayer(LAYER_SURFACE));
        REQUIRE(back.hasLayer(LAYER_TERRAIN));
        CHECK(back.mLayers[LAYER_TERRAIN].mValid == cell.mLayers[LAYER_TERRAIN].mValid);
        CHECK(back.mLayers[LAYER_TERRAIN].mHeightCm == cell.mLayers[LAYER_TERRAIN].mHeightCm);

        for (U32 i = 1; i < CELL_SAMPLES; i += 977)
        {
            if (valid[i])
                CHECK(std::fabs(back.mLayers[LAYER_TERRAIN].mHeightCm[i] / 100.f - metres[i]) <= 0.0051f);
        }
    }

    TEST_CASE("skterraincell: a range past what sixteen bits hold round-trips", "[skterraincell]")
    {
        std::vector<F32> metres(CELL_SAMPLES, -50.f);
        metres[12345] = 3000.f;
        SKTerrainCell cell;
        cell.setLayer(LAYER_SURFACE, SKTerrainLayerSamples::fromMetres(metres.data(), nullptr), 1);
        const std::vector<U8> bytes = cell.encode();
        SKTerrainCell         back;
        REQUIRE(back.decode(bytes.data(), bytes.size()) == SKTerrainCell::DECODE_OK);
        CHECK(back.mLayers[LAYER_SURFACE].mHeightCm == cell.mLayers[LAYER_SURFACE].mHeightCm);
        CHECK(back.mInfo[LAYER_SURFACE].mMax == 3000.f);
        CHECK(back.mInfo[LAYER_SURFACE].mMin == -50.f);
    }

    TEST_CASE("skterraincell: a damaged or foreign file is refused", "[skterraincell]")
    {
        const std::vector<F32> metres = hills();
        SKTerrainCell          cell;
        cell.setLayer(LAYER_TERRAIN, SKTerrainLayerSamples::fromMetres(metres.data(), nullptr), 1);
        std::vector<U8> bytes = cell.encode();
        SKTerrainCell   back;

        std::vector<U8> flipped = bytes;
        flipped[flipped.size() / 2] ^= 0x10;
        CHECK(back.decode(flipped.data(), flipped.size()) == SKTerrainCell::DECODE_CORRUPT);
        CHECK(back.decode(bytes.data(), bytes.size() - 9) == SKTerrainCell::DECODE_CORRUPT);
        CHECK(back.decode(bytes.data(), 3) == SKTerrainCell::DECODE_CORRUPT);

        std::vector<U8> other = bytes;
        other[4]              = U8(SKTerrainCell::FORMAT_VERSION + 1);
        CHECK(back.decode(other.data(), other.size()) == SKTerrainCell::DECODE_VERSION);

        SKTerrainCell header;
        CHECK(SKTerrainCell::peek(bytes.data(), bytes.size(), header) == SKTerrainCell::DECODE_OK);
        CHECK(header.mInfo[LAYER_TERRAIN].mValid == CELL_SAMPLES);
        CHECK_FALSE(header.hasLayer(LAYER_TERRAIN));
    }

    TEST_CASE("skterraincell: setting one layer leaves the other's revision alone", "[skterraincell]")
    {
        const std::vector<F32> metres = hills();
        SKTerrainCell          cell;
        cell.setLayer(LAYER_TERRAIN, SKTerrainLayerSamples::fromMetres(metres.data(), nullptr), 1);
        cell.setLayer(LAYER_SURFACE, SKTerrainLayerSamples::fromMetres(metres.data(), nullptr), 2);
        cell.setLayer(LAYER_SURFACE, SKTerrainLayerSamples::fromMetres(metres.data(), nullptr), 3);
        CHECK(cell.mInfo[LAYER_TERRAIN].mRevision == 1);
        CHECK(cell.mInfo[LAYER_SURFACE].mRevision == 2);
        CHECK(cell.mInfo[LAYER_SURFACE].mStamp == 3);
    }

    TEST_CASE("skterraincell: each pyramid level keeps the lattice points of the base", "[skterraincell]")
    {
        const std::vector<F32>      metres  = hills();
        const SKTerrainLayerSamples samples = SKTerrainLayerSamples::fromMetres(metres.data(), nullptr);
        const SKTerrainPyramid      pyramid = SKTerrainPyramid::build(samples, 20.f);

        for (U32 k = 0; k < LEVEL_COUNT; ++k)
        {
            const SKTerrainPyramid::Level& level = pyramid.mLevels[k];
            REQUIRE(level.mSize == CELL_SIZE >> k);
            const U32 step = 1u << k;
            for (U32 j = 0; j < level.mSize; j += 3)
            {
                for (U32 i = 0; i < level.mSize; i += 5)
                {
                    const U32 base = j * step * CELL_SIZE + i * step;
                    CHECK(level.mHeight[j * level.mSize + i] == samples.mHeightCm[base] / 100.f);
                }
            }
        }
        CHECK(pyramid.mLevels[LEVEL_COUNT - 1].mHeight[0] == samples.mHeightCm[0] / 100.f);
    }

    TEST_CASE("skterraincell: a node's min and max are over the known samples of its block", "[skterraincell]")
    {
        const std::vector<F32> metres = hills();
        std::vector<U8>        valid(CELL_SAMPLES, 1);
        // The highest sample, at the cliff, is missing; it must not show in any node's max.
        U32 top = 0;
        for (U32 i = 0; i < CELL_SAMPLES; ++i)
            if (metres[i] > metres[top])
                top = i;
        valid[top] = 0;
        // A whole 16 m block missing.
        for (U32 y = 32; y < 48; ++y)
            for (U32 x = 64; x < 80; ++x)
                valid[y * CELL_SIZE + x] = 0;

        const SKTerrainLayerSamples samples = SKTerrainLayerSamples::fromMetres(metres.data(), valid.data());
        const SKTerrainPyramid      pyramid = SKTerrainPyramid::build(samples, 20.f);

        F32 lo = 1e9f, hi = -1e9f;
        for (U32 i = 0; i < CELL_SAMPLES; ++i)
        {
            if (samples.mValid[i])
            {
                lo = std::min(lo, samples.mHeightCm[i] / 100.f);
                hi = std::max(hi, samples.mHeightCm[i] / 100.f);
            }
        }
        CHECK(pyramid.mMin == lo);
        CHECK(pyramid.mMax == hi);

        const SKTerrainPyramid::Level& level4 = pyramid.mLevels[4]; // 16 m nodes
        const U32                      hole   = 2 * level4.mSize + 4;
        CHECK(level4.mMin[hole] > level4.mMax[hole]);
        CHECK_FALSE(level4.mValid[hole]);
        CHECK(level4.mHeight[hole] == 20.f);
        CHECK(pyramid.mLevels[0].mHeight[top] == 20.f);
    }

    TEST_CASE("skterraincell: an empty layer's pyramid is all fill and has no range", "[skterraincell]")
    {
        const SKTerrainPyramid pyramid = SKTerrainPyramid::build(SKTerrainLayerSamples(), 18.f);
        CHECK_FALSE(pyramid.hasData());
        CHECK(pyramid.mLevels[0].mHeight[777] == 18.f);
        CHECK(pyramid.mLevels[8].mValid[0] == 0);
    }
} // namespace
