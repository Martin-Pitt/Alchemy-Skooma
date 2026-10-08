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
        mCosts[space] = cost <= 0.f ? 0.f : std::max(cost, 1.f);
    }

    F32 SpaceQueryFilter::spaceCost(U32 space) const
    {
        const auto it = mCosts.find(space);
        return it == mCosts.end() ? 1.f : it->second;
    }

    U32 SpaceQueryFilter::spaceOf(const dtMeshTile* tile, const dtPoly* poly) const
    {
        if (!mSpaceOf || !tile || !poly || mCosts.empty())
        {
            return 0;
        }
        return mSpaceOf(tile, static_cast<U32>(poly - tile->polys));
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
