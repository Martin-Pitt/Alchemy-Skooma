/**
 * @file skmapmarkers.h
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

#ifndef SK_MAPMARKERS_H
#define SK_MAPMARKERS_H

#include "lluuid.h"

#include <map>
#include <set>
#include <string>

namespace skmap
{
    constexpr F32 CODE_HOLD_S = 120.f;

    /// 0 -> "1" ... 8 -> "9", then bijective base 26: 9 -> "A" ... 34 -> "Z", 35 -> "AA".
    std::string codeString(U32 index);

    /// Each coded avatar keeps the lowest index free when first seen, for as long as it is
    /// seen and a hold after; released indices are reused lowest first.
    class MarkerCodes
    {
    public:
        void setHold(F32 seconds) { mHold = seconds; }
        /// The avatar's index, seen now.
        U32 codeFor(const LLUUID& id, F64 now);
        /// Release every index whose holder has been unseen longer than the hold.
        void release(F64 now);
        bool has(const LLUUID& id) const { return mEntries.count(id) > 0; }
        void clear();

    private:
        struct Entry
        {
            U32 mIndex = 0;
            F64 mLastSeen = 0.0;
        };
        std::map<LLUUID, Entry> mEntries;
        std::set<U32> mUsed;
        F32 mHold = CODE_HOLD_S;
    };

    /// How a floor difference draws on a dot: how many chevrons, how steep (rise over half width) and how thick.
    struct ChevronStyle
    {
        U8 mCount = 0;
        F32 mSteep = 0.55f;
        F32 mStroke = 0.f;
    };
    /// The chevrons for a floor difference of abs_delta metres: none under bands[0], one from it, two from
    /// bands[1], three from bands[2]; steepness and stroke grow from bands[3] to bands[4] and hold past it.
    /// A band set below the one before it is raised to it.
    ChevronStyle chevronStyle(F32 abs_delta, const F32 bands[5], F32 base_stroke_px);

    /// The study's marker radius: a floor chevron (one, however many bands) and a one-character code
    /// fill a disc of half the font plus the padding; an uncoded dot is 3 px; a longer code grows to fit its text.
    F32 markerRadius(U8 chevrons, F32 chevron_stroke_px, bool coded, U32 code_length, F32 text_width, F32 font_px, F32 padding_px);
}

#endif // SK_MAPMARKERS_H
