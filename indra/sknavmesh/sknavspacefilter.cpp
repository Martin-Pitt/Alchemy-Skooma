/**
 * @file sknavspacefilter.cpp
 * @brief A Detour query filter that prices and bars polys by the space they were stamped with.
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

#include "sknavspacefilter.h"

#include "DetourNavMesh.h"

#include <algorithm>

namespace sknav
{
    void SpaceQueryFilter::setSpaceCost(U32 space, F32 cost)
    {
        const F32 kept = cost <= 0.f ? 0.f : std::max(cost, 1.f);
        for (auto& c : mCosts)
        {
            if (c.first == space)
            {
                c.second = kept;
                return;
            }
        }
        mCosts.emplace_back(space, kept);
    }

    F32 SpaceQueryFilter::spaceCost(U32 space) const
    {
        for (const auto& c : mCosts)
        {
            if (c.first == space)
            {
                return c.second;
            }
        }
        return 1.f;
    }

    U32 SpaceQueryFilter::spaceOf(const dtMeshTile* tile, const dtPoly* poly) const
    {
        if (!tile || !poly || mCosts.empty())
        {
            return 0;
        }
        const U32 index = static_cast<U32>(poly - tile->polys);
        if (mSpacesOf)
        {
            // Tiles sit in one array, so their address over their size spreads them across the cache.
            CachedTile& cached = mTiles[(reinterpret_cast<uintptr_t>(tile) / sizeof(dtMeshTile)) % mTiles.size()];
            if (cached.mTile != tile)
            {
                cached.mTile = tile;
                cached.mSpaces = mSpacesOf(tile);
            }
            return index < cached.mSpaces.mCount ? cached.mSpaces.mSpaces[index] : 0;
        }
        return mSpaceOf ? mSpaceOf(tile, index) : 0;
    }

    bool SpaceQueryFilter::passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const
    {
        if (!dtQueryFilter::passFilter(ref, tile, poly))
        {
            return false;
        }
        const U32 space = spaceOf(tile, poly);
        return !space || spaceCost(space) > 0.f;
    }

    float SpaceQueryFilter::getCost(const float* pa, const float* pb, const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
                                    const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly, const dtPolyRef nextRef,
                                    const dtMeshTile* nextTile, const dtPoly* nextPoly) const
    {
        const float base = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef, nextTile, nextPoly);
        const U32 space = spaceOf(curTile, curPoly);
        return space ? base * spaceCost(space) : base;
    }
}
