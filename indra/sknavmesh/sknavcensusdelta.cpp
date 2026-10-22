/**
 * @file sknavcensusdelta.cpp
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

#include "sknavcensusdelta.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sknav
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

        bool byPart(const RecordPtr& a, const RecordPtr& b)
        {
            return a->mPart < b->mPart;
        }
    }

    U64 finalise(U64 h)
    {
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
        return h ^ (h >> 31);
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
        RecordsByPart out;
        out.reserve(records.size());
        for (const RecordPtr& rec : records)
        {
            if (rec)
            {
                out[rec->mPart] = rec;
            }
        }
        return out;
    }

    void diffRecords(const RecordsByPart& before, const RecordsByPart& after, std::vector<RecordPtr>& added,
                     std::vector<RecordPtr>& removed, std::vector<RecordPtr>& changed)
    {
        added.clear();
        removed.clear();
        changed.clear();
        for (const auto& entry : after)
        {
            const auto was = before.find(entry.first);
            if (was == before.end())
            {
                added.push_back(entry.second);
            }
            else if (was->second->mSignature != entry.second->mSignature || was->second->mDynamic != entry.second->mDynamic)
            {
                changed.push_back(entry.second);
            }
        }
        for (const auto& entry : before)
        {
            if (!after.count(entry.first))
            {
                removed.push_back(entry.second);
            }
        }
        std::sort(added.begin(), added.end(), byPart);
        std::sort(removed.begin(), removed.end(), byPart);
        std::sort(changed.begin(), changed.end(), byPart);
    }
}
