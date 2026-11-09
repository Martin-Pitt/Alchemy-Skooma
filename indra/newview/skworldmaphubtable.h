/**
 * @file skworldmaphubtable.h
 * @brief The 3D world map's freight hubs: whole regions that serve some ways of travel, read from a hub list page.
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

#pragma once

#include "stdtypes.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skworldmap
{
    /// The ways of travel a hub serves, in the order the hub list's columns give them.
    enum class HubService : U8
    {
        Air,
        Heli,
        Water,
        Seaplane,
        Road,
        Rail,
        Space,
        Count
    };
    constexpr size_t HUB_SERVICES = static_cast<size_t>(HubService::Count);

    /// How big a craft a hub takes for one service: none, or small, medium, large. A service with no sizes
    /// (road, rail) reads as medium.
    enum class HubSize : U8
    {
        None,
        Small,
        Medium,
        Large
    };

    /// One hub: a whole region, as the hub list names it.
    struct Hub
    {
        std::string mRegion;    ///< as the list spells it
        std::string mName;
        std::string mArea;
        std::array<HubSize, HUB_SERVICES> mSizes{};

        HubSize size(HubService s) const { return mSizes[static_cast<size_t>(s)]; }
        bool servesAny() const;
    };

    /// The hubs and a case-insensitive index from region name, as region names are matched on the grid.
    class HubTable
    {
    public:
        /// Reads the hub list page's table, its columns found by their headings. Rows it cannot read are skipped;
        /// a page with no table it can read gives an empty table.
        static HubTable parse(std::string_view html);

        const std::vector<Hub>& hubs() const { return mHubs; }
        bool empty() const { return mHubs.empty(); }

        /// The hub in a region, or null.
        const Hub* find(std::string_view region) const;

    private:
        std::vector<Hub> mHubs;
        std::vector<std::pair<std::string, U32>> mIndex;   ///< lower-cased region name, hub; sorted
    };
} // namespace skworldmap
