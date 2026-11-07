/**
 * @file skworldmapterrain.h
 * @brief The 3D world map's ground heights: the terrain of the regions this viewer is connected to,
 *        sampled into a grid per region, with the water as its floor.
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
#include "v3dmath.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SKWorldMapRoadSource;

/// Ground heights per region: the connected regions' live terrain, else a packed file of the dataset's
/// (SKWorldMapTerrainPack, a debug setting until the terrain cache serves it);
/// the terrain cache will replace the pack behind the same lookups.
class SKWorldMapTerrain
{
public:
    static constexpr S32 SAMPLES = 33;          ///< per side, corners included: 8 m apart on a 256 m region
    static constexpr F64 REFRESH_SECONDS = 10.0;

    struct Region
    {
        LLVector3d mOrigin;         ///< south-west corner, global
        F64 mWidth = 256.0;         ///< metres on a side
        std::vector<F32> mHeights;  ///< SAMPLES x SAMPLES, row 0 south, max(land, water)
        F64 mBuilt = 0.0;
        const void* mRoads = nullptr; ///< the roads it was lowered under, so their arrival rebuilds it
        bool mLive = false;         ///< from a connected region's live terrain, rather than the pack

        F32 at(S32 i, S32 j) const { return mHeights[j * SAMPLES + i]; }
    };

    /// Samples the connected regions, at most one rebuild a frame, and forgets regions no longer connected.
    /// Ground under a road is lowered to just below it, so a road is never hidden by the 8 m grid.
    void update(F64 now, const SKWorldMapRoadSource* roads);

    const std::unordered_map<U64, Region>& regions() const { return mRegions; }

    /// The ground at a global point: the region's grid where there is one, else `fallback`.
    F64 groundAt(const LLVector3d& p, F64 fallback) const;

    void clear()
    {
        mRegions.clear();
        mPacked.clear();
        mPackPath.clear();
    }

private:
    static void lowerUnderRoads(Region& r, U64 handle, const SKWorldMapRoadSource* roads);
    static F64 interpolate(const Region& r, F64 u, F64 v);
    void loadPack(const std::string& path);
    void updatePack(const SKWorldMapRoadSource* roads, const std::unordered_set<U64>& live);

    std::unordered_map<U64, std::vector<F32>> mPacked;  ///< SKWorldMapTerrainPack's regions, as read
    std::string mPackPath;

    std::unordered_map<U64, Region> mRegions;
};
