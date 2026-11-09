/**
 * @file skworldmaphubtable_test.cpp
 * @brief The 3D world map's hub list reader, on markup shaped like the hub list page's.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#include "../skworldmaphubtable.h"

using namespace skworldmap;

namespace
{
    // The page's own layout: a heading row, then every hub on one line of rows, and an empty footer row.
    constexpr std::string_view PAGE = R"(<html><body>
<table id="data-table" class="data-table" translate="no">
    <thead>
        <tr>
            <th>Area</th>
            <th class="skip-filter">Region</th>
            <th class="skip-filter">Hub Name</th>
            <th>Air</th><th>Heli</th><th>Water</th><th>Seaplane</th><th>Road</th><th>Rail</th><th>Space</th>
        </tr>
    </thead>
    <tbody id="myTable">
        <tr><td>Satori &amp; North</td><td>Airtol Hill</td><td>Angels N1 AP MA Space</td><td>L</td><td>L</td><td>-</td><td>L</td><td>Y</td><td>-</td><td>L</td></tr><tr><td>Satori & North</td><td>Ahmadi</td><td>DZ Ahmadi</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td></tr><tr><td>Belli &amp; South</td><td>Acrocorinth</td><td>Half moon  Truck Stop</td><td>-</td><td>-</td><td>M</td><td>S</td><td>Y</td><td>Y</td><td>-</td></tr>
    </tbody>
    <tfoot>
        <tr>
        </tr>
    </tfoot>
</table>
<footer>Copyright &copy; 2026</footer>
</body></html>)";
}

TEST_CASE("skworldmaphubtable: reads the page's rows by their headings")
{
    const HubTable table = HubTable::parse(PAGE);
    REQUIRE(table.hubs().size() == 3);

    const Hub& angels = table.hubs()[0];
    CHECK(angels.mArea == "Satori & North");
    CHECK(angels.mRegion == "Airtol Hill");
    CHECK(angels.mName == "Angels N1 AP MA Space");
    CHECK(angels.size(HubService::Air) == HubSize::Large);
    CHECK(angels.size(HubService::Water) == HubSize::None);
    CHECK(angels.size(HubService::Road) == HubSize::Medium);
    CHECK(angels.size(HubService::Space) == HubSize::Large);

    const Hub& truck = table.hubs()[2];
    CHECK(truck.mName == "Half moon Truck Stop");
    CHECK(truck.size(HubService::Water) == HubSize::Medium);
    CHECK(truck.size(HubService::Seaplane) == HubSize::Small);
    CHECK(truck.size(HubService::Rail) == HubSize::Medium);
}

TEST_CASE("skworldmaphubtable: a drop zone serves nothing")
{
    const HubTable table = HubTable::parse(PAGE);
    const Hub* dz = table.find("Ahmadi");
    REQUIRE(dz != nullptr);
    CHECK_FALSE(dz->servesAny());
    CHECK(table.find("Airtol Hill")->servesAny());
}

TEST_CASE("skworldmaphubtable: regions are found in any case")
{
    const HubTable table = HubTable::parse(PAGE);
    CHECK(table.find("airtol hill") == &table.hubs()[0]);
    CHECK(table.find("ACROCORINTH") == &table.hubs()[2]);
    CHECK(table.find("Nowhere") == nullptr);
    CHECK(table.find("") == nullptr);
}

TEST_CASE("skworldmaphubtable: columns follow their headings, not their order")
{
    constexpr std::string_view moved = R"(<table><tr><th>Rail</th><th>Hub Name</th><th>Region</th></tr>
<tr><td>Y</td><td>Station</td><td>Somewhere</td></tr></table>)";
    const HubTable table = HubTable::parse(moved);
    REQUIRE(table.hubs().size() == 1);
    CHECK(table.hubs()[0].mRegion == "Somewhere");
    CHECK(table.hubs()[0].mName == "Station");
    CHECK(table.hubs()[0].size(HubService::Rail) == HubSize::Medium);
    CHECK(table.hubs()[0].size(HubService::Air) == HubSize::None);
}

TEST_CASE("skworldmaphubtable: a page with no table it can read is empty")
{
    CHECK(HubTable::parse("").empty());
    CHECK(HubTable::parse("<html><body>Down for maintenance</body></html>").empty());
    // Rows with no Region heading are not hubs.
    CHECK(HubTable::parse("<table><tr><th>Name</th></tr><tr><td>X</td></tr></table>").empty());
}

TEST_CASE("skworldmaphubtable: a region listed twice keeps its first row")
{
    constexpr std::string_view twice = R"(<table><tr><th>Region</th><th>Hub Name</th></tr>
<tr><td>Dup</td><td>First</td></tr><tr><td>dup</td><td>Second</td></tr></table>)";
    const HubTable table = HubTable::parse(twice);
    REQUIRE(table.find("Dup") != nullptr);
    CHECK(table.find("Dup")->mName == "First");
}

TEST_CASE("skworldmaphubtable: numeric entities decode to UTF-8")
{
    constexpr std::string_view page = R"(<table><tr><th>Region</th><th>Hub Name</th></tr>
<tr><td>Caf&#233;</td><td>A&#x2019;s</td></tr></table>)";
    const HubTable table = HubTable::parse(page);
    REQUIRE(table.hubs().size() == 1);
    CHECK(table.hubs()[0].mRegion == "Caf\xC3\xA9");
    CHECK(table.hubs()[0].mName == "A\xE2\x80\x99s");
}
