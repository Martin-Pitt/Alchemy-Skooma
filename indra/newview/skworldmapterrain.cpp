/**
 * @file skworldmapterrain.cpp
 * @brief The 3D world map's ground heights.
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

#include "llviewerprecompiledheaders.h"

#include "skworldmapterrain.h"

#include "llsurface.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "skworldmaproads.h"
#include "llregionhandle.h"
#include "llviewercontrol.h"
#include "llfile.h"

#include <cstring>

#include <algorithm>
#include <cmath>
#include <unordered_set>

void SKWorldMapTerrain::update(F64 now, const SKWorldMapRoadSource* roads)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    std::unordered_set<U64> live;
    LLViewerRegion* stalest = nullptr;
    F64 stalest_built = now - REFRESH_SECONDS;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        const U64 handle = region->getHandle();
        live.insert(handle);
        const auto it = mRegions.find(handle);
        const void* road_key = roads ? static_cast<const void*>(roads->region(handle)) : nullptr;
        const F64 built = it == mRegions.end() || it->second.mRoads != road_key ? -1.0 : it->second.mBuilt;
        if (built < stalest_built)
        {
            stalest = region;
            stalest_built = built;
        }
    }
    // A region no longer connected falls back to the pack, if the pack has it.
    for (auto it = mRegions.begin(); it != mRegions.end();)
    {
        it = (it->second.mLive && !live.count(it->first)) ? mRegions.erase(it) : std::next(it);
    }
    updatePack(roads, live);
    if (!stalest)
    {
        return;
    }

    Region& r = mRegions[stalest->getHandle()];
    r.mLive = true;
    r.mOrigin = stalest->getOriginGlobal();
    r.mWidth = stalest->getWidth();
    r.mHeights.resize(SAMPLES * SAMPLES);
    const F32 water = stalest->getWaterHeight();
    const LLSurface& land = stalest->getLand();
    const F32 step = static_cast<F32>(r.mWidth) / (SAMPLES - 1);
    for (S32 j = 0; j < SAMPLES; ++j)
    {
        for (S32 i = 0; i < SAMPLES; ++i)
        {
            // Keep the last sample just inside, as the surface resolves its own edge.
            const F32 x = std::min(i * step, static_cast<F32>(r.mWidth) - 0.01f);
            const F32 y = std::min(j * step, static_cast<F32>(r.mWidth) - 0.01f);
            r.mHeights[j * SAMPLES + i] = std::max(land.resolveHeightRegion(x, y), water);
        }
    }
    lowerUnderRoads(r, stalest->getHandle(), roads);
    r.mRoads = roads ? static_cast<const void*>(roads->region(stalest->getHandle())) : nullptr;
    r.mBuilt = now;
}

void SKWorldMapTerrain::loadPack(const std::string& path)
{
    mPacked.clear();
    for (auto it = mRegions.begin(); it != mRegions.end();)
    {
        it = it->second.mLive ? std::next(it) : mRegions.erase(it);
    }
    if (path.empty())
    {
        return;
    }
    // The packer's format (V:\Scratch\worldmap\map\terrain-pack\pack.py): "SKT1", u32 count, u32 samples,
    // then per region i32 gx, i32 gy and samples^2 f32 heights, row 0 south, little-endian.
    llifstream in(path.c_str(), std::ios::binary);
    char magic[4] = {};
    U32 count = 0, samples = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&count), 4);
    in.read(reinterpret_cast<char*>(&samples), 4);
    if (!in || std::memcmp(magic, "SKT1", 4) != 0 || samples != SAMPLES)
    {
        LL_WARNS("SKWorldMap") << "terrain pack " << path << " is not a " << SAMPLES << "-sample SKT1 file" << LL_ENDL;
        return;
    }
    for (U32 k = 0; k < count; ++k)
    {
        S32 grid[2];
        std::vector<F32> heights(SAMPLES * SAMPLES);
        in.read(reinterpret_cast<char*>(grid), sizeof(grid));
        in.read(reinterpret_cast<char*>(heights.data()), heights.size() * sizeof(F32));
        if (!in || grid[0] < 0 || grid[1] < 0)
        {
            LL_WARNS("SKWorldMap") << "terrain pack " << path << " ends after " << k << " of " << count << " regions" << LL_ENDL;
            break;
        }
        mPacked[to_region_handle(static_cast<U32>(grid[0]) * 256, static_cast<U32>(grid[1]) * 256)] = std::move(heights);
    }
    LL_INFOS("SKWorldMap") << "terrain pack: " << mPacked.size() << " regions from " << path << LL_ENDL;
}

void SKWorldMapTerrain::updatePack(const SKWorldMapRoadSource* roads, const std::unordered_set<U64>& live)
{
    static LLCachedControl<std::string> pack_path(gSavedSettings, "SKWorldMapTerrainPack", "");
    if (pack_path() != mPackPath)
    {
        mPackPath = pack_path();
        loadPack(mPackPath);
    }
    // Packed regions are copied in, and lowered under their roads, a few a frame; again when roads arrive.
    constexpr S32 PER_FRAME = 32;
    S32 built = 0;
    for (const auto& [handle, heights] : mPacked)
    {
        if (live.count(handle))
        {
            continue;
        }
        const void* road_key = roads ? static_cast<const void*>(roads->region(handle)) : nullptr;
        const auto it = mRegions.find(handle);
        if (it != mRegions.end() && it->second.mRoads == road_key)
        {
            continue;
        }
        Region& r = mRegions[handle];
        U32 x, y;
        from_region_handle(handle, &x, &y);
        r.mOrigin.setVec(x, y, 0.0);
        r.mWidth = 256.0;
        r.mHeights = heights;
        r.mLive = false;
        lowerUnderRoads(r, handle, roads);
        r.mRoads = road_key;
        if (++built >= PER_FRAME)
        {
            break;
        }
    }
}

void SKWorldMapTerrain::lowerUnderRoads(Region& r, U64 handle, const SKWorldMapRoadSource* roads)
{
    if (!roads)
    {
        return;
    }
    // A sample within a grid step of a road's edge goes below the road, so no triangle of the grid
    // between samples rises through it. Segments are kept by their midpoint's region, so the
    // neighbours' are looked at too.
    constexpr F32 BELOW_ROAD = 0.5f;
    const F64 step = r.mWidth / (SAMPLES - 1);
    U32 rx, ry;
    from_region_handle(handle, &rx, &ry);
    for (S32 dy = -1; dy <= 1; ++dy)
    {
        for (S32 dx = -1; dx <= 1; ++dx)
        {
            const S64 nx = static_cast<S64>(rx) + dx * 256;
            const S64 ny = static_cast<S64>(ry) + dy * 256;
            if (nx < 0 || ny < 0)
            {
                continue;
            }
            const skworldmap::RoadRegion* roads_here = roads->region(to_region_handle(static_cast<U32>(nx), static_cast<U32>(ny)));
            if (!roads_here)
            {
                continue;
            }
            for (const skworldmap::RoadSegment& s : roads_here->mSegments)
            {
                const F64 reach = s.mWidth * 0.5 + step;
                const F64 ax = s.mA.mdV[VX] - r.mOrigin.mdV[VX], ay = s.mA.mdV[VY] - r.mOrigin.mdV[VY];
                const F64 bx = s.mB.mdV[VX] - r.mOrigin.mdV[VX], by = s.mB.mdV[VY] - r.mOrigin.mdV[VY];
                const S32 i0 = std::max(0, static_cast<S32>(std::floor((std::min(ax, bx) - reach) / step)));
                const S32 i1 = std::min(SAMPLES - 1, static_cast<S32>(std::ceil((std::max(ax, bx) + reach) / step)));
                const S32 j0 = std::max(0, static_cast<S32>(std::floor((std::min(ay, by) - reach) / step)));
                const S32 j1 = std::min(SAMPLES - 1, static_cast<S32>(std::ceil((std::max(ay, by) + reach) / step)));
                const F64 ex = bx - ax, ey = by - ay;
                const F64 len2 = std::max(ex * ex + ey * ey, 1.0e-6);
                for (S32 j = j0; j <= j1; ++j)
                {
                    for (S32 i = i0; i <= i1; ++i)
                    {
                        const F64 px = i * step - ax, py = j * step - ay;
                        const F64 t = std::clamp((px * ex + py * ey) / len2, 0.0, 1.0);
                        const F64 qx = px - t * ex, qy = py - t * ey;
                        if (qx * qx + qy * qy > reach * reach)
                        {
                            continue;
                        }
                        const F32 road_z = static_cast<F32>(s.mA.mdV[VZ] + t * (s.mB.mdV[VZ] - s.mA.mdV[VZ]));
                        F32& h = r.mHeights[j * SAMPLES + i];
                        h = std::min(h, road_z - BELOW_ROAD);
                    }
                }
            }
        }
    }
}

F64 SKWorldMapTerrain::groundAt(const LLVector3d& p, F64 fallback) const
{
    // The 256 m cell's own region first; only a larger live region can cover a point from another cell.
    const auto sample = [&](const Region& r, F64& out) {
        const F64 u = (p.mdV[VX] - r.mOrigin.mdV[VX]) / r.mWidth;
        const F64 v = (p.mdV[VY] - r.mOrigin.mdV[VY]) / r.mWidth;
        if (u < 0.0 || v < 0.0 || u > 1.0 || v > 1.0 || r.mHeights.empty())
        {
            return false;
        }
        out = interpolate(r, u, v);
        return true;
    };
    F64 out = fallback;
    if (p.mdV[VX] >= 0.0 && p.mdV[VY] >= 0.0)
    {
        const U32 x = static_cast<U32>(p.mdV[VX] / 256.0) * 256;
        const U32 y = static_cast<U32>(p.mdV[VY] / 256.0) * 256;
        const auto it = mRegions.find(to_region_handle(x, y));
        if (it != mRegions.end() && sample(it->second, out))
        {
            return out;
        }
    }
    for (const auto& [handle, r] : mRegions)
    {
        if (r.mLive && r.mWidth > 256.0 && sample(r, out))
        {
            return out;
        }
    }
    return fallback;
}

F64 SKWorldMapTerrain::interpolate(const Region& r, F64 u, F64 v)
{
    const F64 fx = u * (SAMPLES - 1);
    const F64 fy = v * (SAMPLES - 1);
    const S32 i = std::min(static_cast<S32>(fx), SAMPLES - 2);
    const S32 j = std::min(static_cast<S32>(fy), SAMPLES - 2);
    const F64 tx = fx - i;
    const F64 ty = fy - j;
    const F64 south = r.at(i, j) * (1.0 - tx) + r.at(i + 1, j) * tx;
    const F64 north = r.at(i, j + 1) * (1.0 - tx) + r.at(i + 1, j + 1) * tx;
    return south * (1.0 - ty) + north * ty;
}
