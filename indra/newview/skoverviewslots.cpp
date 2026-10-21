/**
 * @file skoverviewslots.cpp
 * @brief The census overview's vertex pages: runs allocated within a page, linkset ids stable per root, and the draw
 *        commands a shot builds over a page's slots on the CPU, as overviewCommandsC.glsl builds them on the GPU.
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

#include "skoverviewslots.h"

#include <cmath>
#include <iterator>

namespace skoverview
{
    SlotAllocator::SlotAllocator(U32 capacity)
    : mCapacity(capacity)
    {
        if (capacity)
        {
            mFree.emplace(0u, capacity);
        }
    }

    U32 SlotAllocator::allocate(U32 count)
    {
        if (!count)
        {
            return NONE;
        }
        for (auto it = mFree.begin(); it != mFree.end(); ++it)
        {
            if (it->second < count)
            {
                continue;
            }
            const U32 first = it->first;
            const U32 left = it->second - count;
            mFree.erase(it);
            if (left)
            {
                mFree.emplace(first + count, left);
            }
            mUsed += count;
            return first;
        }
        return NONE;
    }

    void SlotAllocator::release(U32 first, U32 count)
    {
        if (!count)
        {
            return;
        }
        llassert(first + count <= mCapacity && mUsed >= count);
        mUsed -= count;
        auto next = mFree.lower_bound(first);
        llassert(next == mFree.end() || next->first >= first + count);
        // Joined to the free run that ends where it starts, and to the one that starts where it ends.
        if (next != mFree.begin())
        {
            auto prev = std::prev(next);
            llassert(prev->first + prev->second <= first);
            if (prev->first + prev->second == first)
            {
                first = prev->first;
                count += prev->second;
                mFree.erase(prev);
            }
        }
        if (next != mFree.end() && next->first == first + count)
        {
            count += next->second;
            mFree.erase(next);
        }
        mFree.emplace(first, count);
    }

    U32 IdPool::acquire()
    {
        if (!mFree.empty())
        {
            const U32 id = *mFree.begin();
            mFree.erase(mFree.begin());
            return id;
        }
        return mNext++;
    }

    void IdPool::release(U32 id)
    {
        llassert(id < mNext && !mFree.count(id));
        mFree.insert(id);
        // The free ids at the top are no longer below anything in use.
        while (!mFree.empty() && *mFree.rbegin() + 1 == mNext)
        {
            mFree.erase(std::prev(mFree.end()));
            --mNext;
        }
    }

    void IdPool::clear()
    {
        mNext = 0;
        mFree.clear();
    }

    bool boxInPlanes(const F32 lo[3], const F32 hi[3], const F32 planes[6][4])
    {
        for (S32 p = 0; p < 6; ++p)
        {
            const F32* plane = planes[p];
            // The plane's value at the box's corner furthest inside it.
            F32 d = plane[3];
            for (S32 a = 0; a < 3; ++a)
            {
                const F32 centre = 0.5f * (lo[a] + hi[a]);
                const F32 half = 0.5f * (hi[a] - lo[a]);
                d += plane[a] * centre - std::fabs(plane[a]) * half;
            }
            if (d > 0.f)
            {
                return false;
            }
        }
        return true;
    }

    void buildCommands(const std::vector<SlotCull>& slots, const LevelCull& level, const F32 planes[6][4],
                       std::vector<U32>& first, std::vector<U32>& count)
    {
        first.resize(slots.size());
        count.resize(slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
        {
            const SlotCull& slot = slots[i];
            const bool keep = level.keeps(slot.mLo[3], slot.mHi[3], slot.mGround != 0) && boxInPlanes(slot.mLo, slot.mHi, planes);
            first[i] = slot.mFirst;
            count[i] = keep ? slot.mCount : 0;
        }
    }

    void mergeRuns(const std::vector<U32>& first, const std::vector<U32>& count, std::vector<S32>& merged_first,
                   std::vector<S32>& merged_count)
    {
        merged_first.clear();
        merged_count.clear();
        for (size_t i = 0; i < first.size() && i < count.size(); ++i)
        {
            if (!count[i])
            {
                continue;
            }
            if (!merged_first.empty() && (U32)(merged_first.back() + merged_count.back()) == first[i])
            {
                merged_count.back() += (S32)count[i];
            }
            else
            {
                merged_first.push_back((S32)first[i]);
                merged_count.push_back((S32)count[i]);
            }
        }
    }
}
