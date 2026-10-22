/**
 * @file sknavprobecache.h
 * @brief Room probe parts cached under the content keys of the tiles a space covers.
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

#ifndef SK_NAVPROBECACHE_H
#define SK_NAVPROBECACHE_H

#include "sknavprobes.h"

#include <unordered_map>
#include <vector>

namespace sknav
{
    /// Bumped whenever the parts' disk form changes; entries of another version are rebuilt.
    constexpr U32 PROBE_PARTS_VERSION = 1;

    /// A cached space whose id changed matches by its anchor within this.
    constexpr F32 PROBE_ANCHOR_MATCH_M = 1.5f;

    /// The key the parts of the spaces over a set of tiles are cached under:
    /// the tiles' content keys in any order, every placement parameter and
    /// the library's build fingerprint. Zero, meaning do not cache, when no
    /// tile is given or any tile has no content key.
    U64 probePartsKey(std::vector<U64> tile_keys, const ProbeParams& params);

    /// One space's parts as they were placed.
    struct CachedSpaceParts
    {
        U32 mSpace = 0;             ///< its id when placed
        LLVector3 mAnchor;          ///< Space::mAnchor, what a re-cut space is recognised by
        std::vector<ProbePart> mParts;
    };

    /// A versioned little-endian form of every space under one key, checksummed.
    std::vector<U8> packProbeParts(const std::vector<CachedSpaceParts>& spaces, U64 key);
    /// False, leaving out untouched, when the bytes are short, damaged, of
    /// another version or written for another key.
    bool unpackProbeParts(const U8* data, size_t size, U64 key, std::vector<CachedSpaceParts>& out);

    /// Parts by key in memory, bounded by a count of parts and let go least
    /// recently seen first; nothing here knows where the camera is. Not
    /// thread safe: the viewer keeps it on the main thread.
    class ProbePartsCache
    {
    public:
        explicit ProbePartsCache(size_t max_parts = 65536);

        /// The parts of a space under the key: the entry of the same id whose
        /// anchor is within PROBE_ANCHOR_MATCH_M, else the nearest anchor
        /// within it. Null on a miss. Marks the key seen.
        const std::vector<ProbePart>* find(U64 key, U32 space, const LLVector3& anchor);
        bool hasKey(U64 key) const;
        /// The cached parts of a space, or its parts placed now and cached;
        /// placed says which. A space re-cut or re-signed over unchanged tiles
        /// keeps the parts it had.
        std::vector<ProbePart> partsFor(U64 key, const Space& space, const ProbeParams& params, bool* placed = nullptr);
        /// Add a space's parts, replacing the entry find() would have matched.
        void insert(U64 key, U32 space, const LLVector3& anchor, const std::vector<ProbePart>& parts);
        /// Spaces read back from disk; ones already held for the key win.
        void adopt(U64 key, const std::vector<CachedSpaceParts>& spaces);
        /// Every space under the key, or null.
        const std::vector<CachedSpaceParts>* spaces(U64 key) const;
        void setMaxParts(size_t max_parts);
        void clear();
        size_t keys() const { return mEntries.size(); }
        size_t parts() const { return mParts; }

    private:
        struct Entry
        {
            std::vector<CachedSpaceParts> mSpaces;
            size_t mParts = 0;
            U64 mUsed = 0;
        };
        S32 match(const Entry& entry, U32 space, const LLVector3& anchor) const;
        void prune(U64 keep);

        std::unordered_map<U64, Entry> mEntries;
        size_t mParts = 0;
        size_t mMax = 0;
        U64 mClock = 0;
    };
}

#endif // SK_NAVPROBECACHE_H
