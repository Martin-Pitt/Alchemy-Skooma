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
    const std::vector<SkyMap::Interval>* SkyMap::find(S64 x, S64 y) const
    {
        const auto b = mBlocks.find(blockKey(x, y));
        if (b == mBlocks.end())
        {
            return nullptr;
        }
        const auto c = b->second->mCells.find(cellKey(x, y));
        return c == b->second->mCells.end() ? nullptr : &c->second;
    }

    SkyMap::Block& SkyMap::writable(S64 x, S64 y)
    {
        std::shared_ptr<Block>& b = mBlocks[blockKey(x, y)];
        if (!b)
        {
            b = std::make_shared<Block>();
        }
        else if (b.use_count() > 1)
        {
            b = std::make_shared<Block>(*b);    // a snapshot still reads the old one
        }
        return *b;
    }

    void SkyMap::drop(U64 contributor)
    {
        const auto it = mContributors.find(contributor);
        if (it == mContributors.end())
        {
            return;
        }
        for (U64 key : *it->second)
        {
            const S64 x = static_cast<S32>(key >> 32), y = static_cast<S32>(key & 0xffffffffu);
            if (!find(x, y))
            {
                continue;
            }
            Block& block = writable(x, y);
            auto cell = block.mCells.find(key);
            auto& v = cell->second;
            v.erase(std::remove_if(v.begin(), v.end(), [contributor](const Interval& i) { return i.mContributor == contributor; }), v.end());
            if (v.empty())
            {
                block.mCells.erase(cell);
                --mCellCount;
                if (block.mCells.empty())
                {
                    mBlocks.erase(blockKey(x, y));
                }
            }
        }
        mContributors.erase(it);
    }

    void SkyMap::set(U64 contributor, const LLVector3d& tile_min, const TileAir& air)
    {
        drop(contributor);
        // Tiles are unrotated and start on whole metres, so each top cell is one global metre cell.
        const F64 metres = air.mTopCell * air.mCell;
        auto touched = std::make_shared<std::vector<U64>>();
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
                std::vector<Interval>& cell = writable(gx, gy).mCells[key];
                if (cell.empty())
                {
                    ++mCellCount;
                }
                for (U32 i = air.mTopStart[c]; i < air.mTopStart[c + 1]; ++i)
                {
                    const TopInterval& t = air.mTop[i];
                    cell.push_back({ tile_min.mdV[VZ] + t.mBottom * air.mUnit, tile_min.mdV[VZ] + t.mTop * air.mUnit, contributor });
                }
                touched->push_back(key);
            }
        }
        if (!touched->empty())
        {
            mContributors[contributor] = touched;
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
        const std::vector<Interval>* cell = find(static_cast<S64>(std::floor(pos.mdV[VX])), static_cast<S64>(std::floor(pos.mdV[VY])));
        if (!cell)
        {
            return true;
        }
        for (const Interval& i : *cell)
        {
            const bool inside = i.mBottom <= pos.mdV[VZ] && i.mTop > pos.mdV[VZ];
            if (inside || (i.mBottom > pos.mdV[VZ] && i.mBottom - pos.mdV[VZ] <= reach))
            {
                return false;
            }
        }
        return true;
    }

    bool SkyMap::skyOpenAbove(const LLVector3d& pos, F32 reach, U64 ignore) const
    {
        const std::vector<Interval>* cell = find(static_cast<S64>(std::floor(pos.mdV[VX])), static_cast<S64>(std::floor(pos.mdV[VY])));
        if (!cell)
        {
            return true;
        }
        for (const Interval& i : *cell)
        {
            if (i.mContributor != ignore && i.mBottom > pos.mdV[VZ] && i.mBottom - pos.mdV[VZ] <= reach)
            {
                return false;
            }
        }
        return true;
    }

    bool SkyMap::topAt(F64 x, F64 y, F64& top) const
    {
        const std::vector<Interval>* cell = find(static_cast<S64>(std::floor(x)), static_cast<S64>(std::floor(y)));
        if (!cell || cell->empty())
        {
            return false;
        }
        top = cell->front().mTop;
        for (const Interval& i : *cell)
        {
            top = std::max(top, i.mTop);
        }
        return true;
    }

    size_t SkyMap::bytes() const
    {
        size_t n = sizeof(SkyMap);
        for (const auto& b : mBlocks)
        {
            n += sizeof(b) + sizeof(Block) + 16;
            for (const auto& c : b.second->mCells)
            {
                n += sizeof(c) + 16 + c.second.capacity() * sizeof(Interval);
            }
        }
        for (const auto& c : mContributors)
        {
            n += sizeof(c) + 16 + c.second->capacity() * sizeof(U64);
        }
        return n;
    }
}
