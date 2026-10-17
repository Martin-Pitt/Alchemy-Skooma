/**
 * @file skmodalviewmath.cpp
 * @brief The modal view's claim order and its camera snapshot maths, free of the viewer.
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

#include "skmodalviewmath.h"

#include <algorithm>

namespace skmodalview
{
    void Claims::claim(const void* key, S32 priority)
    {
        release(key);
        mClaims.push_back({ key, priority, mNextSequence++ });
    }

    bool Claims::release(const void* key)
    {
        auto it = std::find_if(mClaims.begin(), mClaims.end(), [key](const Claim& c) { return c.mKey == key; });
        if (it == mClaims.end())
        {
            return false;
        }
        mClaims.erase(it);
        return true;
    }

    bool Claims::holds(const void* key) const
    {
        return std::any_of(mClaims.begin(), mClaims.end(), [key](const Claim& c) { return c.mKey == key; });
    }

    const void* Claims::top() const
    {
        const Claim* best = nullptr;
        for (const Claim& c : mClaims)
        {
            if (!best || c.mPriority > best->mPriority || (c.mPriority == best->mPriority && c.mSequence > best->mSequence))
            {
                best = &c;
            }
        }
        return best ? best->mKey : nullptr;
    }

    std::vector<const void*> Claims::ordered() const
    {
        std::vector<Claim> sorted = mClaims;
        std::sort(sorted.begin(), sorted.end(), [](const Claim& a, const Claim& b) {
            return a.mPriority != b.mPriority ? a.mPriority > b.mPriority : a.mSequence > b.mSequence;
        });
        std::vector<const void*> keys;
        keys.reserve(sorted.size());
        for (const Claim& c : sorted)
        {
            keys.push_back(c.mKey);
        }
        return keys;
    }
} // namespace skmodalview
