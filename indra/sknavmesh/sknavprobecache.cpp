/**
 * @file sknavprobecache.cpp
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

#include "linden_common.h"

#include "sknavprobecache.h"
#include "sknavfingerprint.h"
#include "sknavtilecache.h"

#include <algorithm>
#include <cstring>

namespace sknav
{
    namespace
    {
        constexpr U32 MAGIC = 0x50504b53;           // 'SKPP'
        constexpr U64 KEY_SALT = 0x70726f6265706172ull;  // keeps these keys apart from the tile products'

        U64 mixFloat(U64 h, F32 v)
        {
            U32 bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            return mix(h, bits);
        }

        U64 checksum(const U8* data, size_t size)
        {
            U64 h = SEED;
            for (size_t i = 0; i < size; ++i)
            {
                h = mix(h, data[i]);
            }
            return h;
        }

        struct Writer
        {
            std::vector<U8> mOut;
            template <typename T> void pod(const T& v)
            {
                const U8* p = reinterpret_cast<const U8*>(&v);
                mOut.insert(mOut.end(), p, p + sizeof(T));
            }
            void vec(const LLVector3& v)
            {
                pod(v.mV[0]);
                pod(v.mV[1]);
                pod(v.mV[2]);
            }
        };

        struct Reader
        {
            const U8* mData;
            size_t mSize;
            size_t mPos = 0;
            template <typename T> bool pod(T& v)
            {
                if (mSize - mPos < sizeof(T))
                {
                    return false;
                }
                std::memcpy(&v, mData + mPos, sizeof(T));
                mPos += sizeof(T);
                return true;
            }
            bool vec(LLVector3& v) { return pod(v.mV[0]) && pod(v.mV[1]) && pod(v.mV[2]); }
            bool count(U32& n, size_t per) { return pod(n) && static_cast<U64>(n) * per <= mSize - mPos; }
        };

        constexpr size_t PART_BYTES = 4 + 12 + 4 + 4 + 1 + 4 + 12 + 12;
    }

    static_assert(sizeof(void*) != 8 || sizeof(ProbeParams) == 80, "a new ProbeParams field must go into probePartsKey");
    static_assert(sizeof(void*) != 8 || sizeof(ProbePart) == 56, "a new ProbePart field must go into packProbeParts");

    U64 probePartsKey(std::vector<U64> tile_keys, const ProbeParams& p)
    {
        if (tile_keys.empty() || std::find(tile_keys.begin(), tile_keys.end(), U64(0)) != tile_keys.end())
        {
            return 0;
        }
        std::sort(tile_keys.begin(), tile_keys.end());
        tile_keys.erase(std::unique(tile_keys.begin(), tile_keys.end()), tile_keys.end());
        U64 h = mix(SEED, KEY_SALT);
        h = mix(h, PROBE_PARTS_VERSION);
        h = mix(h, SKNAV_BUILD_FINGERPRINT);
        h = mix(h, tile_keys.size());
        for (U64 k : tile_keys)
        {
            h = mix(h, k);
        }
        for (F32 v : { p.mCellM, p.mMinPartM2, p.mMinPartWidthM, p.mMaxRadiusM, p.mMinRadiusM, p.mCoverageShare, p.mSpaceRadiusM,
                       p.mBlendZoneM, p.mBlendShare, p.mUnknownRadiusM, p.mObligationShare, p.mOpenShare, p.mMoveM, p.mRadiusShare,
                       p.mMatchM, p.mBudgetHoldM, p.mSpacingM })
        {
            h = mixFloat(h, v);
        }
        h = mix(h, p.mMaxRects);
        h = mix(h, p.mUnknownMinCells);
        h = mix(h, p.mMaxPerSpace);
        const U64 key = finishKey(h);
        return key ? key : 1;
    }

    std::vector<U8> packProbeParts(const std::vector<CachedSpaceParts>& spaces, U64 key)
    {
        Writer w;
        w.pod(MAGIC);
        w.pod(PROBE_PARTS_VERSION);
        w.pod(key);
        w.pod(static_cast<U32>(spaces.size()));
        for (const CachedSpaceParts& s : spaces)
        {
            w.pod(s.mSpace);
            w.vec(s.mAnchor);
            w.pod(static_cast<U32>(s.mParts.size()));
            for (const ProbePart& part : s.mParts)
            {
                w.pod(part.mPart);
                w.vec(part.mOrigin);
                w.pod(part.mRadius);
                w.pod(part.mSpaceRadius);
                w.pod(part.mGrowth);
                w.pod(part.mArea);
                w.vec(part.mMin);
                w.vec(part.mMax);
            }
        }
        w.pod(checksum(w.mOut.data(), w.mOut.size()));
        return std::move(w.mOut);
    }

    bool unpackProbeParts(const U8* data, size_t size, U64 key, std::vector<CachedSpaceParts>& out)
    {
        if (!data || size < sizeof(U64))
        {
            return false;
        }
        const size_t body = size - sizeof(U64);
        U64 sum = 0;
        std::memcpy(&sum, data + body, sizeof(sum));
        if (sum != checksum(data, body))
        {
            return false;
        }
        Reader r{ data, body };
        U32 magic = 0, version = 0, n = 0;
        U64 stored = 0;
        if (!(r.pod(magic) && r.pod(version) && r.pod(stored)) || magic != MAGIC || version != PROBE_PARTS_VERSION || stored != key)
        {
            return false;
        }
        std::vector<CachedSpaceParts> spaces;
        if (!r.count(n, 20))
        {
            return false;
        }
        spaces.resize(n);
        for (CachedSpaceParts& s : spaces)
        {
            U32 parts = 0;
            if (!(r.pod(s.mSpace) && r.vec(s.mAnchor) && r.count(parts, PART_BYTES)))
            {
                return false;
            }
            s.mParts.resize(parts);
            for (ProbePart& part : s.mParts)
            {
                if (!(r.pod(part.mPart) && r.vec(part.mOrigin) && r.pod(part.mRadius) && r.pod(part.mSpaceRadius) && r.pod(part.mGrowth) &&
                      r.pod(part.mArea) && r.vec(part.mMin) && r.vec(part.mMax)))
                {
                    return false;
                }
            }
        }
        if (r.mPos != r.mSize)
        {
            return false;
        }
        out.swap(spaces);
        return true;
    }

    ProbePartsCache::ProbePartsCache(size_t max_parts) : mMax(max_parts)
    {
    }

    S32 ProbePartsCache::match(const Entry& entry, U32 space, const LLVector3& anchor) const
    {
        const F32 limit = PROBE_ANCHOR_MATCH_M * PROBE_ANCHOR_MATCH_M;
        S32 best = -1;
        F32 best_d2 = limit;
        for (size_t i = 0; i < entry.mSpaces.size(); ++i)
        {
            const CachedSpaceParts& s = entry.mSpaces[i];
            const F32 d2 = dist_vec_squared(s.mAnchor, anchor);
            if (d2 > limit)
            {
                continue;
            }
            if (s.mSpace == space)
            {
                return static_cast<S32>(i);
            }
            if (d2 < best_d2 || best < 0)
            {
                best_d2 = d2;
                best = static_cast<S32>(i);
            }
        }
        return best;
    }

    const std::vector<ProbePart>* ProbePartsCache::find(U64 key, U32 space, const LLVector3& anchor)
    {
        const auto it = mEntries.find(key);
        if (it == mEntries.end())
        {
            return nullptr;
        }
        it->second.mUsed = ++mClock;
        const S32 i = match(it->second, space, anchor);
        return i < 0 ? nullptr : &it->second.mSpaces[i].mParts;
    }

    bool ProbePartsCache::hasKey(U64 key) const
    {
        return mEntries.count(key) != 0;
    }

    std::vector<ProbePart> ProbePartsCache::partsFor(U64 key, const Space& space, const ProbeParams& params, bool* placed)
    {
        if (const std::vector<ProbePart>* held = find(key, space.mId, space.mAnchor))
        {
            if (placed) *placed = false;
            return *held;
        }
        std::vector<ProbePart> parts = spaceProbes(space, params);
        insert(key, space.mId, space.mAnchor, parts);
        if (placed) *placed = true;
        return parts;
    }

    void ProbePartsCache::insert(U64 key, U32 space, const LLVector3& anchor, const std::vector<ProbePart>& parts)
    {
        Entry& entry = mEntries[key];
        entry.mUsed = ++mClock;
        const S32 i = match(entry, space, anchor);
        if (i >= 0)
        {
            CachedSpaceParts& s = entry.mSpaces[i];
            mParts -= s.mParts.size();
            entry.mParts -= s.mParts.size();
            s.mSpace = space;
            s.mAnchor = anchor;
            s.mParts = parts;
        }
        else
        {
            entry.mSpaces.push_back({ space, anchor, parts });
        }
        mParts += parts.size();
        entry.mParts += parts.size();
        prune(key);
    }

    void ProbePartsCache::adopt(U64 key, const std::vector<CachedSpaceParts>& spaces)
    {
        Entry& entry = mEntries[key];
        entry.mUsed = ++mClock;
        for (const CachedSpaceParts& s : spaces)
        {
            if (match(entry, s.mSpace, s.mAnchor) < 0)
            {
                entry.mSpaces.push_back(s);
                mParts += s.mParts.size();
                entry.mParts += s.mParts.size();
            }
        }
        prune(key);
    }

    const std::vector<CachedSpaceParts>* ProbePartsCache::spaces(U64 key) const
    {
        const auto it = mEntries.find(key);
        return it == mEntries.end() ? nullptr : &it->second.mSpaces;
    }

    void ProbePartsCache::setMaxParts(size_t max_parts)
    {
        mMax = max_parts;
        prune(0);
    }

    void ProbePartsCache::clear()
    {
        mEntries.clear();
        mParts = 0;
    }

    void ProbePartsCache::prune(U64 keep)
    {
        if (mParts <= mMax)
        {
            return;
        }
        std::vector<std::pair<U64, U64>> order;     // used, key
        order.reserve(mEntries.size());
        for (const auto& e : mEntries)
        {
            if (e.first != keep)
            {
                order.emplace_back(e.second.mUsed, e.first);
            }
        }
        std::sort(order.begin(), order.end());
        for (const auto& o : order)
        {
            if (mParts <= mMax)
            {
                break;
            }
            const auto it = mEntries.find(o.second);
            mParts -= it->second.mParts;
            mEntries.erase(it);
        }
    }
}
