/**
 * @file sknavtilecache.h
 * @brief Tile products on disk under a session-stable content key, and the air budget's eviction order.
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

#ifndef SK_NAVTILECACHE_H
#define SK_NAVTILECACHE_H

#include "sknavbuild.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sknav
{
    /// Positions and extents are keyed to the census's own movement epsilon,
    /// so a part that did not move by its measure keys the same.
    constexpr F32 KEY_POSITION_M = 0.01f;
    /// Unit axes, and anything else dimensionless.
    constexpr F32 KEY_UNIT = 1e-3f;

    /// What a record builds from, and nothing that lives only for a session:
    /// its part id (unless it is a placeholder keyed by a local id), role,
    /// flags, shape, region origin, quantised transform, bounds and triangles.
    U64 recordContentKey(const Record& record);

    /// splitmix64's finaliser. FNV only carries a change upwards, so a key
    /// whose last input moved in its top bits would keep its low bits.
    U64 finishKey(U64 h);

    /// Everything a tile build reads, from the input itself: mesh origin, tile
    /// index, height range, every parameter, the terrain window's samples and
    /// the multiset of record and cut keys, in whatever order they came.
    U64 tileContentKey(const TileBuildInput& input);

    /// Bumped whenever the product format or what a build makes of its input
    /// changes, which orphans every entry written before.
    constexpr U32 TILE_PRODUCT_VERSION = 10;

    /// A versioned little-endian form of a tile's products under its key:
    /// a header, then tagged sections a reader skips when it does not know
    /// them, then a checksum over everything before it.
    std::vector<U8> packTileProduct(const TileBuildResult& result, U64 key);
    /// False, leaving out untouched, when the bytes are short, damaged, of
    /// another version or written for another key.
    bool unpackTileProduct(const U8* data, size_t size, U64 key, TileBuildResult& out);

    /// Where a settled tile's detail air and held raster wait on disk, under its raster's key.
    U64 spillKey(U64 content_key);
    /// The air and raster bit for bit; either may be null.
    std::vector<U8> packSpill(const TileAir* air, const TileRaster* raster, U64 key);
    /// False, leaving the outputs untouched, on bytes that are short, damaged, of
    /// another version or key. A raster read back points at the air read with it.
    bool unpackSpill(const U8* data, size_t size, U64 key, TileAirPtr& air, TileRasterPtr& raster);

    /// Where a tile's stamped polys are cached: under its raster's key and the
    /// stamp's pattern, so the same partition finds them whatever the spaces are called.
    U64 stampedKey(U64 content_key, U64 pattern);
    /// What the polygon stage made: Detour, edges, links, flights and the stamp's tables.
    TileBuildResult stampedPart(const TileBuildResult& result);

    /// A tile's air with only its solid tops, which the sky map reads: what an
    /// evicted tile keeps.
    TileAirPtr coarseAir(const TileAir& air);

    /// Products on disk, one file per key, pruned least recently used under a
    /// byte cap. Safe from any thread; the viewer calls it from workers only.
    class TileDiskCache
    {
    public:
        /// Touches nothing on disk until index() has run.
        TileDiskCache(const std::string& dir, U64 cap_bytes);
        /// Create the folder, sweep what a crash left half written, and index
        /// the entries there oldest first. Slow on a big folder: a worker's job.
        void index();

        bool load(U64 key, std::vector<U8>& out);
        bool store(U64 key, const std::vector<U8>& bytes);
        void erase(U64 key);
        bool contains(U64 key) const;
        void setCap(U64 cap_bytes);
        U64 bytes() const;
        size_t entries() const;
        /// Files a remove failed on, tried again at the next prune.
        size_t pendingRemovals() const;

    private:
        struct Entry
        {
            U64 mBytes = 0;
            U64 mUsed = 0;
        };
        std::string pathOf(U64 key) const;
        void pruneLocked(std::vector<std::string>& doomed);
        void removeFiles(const std::vector<std::string>& paths);

        mutable std::mutex mMutex;
        std::string mDir;
        U64 mCap = 0;
        U64 mBytes = 0;
        U64 mClock = 0;
        std::unordered_map<U64, Entry> mEntries;
        std::vector<std::string> mDoomed;
    };

    /// Nearness integrated over time with a decay: a tile the camera sits by
    /// climbs, one it left long ago sinks. Distance is from the camera to the
    /// tile's footprint.
    F64 accumulateNearness(F64 score, F64 distance, F64 radius, F64 dt, F64 decay_seconds = 60.0);

    struct EvictionCandidate
    {
        size_t mBytes = 0;
        F64 mScore = 0.0;       ///< accumulateNearness
        F64 mDistance = 0.0;
    };

    /// Indices to evict, least near first, until the rest fit the budget.
    /// Nothing within protect_distance is ever chosen, so the result may
    /// still leave the total over it.
    std::vector<size_t> chooseEvictions(const std::vector<EvictionCandidate>& candidates, size_t budget_bytes, F64 protect_distance);
}

#endif // SK_NAVTILECACHE_H
