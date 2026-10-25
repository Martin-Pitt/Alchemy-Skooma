/**
 * @file skcensusdelta.cpp
 * @brief What a census scan holds, as one hash, and what changed between two scans, by part.
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

#include "skcensusdelta.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace skworld
{
    namespace
    {
        U64 mixId(U64 h, const LLUUID& id)
        {
            U64 lo, hi;
            std::memcpy(&lo, id.mData, sizeof(lo));
            std::memcpy(&hi, id.mData + 8, sizeof(hi));
            return mix(mix(h, lo), hi);
        }

        // A position to the 1/64 m the overview's own entries are rounded to.
        U64 mixPoint(U64 h, const LLVector3d& p)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                h = mix(h, static_cast<U64>(std::llround(p.mdV[k] * 64.0)));
            }
            return h;
        }

        U64 featureHash(const Feature& f)
        {
            U64 roots = 0;
            for (const LLUUID& root : f.mRoots)
            {
                roots += finalise(mixId(SEED, root));
            }
            U64 h = mix(SEED, static_cast<U64>(f.mKind));
            h = mix(h, roots);
            h = mixPoint(mixPoint(h, f.mMin), f.mMax);
            return finalise(mix(h, f.mWorking));
        }

        U64 doorHash(const DoorEntity& d)
        {
            U64 h = mixId(SEED, d.mId);
            for (const LLUUID& leaf : d.mLeaves)
            {
                h = mixId(h, leaf);
            }
            h = mixPoint(mixPoint(h, d.mMin), d.mMax);
            h = mix(h, static_cast<U64>(std::llround(d.mSill * 64.0)));
            h = mix(h, (d.mPlaneKnown ? 1u : 0u) | (d.mDouble ? 2u : 0u) | (d.mDamageable ? 4u : 0u));
            return finalise(h);
        }

        U64 bigEndianWord(const LLUUID& id, size_t at)
        {
            U64 w = 0;
            for (size_t b = 0; b < 8; ++b)
            {
                w = (w << 8) | id.mData[at + b];
            }
            return w;
        }

        bool partOrder(const PartRecord& a, const PartRecord& b)
        {
            return a.mHigh < b.mHigh || (a.mHigh == b.mHigh && a.mLow < b.mLow);
        }
    }

    U64 recordHash(const Record& rec)
    {
        U64 h = mixId(SEED, rec.mPart);
        h = mix(h, rec.mSignature);
        return finalise(mix(h, rec.mDynamic));
    }

    U64 censusHash(const std::vector<RecordPtr>& records, const std::vector<Feature>& features,
                   const std::vector<DoorEntity>& doors, const ClusterWindow& window, F64 gap)
    {
        // Sums, not XOR: a part listed twice must not cancel itself out.
        U64 record_sum = 0;
        for (const RecordPtr& rec : records)
        {
            if (rec)
            {
                record_sum += recordHash(*rec);
            }
        }
        U64 feature_sum = 0;
        for (const Feature& f : features)
        {
            feature_sum += featureHash(f);
        }
        U64 door_sum = 0;
        for (const DoorEntity& d : doors)
        {
            door_sum += doorHash(d);
        }
        U64 h = mix(mix(mix(SEED, record_sum), feature_sum), door_sum);
        h = mixPoint(mixPoint(h, window.mMin), window.mMax);
        return finalise(mixF64(h, gap));
    }

    RecordsByPart recordsByPart(const std::vector<RecordPtr>& records)
    {
        // Sorted as plain words with the listing order last, so of a part
        // listed twice the last listed is kept; the pointers are copied once.
        struct Key
        {
            U64 mHigh;
            U64 mLow;
            size_t mIndex;
        };
        std::vector<Key> keys;
        keys.reserve(records.size());
        for (size_t i = 0; i < records.size(); ++i)
        {
            if (const RecordPtr& rec = records[i])
            {
                keys.push_back({ bigEndianWord(rec->mPart, 0), bigEndianWord(rec->mPart, 8), i });
            }
        }
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            return a.mHigh != b.mHigh ? a.mHigh < b.mHigh : a.mLow != b.mLow ? a.mLow < b.mLow : a.mIndex < b.mIndex;
        });
        RecordsByPart out;
        out.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i)
        {
            if (i + 1 < keys.size() && keys[i + 1].mHigh == keys[i].mHigh && keys[i + 1].mLow == keys[i].mLow)
            {
                continue;
            }
            out.push_back({ keys[i].mHigh, keys[i].mLow, records[keys[i].mIndex] });
        }
        return out;
    }

    void diffRecords(const RecordsByPart& before, const RecordsByPart& after, std::vector<RecordPtr>& added,
                     std::vector<RecordPtr>& removed, std::vector<RecordPtr>& changed)
    {
        added.clear();
        removed.clear();
        changed.clear();
        // Both in part order: one walk down the two side by side.
        size_t b = 0, a = 0;
        while (b < before.size() || a < after.size())
        {
            if (a == after.size() || (b < before.size() && partOrder(before[b], after[a])))
            {
                removed.push_back(before[b++].mRecord);
            }
            else if (b == before.size() || partOrder(after[a], before[b]))
            {
                added.push_back(after[a++].mRecord);
            }
            else
            {
                const Record& was = *before[b++].mRecord;
                const RecordPtr& now = after[a++].mRecord;
                if (was.mSignature != now->mSignature || was.mDynamic != now->mDynamic)
                {
                    changed.push_back(now);
                }
            }
        }
    }
}
