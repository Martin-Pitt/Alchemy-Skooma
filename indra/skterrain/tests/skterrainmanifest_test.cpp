/**
 * @file skterrainmanifest_test.cpp
 * @brief The cache's index: its rows round-trip, and anything doubtful is rebuilt.
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

#include "skterrainmanifest.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    using namespace SKTerrain;

    TEST_CASE("skterrainmanifest: rows round-trip through the index text", "[skterrainmanifest]")
    {
        std::vector<F32> flat(CELL_SAMPLES, 23.45f);
        SKTerrainCell    cell;
        cell.mHandle      = handleFromGrid(1002, 1034);
        cell.mWater       = 21.5f;
        cell.mCaptureDate = 1793900000;
        cell.setLayer(LAYER_TERRAIN, SKTerrainLayerSamples::fromMetres(flat.data(), nullptr), 7);
        const std::vector<U8> bytes = cell.encode();

        SKTerrainCell header;
        REQUIRE(SKTerrainCell::peek(bytes.data(), bytes.size(), header) == SKTerrainCell::DECODE_OK);
        SKTerrainManifestRow row = SKTerrainManifestRow::fromHeader(header, bytes.size(), SKTerrainManifest::checksumOf(bytes.data(), bytes.size()));
        row.mLastSeen            = 1793900100;
        row.mSource              = "live";

        SKTerrainManifest manifest;
        manifest.mGrid = "agni";
        manifest.mRows.push_back(row);
        row.mHandle = handleFromGrid(1, 2);
        row.mSource = "census\tsurface"; // a tab may not split a row
        manifest.mRows.push_back(row);

        const auto back = SKTerrainManifest::parse(manifest.format(), "agni");
        REQUIRE(back);
        REQUIRE(back->mRows.size() == 2);
        const SKTerrainManifestRow& r = back->mRows[0];
        CHECK(r.mHandle == cell.mHandle);
        CHECK(r.hasLayer(LAYER_TERRAIN));
        CHECK_FALSE(r.hasLayer(LAYER_SURFACE));
        CHECK(r.mRevision[LAYER_TERRAIN] == 1);
        // The cell keeps whole centimetres, so its range is 2345 cm as an F32, an ULP under 23.45f; the row carries it exactly.
        CHECK(r.mMin[LAYER_TERRAIN] == header.mInfo[LAYER_TERRAIN].mMin);
        CHECK(r.mMax[LAYER_TERRAIN] == header.mInfo[LAYER_TERRAIN].mMax);
        CHECK(std::abs(r.mMin[LAYER_TERRAIN] - 23.45f) < 0.005f);
        CHECK(r.mWater == 21.5f);
        CHECK(r.mCaptureDate == 1793900000);
        CHECK(r.mLastSeen == 1793900100);
        CHECK(r.mSource == "live");
        CHECK(r.mFileSize == bytes.size());
        CHECK(r.mChecksum == SKTerrainManifest::checksumOf(bytes.data(), bytes.size()));
        CHECK(back->mRows[1].mSource == "census surface");
    }

    TEST_CASE("skterrainmanifest: another grid, version or a damaged row asks for a rebuild", "[skterrainmanifest]")
    {
        SKTerrainManifest manifest;
        manifest.mGrid = "agni";
        manifest.mRows.emplace_back();
        const std::string text = manifest.format();
        CHECK(SKTerrainManifest::parse(text, "agni"));
        CHECK_FALSE(SKTerrainManifest::parse(text, "aditi"));
        CHECK_FALSE(SKTerrainManifest::parse("SKTerrainIndex\t999\tagni\n", "agni"));
        CHECK_FALSE(SKTerrainManifest::parse(text + "3e8\t1\n", "agni"));
        CHECK_FALSE(SKTerrainManifest::parse("", "agni"));
    }

    TEST_CASE("skterrainmanifest: file names name the grid cell", "[skterrainmanifest]")
    {
        const U64 handle = handleFromGrid(1000, 992);
        CHECK(SKTerrainManifest::fileName(handle) == "1000_992.skt");
        CHECK(SKTerrainManifest::handleFromFileName("1000_992.skt") == handle);
        CHECK_FALSE(SKTerrainManifest::handleFromFileName("1000_992.skt.tmp"));
        CHECK_FALSE(SKTerrainManifest::handleFromFileName("01000_992.skt"));
        CHECK_FALSE(SKTerrainManifest::handleFromFileName("index"));
    }
} // namespace
