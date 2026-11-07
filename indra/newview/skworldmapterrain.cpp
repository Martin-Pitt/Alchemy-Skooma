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

#include <algorithm>
#include <unordered_set>

void SKWorldMapTerrain::update(F64 now)
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
        const F64 built = it == mRegions.end() ? -1.0 : it->second.mBuilt;
        if (built < stalest_built)
        {
            stalest = region;
            stalest_built = built;
        }
    }
    for (auto it = mRegions.begin(); it != mRegions.end();)
    {
        it = live.count(it->first) ? std::next(it) : mRegions.erase(it);
    }
    if (!stalest)
    {
        return;
    }

    Region& r = mRegions[stalest->getHandle()];
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
    r.mBuilt = now;
}

F64 SKWorldMapTerrain::groundAt(const LLVector3d& p, F64 fallback) const
{
    for (const auto& [handle, r] : mRegions)
    {
        const F64 u = (p.mdV[VX] - r.mOrigin.mdV[VX]) / r.mWidth;
        const F64 v = (p.mdV[VY] - r.mOrigin.mdV[VY]) / r.mWidth;
        if (u < 0.0 || v < 0.0 || u > 1.0 || v > 1.0 || r.mHeights.empty())
        {
            continue;
        }
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
    return fallback;
}
