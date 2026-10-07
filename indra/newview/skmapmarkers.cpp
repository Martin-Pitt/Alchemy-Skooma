/**
 * @file skmapmarkers.cpp
 * @brief Stable short codes for mini-map allies, and the marker radius they need.
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

#include "skmapmarkers.h"

#include "llmath.h"

#include <cmath>

namespace skmap
{
    std::string codeString(U32 index)
    {
        if (index < 9)
        {
            return std::string(1, static_cast<char>('1' + index));
        }
        S64 n = static_cast<S64>(index) - 9;
        std::string code;
        do
        {
            code.insert(code.begin(), static_cast<char>('A' + n % 26));
            n = n / 26 - 1;
        } while (n >= 0);
        return code;
    }

    U32 MarkerCodes::codeFor(const LLUUID& id, F64 now)
    {
        auto it = mEntries.find(id);
        if (it != mEntries.end())
        {
            it->second.mLastSeen = now;
            return it->second.mIndex;
        }
        release(now);
        U32 index = 0;
        while (mUsed.count(index))
        {
            ++index;
        }
        mUsed.insert(index);
        mEntries[id] = { index, now };
        return index;
    }

    void MarkerCodes::release(F64 now)
    {
        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            if (now - it->second.mLastSeen > mHold)
            {
                mUsed.erase(it->second.mIndex);
                it = mEntries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void MarkerCodes::clear()
    {
        mEntries.clear();
        mUsed.clear();
    }

    F32 markerRadius(bool chevron, bool coded, U32 code_length, F32 text_width, F32 font_px, F32 padding_px)
    {
        const F32 single = font_px * 0.5f + padding_px;
        if (chevron)
        {
            return single;
        }
        if (!coded)
        {
            return 3.f;
        }
        if (code_length <= 1)
        {
            return single;
        }
        return llmax(single, std::hypot(text_width * 0.5f, font_px * 0.36f) + padding_px);
    }
}
