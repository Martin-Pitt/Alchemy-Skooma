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

#include <memory>
#include <unordered_map>
#include <vector>

namespace sknav
{
    /// Solid intervals per global 1 m cell, each owned by the tile that
    /// contributed it. A cell is rebuilt from its current contributors on
    /// every change, so a roof whose tile is republished without it, or
    /// removed, is forgotten. Not thread-safe, but a copy is cheap and
    /// independent: cells live in 32 m blocks shared copy-on-write, so a
    /// snapshot for a worker costs a pointer per block.
    constexpr F32 SIDE_OPEN_REACH_M = 24.f;    ///< side openness looks for walls this far out

    /// Whether two airs give a sky map the same solid tops, so either sets the same cells.
    bool sameTops(const TileAir& a, const TileAir& b);

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

        /// No solid starts above pos within reach metres, and pos is not inside one.
        bool skyOpenAt(const LLVector3d& pos, F32 reach = 50.f) const;
        /// No solid but ignore's starts above pos within reach: a tile's own
        /// air already knows what stands over it.
        bool skyOpenAbove(const LLVector3d& pos, F32 reach, U64 ignore) const;
        /// The share of a horizontal spot's sky, cosine-weighted, hidden by the
        /// solids within radius: a disc of radius r at height h hides
        /// r^2 / (r^2 + h^2), so what is small and high hides little and what
        /// is broad and low nearly all, and a wall hides the sky under its top.
        /// Read from columns along spokes, no rays. roof, when given, gets the
        /// share the solids overhead hide without the walls, and open side openness.
        F32 coverAbove(const LLVector3d& pos, F32 radius, F32* roof = nullptr, F32* open = nullptr) const;
        /// Side openness, O: the share of 16 spokes out of pos that no solid
        /// standing through pos's height crosses within SIDE_OPEN_REACH_M.
        F32 sideOpenAt(const LLVector3d& pos) const;
        /// The top of the highest solid over (x, y); false when there is none.
        bool topAt(F64 x, F64 y, F64& top) const;

        /// Bumped on every change, for readers holding derived results.
        U64 version() const { return mVersion; }
        size_t cells() const { return mCellCount; }
        size_t bytes() const;

    private:
        friend class SkyView;
        static U64 cellKey(S64 x, S64 y) { return (static_cast<U64>(static_cast<U32>(x)) << 32) | static_cast<U32>(y); }
        static U64 blockKey(S64 x, S64 y) { return cellKey(x >> BLOCK_SHIFT, y >> BLOCK_SHIFT); }
        const std::vector<Interval>* find(S64 x, S64 y) const;
        void drop(U64 contributor);

        static constexpr S32 BLOCK_SHIFT = 5;
        struct Block
        {
            std::unordered_map<U64, std::vector<Interval>> mCells;
        };
        /// Mutated only through a block this map alone holds.
        /// Copy only while the source is quiescent, then give each copy one owner.
        /// Published maps are read-only: writers must use a separate copy, and every
        /// block mutation must pass through writable() to detach shared storage.
        /// Never copy, read and mutate the same map instance concurrently. SkyView
        /// holds raw cell pointers, so its source map must remain alive and unchanged
        /// for the entire view lifetime; shared blocks alone do not keep a view valid.
        Block& writable(S64 x, S64 y);

        std::unordered_map<U64, std::shared_ptr<Block>> mBlocks;
        std::unordered_map<U64, std::shared_ptr<const std::vector<U64>>> mContributors;   ///< the cells each contributor touches
        size_t mCellCount = 0;
        U64 mVersion = 0;
    };

    /// A dense window over a map for many reads in one area; reads outside it
    /// go to the map. The map must outlive it and not change meanwhile.
    class SkyView
    {
    public:
        SkyView(const SkyMap& map, F64 x0, F64 y0, F64 x1, F64 y1);
        F32 coverAbove(const LLVector3d& pos, F32 radius, F32* roof = nullptr, F32* open = nullptr) const;
        F32 sideOpenAt(const LLVector3d& pos) const;

    private:
        const std::vector<SkyMap::Interval>* cell(S64 x, S64 y) const;
        const SkyMap* mMap;
        S64 mX0, mY0;
        S32 mWidth = 0, mHeight = 0;
        std::vector<const std::vector<SkyMap::Interval>*> mCells;
    };
}

#endif // SK_NAVSKYMAP_H
