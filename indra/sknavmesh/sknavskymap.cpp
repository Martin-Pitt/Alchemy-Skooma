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

    namespace
    {
        /// The share of 16 spokes out of pos no column standing through pos's
        /// height crosses within SIDE_OPEN_REACH_M, read a metre at a time
        /// along each so no wall is stepped over, stopping at the first.
        template<typename AT>
        F32 sides(const AT& at, const LLVector3d& pos)
        {
            constexpr S32 SPOKES = 16;
            static F32 dirs[SPOKES][2];
            static const bool init = [] {
                for (S32 j = 0; j < SPOKES; ++j)
                {
                    const F64 ang = j * 6.283185307179586 / SPOKES;
                    dirs[j][0] = static_cast<F32>(std::cos(ang));
                    dirs[j][1] = static_cast<F32>(std::sin(ang));
                }
                return true;
            }();
            (void)init;
            const F64 z = pos.mdV[VZ];
            auto wall = [&](F64 x, F64 y) {
                if (const std::vector<SkyMap::Interval>* cell = at(static_cast<S64>(std::floor(x)), static_cast<S64>(std::floor(y))))
                {
                    for (const SkyMap::Interval& i : *cell)
                    {
                        if (i.mBottom <= z && i.mTop > z)
                        {
                            return true;
                        }
                    }
                }
                return false;
            };
            S32 open = 0;
            for (S32 j = 0; j < SPOKES; ++j)
            {
                bool walled = false;
                for (F64 r = 0.5; r < SIDE_OPEN_REACH_M && !walled; r += 1.0)
                {
                    walled = wall(pos.mdV[VX] + r * dirs[j][0], pos.mdV[VY] + r * dirs[j][1]);
                }
                open += walled ? 0 : 1;
            }
            return static_cast<F32>(open) / SPOKES;
        }

        /// The cosine-weighted share of sky hidden over pos, along spokes read
        /// outwards: a column's lowest solid starting above pos hides the band
        /// of zenith its annulus spans (a disc of radius r at height h hides
        /// r^2 / (r^2 + h^2)), and a column standing through pos's height is a
        /// wall hiding the sky under its top's elevation, over the part of the
        /// spoke's wedge it spans, read along two directions across it. roof, when asked, gets the share
        /// the solids overhead hide alone.
        template<typename AT>
        F32 cover(const AT& at, const LLVector3d& pos, F32 radius, F32* roof)
        {
            static const F32 EDGES[] = { 0.5f, 1.f, 1.5f, 2.5f, 3.5f, 5.f, 7.f, 10.f, 14.f, 20.f, 28.f, 40.f, 56.f };
            constexpr S32 RINGS = sizeof(EDGES) / sizeof(EDGES[0]) - 1;
            constexpr S32 SPOKES = 16;
            constexpr S32 SUBS = 2;     // wall directions across each spoke's wedge, so a wall hides only the part it spans
            constexpr F64 WEDGE = 6.283185307179586 / SPOKES;
            static F32 dirs[SPOKES * SUBS][2];
            static const bool init = [] {
                for (S32 j = 0; j < SPOKES * SUBS; ++j)
                {
                    const F64 ang = (j / SUBS) * WEDGE + ((j % SUBS + 0.5) / SUBS - 0.5) * WEDGE;
                    dirs[j][0] = static_cast<F32>(std::cos(ang));
                    dirs[j][1] = static_cast<F32>(std::sin(ang));
                }
                return true;
            }();
            (void)init;
            const F64 z = pos.mdV[VZ];
            auto disc = [](F64 r, F64 h) { return r * r / (r * r + h * h); };
            // The height of the lowest solid starting above z, 0 for none; wall the height a solid through z rises to.
            auto read = [&](F64 x, F64 y, F64& wall) {
                wall = 0.0;
                F64 h = 0.0;
                if (const std::vector<SkyMap::Interval>* cell = at(static_cast<S64>(std::floor(x)), static_cast<S64>(std::floor(y))))
                {
                    for (const SkyMap::Interval& i : *cell)
                    {
                        if (i.mBottom > z)
                        {
                            h = h == 0.0 ? i.mBottom - z : std::min(h, i.mBottom - z);
                        }
                        else if (i.mTop > z)
                        {
                            wall = std::max(wall, i.mTop - z);
                        }
                    }
                }
                return h;
            };
            F64 wall = 0.0;
            const F64 h0 = read(pos.mdV[VX], pos.mdV[VY], wall);
            const F64 c0 = h0 > 0.0 ? disc(EDGES[0], h0) : 0.0;     // the column overhead, the same on every spoke
            F64 total = c0, over = c0;
            // Past OUTER one read per spoke: walls there are far enough for its wedge to be their measure.
            constexpr F64 OUTER = 14.0;
            for (S32 j = 0; j < SPOKES; ++j)
            {
                F64 lo[RINGS], hi[RINGS];
                S32 n = 0;
                F64 low[SUBS] = {};     // per wall direction, the share of its sky the walls on it hide, from the horizon up
                const S32 mid = j * SUBS;    // the first wall direction carries the overhead reads too
                for (S32 k = 0; k < RINGS && EDGES[k] < radius; ++k)
                {
                    const F64 a = EDGES[k], b = std::min<F64>(EDGES[k + 1], radius), r = 0.5 * (a + b);
                    F64 w[SUBS];
                    const F64 h = read(pos.mdV[VX] + r * dirs[mid][0], pos.mdV[VY] + r * dirs[mid][1], w[0]);
                    // Near the middle the wedge is under a column wide: one read speaks for it all.
                    const bool single = r * WEDGE < 1.5 || a >= OUTER;
                    for (S32 q = 1; q < SUBS; ++q)
                    {
                        w[q] = w[0];
                        if (!single)
                        {
                            read(pos.mdV[VX] + r * dirs[j * SUBS + q][0], pos.mdV[VY] + r * dirs[j * SUBS + q][1], w[q]);
                        }
                    }
                    if (h > 0.0)
                    {
                        lo[n] = disc(a, h);
                        hi[n++] = disc(b, h);
                    }
                    for (S32 q = 0; q < SUBS; ++q)
                    {
                        if (w[q] > 0.0)
                        {
                            low[q] = std::max(low[q], 1.0 - disc(r, w[q]));
                        }
                    }
                }
                F64 low_mean = 0.0;
                for (S32 q = 0; q < SUBS; ++q)
                {
                    low_mean += low[q] / SUBS;
                }
                const F64 horizon = std::max(1.0 - low_mean, c0);
                F64 spoke = 1.0 - horizon, roofed = 0.0;
                for (S32 i = 0; i < n; ++i)
                {
                    spoke += std::max(0.0, std::min(hi[i], horizon) - std::max(lo[i], c0));
                    roofed += std::max(0.0, hi[i] - std::max(lo[i], c0));
                }
                total += std::min(spoke, 1.0 - c0) / SPOKES;
                over += std::min(roofed, 1.0 - c0) / SPOKES;
            }
            if (roof)
            {
                *roof = static_cast<F32>(std::min(over, 1.0));
            }
            return static_cast<F32>(std::min(total, 1.0));
        }
    }

    F32 SkyMap::coverAbove(const LLVector3d& pos, F32 radius, F32* roof, F32* open) const
    {
        const auto at = [this](S64 x, S64 y) { return find(x, y); };
        if (open)
        {
            *open = sides(at, pos);
        }
        return cover(at, pos, radius, roof);
    }

    F32 SkyMap::sideOpenAt(const LLVector3d& pos) const
    {
        return sides([this](S64 x, S64 y) { return find(x, y); }, pos);
    }

    SkyView::SkyView(const SkyMap& map, F64 x0, F64 y0, F64 x1, F64 y1)
        : mMap(&map), mX0(static_cast<S64>(std::floor(x0))), mY0(static_cast<S64>(std::floor(y0)))
    {
        mWidth = static_cast<S32>(std::floor(x1)) - static_cast<S32>(mX0) + 1;
        mHeight = static_cast<S32>(std::floor(y1)) - static_cast<S32>(mY0) + 1;
        if (mWidth <= 0 || mHeight <= 0 || static_cast<S64>(mWidth) * mHeight > (1 << 24))
        {
            mWidth = mHeight = 0;
            return;
        }
        mCells.assign(static_cast<size_t>(mWidth) * mHeight, nullptr);
        for (S32 y = 0; y < mHeight; ++y)
        {
            for (S32 x = 0; x < mWidth; ++x)
            {
                mCells[x + static_cast<size_t>(y) * mWidth] = map.find(mX0 + x, mY0 + y);
            }
        }
    }

    const std::vector<SkyMap::Interval>* SkyView::cell(S64 x, S64 y) const
    {
        const S64 i = x - mX0, j = y - mY0;
        return i >= 0 && j >= 0 && i < mWidth && j < mHeight ? mCells[i + j * mWidth] : mMap->find(x, y);
    }

    F32 SkyView::coverAbove(const LLVector3d& pos, F32 radius, F32* roof, F32* open) const
    {
        const auto at = [this](S64 x, S64 y) { return cell(x, y); };
        if (open)
        {
            *open = sides(at, pos);
        }
        return cover(at, pos, radius, roof);
    }

    F32 SkyView::sideOpenAt(const LLVector3d& pos) const
    {
        return sides([this](S64 x, S64 y) { return cell(x, y); }, pos);
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
