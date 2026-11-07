/**
 * @file skterraindataset_test.cpp
 * @brief The dataset's grey16 cells and index rows, made up and real.
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

#include "skterraindataset.h"
#include "skterrainpng.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace
{
    using namespace SKTerrain;

    SKTerrainLayerSamples ramp()
    {
        SKTerrainLayerSamples s;
        s.resize();
        for (U32 i = 0; i < CELL_SAMPLES; ++i)
        {
            s.mHeightCm[i] = -300 + S32(i % 9001);
            s.mValid[i]    = (i % 13) != 0;
        }
        return s;
    }

    TEST_CASE("skterraindataset: a grey16 cell keeps every bit and its row order", "[skterraindataset]")
    {
        const SKTerrainLayerSamples s = ramp();
        S32                         offset = 0;
        const auto                  image  = SKTerrainPng::fromSamples(s, offset);
        REQUIRE(image);
        CHECK(offset == -301);
        CHECK(image->mPixels[0] == 0);   // missing
        CHECK(image->mPixels[1] == 2);   // -299 cm
        const std::vector<U8> png = SKTerrainPng::encode(*image);
        REQUIRE(!png.empty());
        // Network byte order: the first row's second sample, 2, is 00 02.
        const auto decoded = SKTerrainPng::decode(png.data(), png.size());
        REQUIRE(decoded);
        CHECK(decoded->mPixels == image->mPixels);
        const auto back = SKTerrainPng::toSamples(*decoded, offset);
        REQUIRE(back);
        CHECK(back->mValid == s.mValid);
        for (U32 i = 0; i < CELL_SAMPLES; ++i)
        {
            if (s.mValid[i] && back->mHeightCm[i] != s.mHeightCm[i])
            {
                FAIL("sample " << i << " came back " << back->mHeightCm[i]);
            }
        }
    }

    TEST_CASE("skterraindataset: a range the format cannot hold is refused", "[skterraindataset]")
    {
        SKTerrainLayerSamples s;
        s.resize();
        s.mValid[0]        = 1;
        s.mValid[1]        = 1;
        s.mHeightCm[1]     = 65535;
        S32 offset         = 0;
        CHECK_FALSE(SKTerrainPng::fromSamples(s, offset));
        CHECK_FALSE(SKTerrainPng::fromSamples(SKTerrainLayerSamples(), offset));
        const U8 junk[16] = {};
        CHECK_FALSE(SKTerrainPng::decode(junk, sizeof(junk)));
    }

    TEST_CASE("skterraindataset: index rows read as the schema writes them", "[skterraindataset]")
    {
        const std::string json =
            R"([{"grid":[994,1025],"height":"terrain/994_1025.png","offset_cm":67,"origin_m":[254464,262400],"size":[256,256],"step_m":1,"valid_samples":65536,"water_m":20.0},)"
            R"({"grid":[1008,901],"height":null,"offset_cm":null,"origin_m":[258048,230656],"size":[256,256],"step_m":1,"valid_samples":0,"water_m":null},)"
            R"({"grid":[1,2],"height":"terrain/1_2.png","offset_cm":0,"size":[512,256],"step_m":1,"valid_samples":1}])";
        const std::vector<SKTerrainDatasetRow> rows = SKTerrainDataset::parseIndex(json);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].mGridX == 994);
        CHECK(rows[0].mGridY == 1025);
        CHECK(rows[0].mHeight == "terrain/994_1025.png");
        CHECK(rows[0].mOffsetCm == 67);
        CHECK(rows[0].mWater == 20.f);
        CHECK(rows[0].handle() == handleFromGrid(994, 1025));
        CHECK(rows[1].mHeight.empty());
        CHECK_FALSE(rows[1].mWater);

        const std::vector<SKTerrainDatasetRow> again = SKTerrainDataset::parseIndex(SKTerrainDataset::formatIndex(rows));
        REQUIRE(again.size() == 2);
        CHECK(again[0].mOffsetCm == 67);
        CHECK(again[0].mWater == 20.f);
        CHECK(again[1].mValidSamples == 0);
    }

    // Two real cells of a dataset release, where SK_TERRAIN_DATASET_DIR names one.
    TEST_CASE("skterraindataset: real dataset cells read and write back the same", "[skterraindataset]")
    {
        const char* dir = std::getenv("SK_TERRAIN_DATASET_DIR");
        if (!dir || !*dir)
            SKIP("SK_TERRAIN_DATASET_DIR is not set");
        const std::filesystem::path root(dir);
        std::ifstream               in(root / "terrain" / "index.json", std::ios::binary);
        REQUIRE(in);
        const std::string                      json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::vector<SKTerrainDatasetRow> rows = SKTerrainDataset::parseIndex(json);
        REQUIRE(!rows.empty());

        int checked = 0;
        for (const SKTerrainDatasetRow& row : rows)
        {
            if (row.mHeight.empty() || (row.mGridX != 994 && row.mGridX != 1002) || checked == 2)
                continue;
            INFO("cell " << row.mGridX << "_" << row.mGridY);
            const auto samples = SKTerrainDataset::readCell(root, row);
            REQUIRE(samples);
            CHECK(samples->validCount() == row.mValidSamples);

            // The file of the cache keeps it to the centimetre, and the PNG written back is the same picture.
            SKTerrainCell cell;
            cell.mHandle = row.handle();
            cell.mWater  = row.mWater.value_or(20.f);
            cell.setLayer(LAYER_TERRAIN, *samples, 1);
            const std::vector<U8> bytes = cell.encode();
            SKTerrainCell         back;
            REQUIRE(back.decode(bytes.data(), bytes.size()) == SKTerrainCell::DECODE_OK);
            CHECK(back.mLayers[LAYER_TERRAIN].mHeightCm == samples->mHeightCm);

            S32        offset = 0;
            const auto image  = SKTerrainPng::fromSamples(*samples, offset);
            REQUIRE(image);
            CHECK(offset == *row.mOffsetCm);
            std::ifstream png_in(root / std::filesystem::path(row.mHeight), std::ios::binary);
            const std::vector<U8> original((std::istreambuf_iterator<char>(png_in)), std::istreambuf_iterator<char>());
            const auto            decoded = SKTerrainPng::decode(original.data(), original.size());
            REQUIRE(decoded);
            CHECK(decoded->mPixels == image->mPixels);
            ++checked;
        }
        CHECK(checked == 2);
    }
} // namespace
