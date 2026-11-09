/**
 * @file skworldmaphubs.h
 * @brief The 3D world map's freight hubs: the hub list fetched at most once a refresh period, kept on disk,
 *        and matched to the regions the world map knows.
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

#include "skworldmaphubtable.h"

#include <memory>
#include <vector>

/// Hubs are whole regions, so a hub is placed by its region's handle once the world map has the region's name.
class SKWorldMapHubs
{
public:
    struct Placed
    {
        U64 mHandle;
        U32 mHub;           ///< into table().hubs()
        F32 mX, mY;         ///< where in the region the hub stands, in metres: its place, else the middle
    };

    ~SKWorldMapHubs();

    /// Loads the list from disk on first use and fetches it again once it is older than the refresh period.
    void update(F64 now);

    const skworldmap::HubTable& table() const { return mTable; }

    /// The hubs in regions the world map knows; follows the world map as regions arrive.
    const std::vector<Placed>& placed();

private:
    void loadCache();
    void fetch(const std::string& url, F64 now);
    void adopt(skworldmap::HubTable table);
    void updatePlaces(F64 now);

    skworldmap::HubTable mTable;
    skworldmap::HubPlaces mPlaces;
    std::string mPlacesPath;            ///< the file mPlaces was read from
    S64 mPlacesWritten = -1;            ///< its modification time then; -1 for no file
    F64 mPlacesCheckAt = 0.0;           ///< frame time to look at the file again
    std::vector<Placed> mPlaced;
    size_t mPlacedRegions = 0;          ///< the world map's region count when mPlaced was made
    bool mPlacedStale = true;
    bool mLoaded = false;               ///< the disk copy has been read
    bool mFetching = false;
    F64 mFetchedAt = 0.0;               ///< when the list on disk was written, seconds since the epoch; 0 for never
    F64 mRetryAt = 0.0;                 ///< after a failed fetch, frame time to try again
    std::shared_ptr<SKWorldMapHubs*> mSelf;  ///< lets a fetch outlive the map and land on nothing
};
