/**
 * @file sknavskymap.cpp
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

#include "linden_common.h"

#include "sknavskymap.h"

#include <algorithm>
#include <cmath>

namespace sknav
{
    void SkyMap::drop(U64 contributor)
    {
        const auto it = mContributors.find(contributor);
        if (it == mContributors.end())
        {
            return;
        }
        for (U64 key : it->second)
        {
            const auto cell = mCells.find(key);
            if (cell == mCells.end())
            {
                continue;
            }
            auto& v = cell->second;
            v.erase(std::remove_if(v.begin(), v.end(), [contributor](const Interval& i) { return i.mContributor == contributor; }), v.end());
            if (v.empty())
            {
                mCells.erase(cell);
            }
        }
        mContributors.erase(it);
    }

    void SkyMap::set(U64 contributor, const LLVector3d& tile_min, const TileAir& air)
    {
        drop(contributor);
        // Tiles are unrotated and start on whole metres, so each top cell is one global metre cell.
        const F64 metres = air.mTopCell * air.mCell;
        std::vector<U64>& touched = mContributors[contributor];
        for (S32 ty = 0; ty < air.mTopHeight; ++ty)
        {
            for (S32 tx = 0; tx < air.mTopWidth; ++tx)
            {
                const S32 c = tx + ty * air.mTopWidth;
                if (air.mTopStart[c] == air.mTopStart[c + 1])
                {
                    continue;
                }
                const S64 gx = static_cast<S64>(std::floor(tile_min.mdV[VX] + (tx + 0.5) * metres));
                const S64 gy = static_cast<S64>(std::floor(tile_min.mdV[VY] + (ty + 0.5) * metres));
                const U64 key = cellKey(gx, gy);
                std::vector<Interval>& cell = mCells[key];
                for (U32 i = air.mTopStart[c]; i < air.mTopStart[c + 1]; ++i)
                {
                    const TopInterval& t = air.mTop[i];
                    cell.push_back({ tile_min.mdV[VZ] + t.mBottom * air.mUnit, tile_min.mdV[VZ] + t.mTop * air.mUnit, contributor });
                }
                touched.push_back(key);
            }
        }
        if (touched.empty())
        {
            mContributors.erase(contributor);
        }
        ++mVersion;
    }

    void SkyMap::remove(U64 contributor)
    {
        if (has(contributor))
        {
            drop(contributor);
            ++mVersion;
        }
    }

    bool SkyMap::skyOpenAt(const LLVector3d& pos, F32 reach) const
    {
        const auto it = mCells.find(cellKey(static_cast<S64>(std::floor(pos.mdV[VX])), static_cast<S64>(std::floor(pos.mdV[VY]))));
        if (it == mCells.end())
        {
            return true;
        }
        for (const Interval& i : it->second)
        {
            const bool inside = i.mBottom <= pos.mdV[VZ] && i.mTop > pos.mdV[VZ];
            if (inside || (i.mBottom > pos.mdV[VZ] && i.mBottom - pos.mdV[VZ] <= reach))
            {
                return false;
            }
        }
        return true;
    }

    size_t SkyMap::bytes() const
    {
        size_t n = sizeof(SkyMap);
        for (const auto& c : mCells)
        {
            n += sizeof(c) + 16 + c.second.capacity() * sizeof(Interval);
        }
        for (const auto& c : mContributors)
        {
            n += sizeof(c) + 16 + c.second.capacity() * sizeof(U64);
        }
        return n;
    }
}
