/**
 * @file sknavskymap.h
 * @brief What stands over each square metre of the world, from every published tile's solid tops.
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

#ifndef SK_NAVSKYMAP_H
#define SK_NAVSKYMAP_H

#include "sknavairdistil.h"
#include "v3dmath.h"

#include <unordered_map>
#include <vector>

namespace sknav
{
    /// Solid intervals per global 1 m cell, each owned by the tile that
    /// contributed it. A cell is rebuilt from its current contributors on
    /// every change, so a roof whose tile is republished without it, or
    /// removed, is forgotten. Not thread-safe.
    class SkyMap
    {
    public:
        struct Interval
        {
            F64 mBottom = 0.0;      ///< global z
            F64 mTop = 0.0;
            U64 mContributor = 0;
        };

        /// Replace what one tile contributes. tile_min is the global corner
        /// of the tile, its z where the air's height 0 is.
        void set(U64 contributor, const LLVector3d& tile_min, const TileAir& air);
        void remove(U64 contributor);
        bool has(U64 contributor) const { return mContributors.count(contributor) != 0; }

        /// No solid starts above pos within reach metres.
        bool skyOpenAt(const LLVector3d& pos, F32 reach = 50.f) const;

        /// Bumped on every change, for readers holding derived results.
        U64 version() const { return mVersion; }
        size_t cells() const { return mCells.size(); }
        size_t bytes() const;

    private:
        static U64 cellKey(S64 x, S64 y) { return (static_cast<U64>(static_cast<U32>(x)) << 32) | static_cast<U32>(y); }
        void drop(U64 contributor);

        std::unordered_map<U64, std::vector<Interval>> mCells;
        std::unordered_map<U64, std::vector<U64>> mContributors;   ///< the cells each contributor touches
        U64 mVersion = 0;
    };
}

#endif // SK_NAVSKYMAP_H
