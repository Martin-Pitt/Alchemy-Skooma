/**
 * @file sknavspacefilter.h
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

#ifndef SK_NAVSPACEFILTER_H
#define SK_NAVSPACEFILTER_H

#include "stdtypes.h"

#include "DetourNavMeshQuery.h"

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace sknav
{
    /// Detour's filter with a cost per space: a poly's step costs its area's
    /// cost times its space's, and a space priced at 0 is barred. Spaces with
    /// no price cost 1, so with none set it is Detour's own filter. Costs below
    /// 1 are raised to it, which keeps the search's distance estimate a lower
    /// bound. Keep one per query: it caches tiles' tables. Needs Detour built with DT_VIRTUAL_QUERYFILTER, or the overrides
    /// below do not compile.
    class SpaceQueryFilter : public dtQueryFilter
    {
    public:
        /// The space id of a tile's poly, by the poly's index in its tile; 0 for none.
        typedef std::function<U32(const dtMeshTile* tile, U32 poly)> SpaceOf;
        /// A tile's polys' space ids, by poly index; null or short where it has none.
        struct TileSpaces
        {
            const U32* mSpaces = nullptr;
            U32 mCount = 0;
        };
        typedef std::function<TileSpaces(const dtMeshTile* tile)> SpacesOf;

        explicit SpaceQueryFilter(SpaceOf space_of = nullptr) : mSpaceOf(std::move(space_of)) {}
        /// The fast form: each tile's table is fetched once and kept, so queries do not call out per poly.
        explicit SpaceQueryFilter(SpacesOf spaces_of) : mSpacesOf(std::move(spaces_of)) {}

        /// The cost of crossing a space, per metre; 0 bars it.
        void setSpaceCost(U32 space, F32 cost);
        F32 spaceCost(U32 space) const;

        bool passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const override;
        float getCost(const float* pa, const float* pb, const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
                      const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly, const dtPolyRef nextRef,
                      const dtMeshTile* nextTile, const dtPoly* nextPoly) const override;

    private:
        U32 spaceOf(const dtMeshTile* tile, const dtPoly* poly) const;

        struct CachedTile
        {
            const dtMeshTile* mTile = nullptr;
            TileSpaces mSpaces;
        };

        SpaceOf mSpaceOf;
        SpacesOf mSpacesOf;
        std::vector<std::pair<U32, F32>> mCosts;
        mutable std::array<CachedTile, 64> mTiles;
    };
}

#endif // SK_NAVSPACEFILTER_H
