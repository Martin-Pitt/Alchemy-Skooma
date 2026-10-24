/**
 * @file skscrollbaroverlay.h
 * @brief The overlay scroll bar's fade and widening, as pure arithmetic.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin Pitt
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

#ifndef SK_SCROLLBAROVERLAY_H
#define SK_SCROLLBAROVERLAY_H

#include "stdtypes.h"

#include <algorithm>

namespace SKScrollbarOverlay
{
    constexpr F32 THIN_WIDTH = 4.f;
    constexpr F32 WIDE_WIDTH = 10.f;
    constexpr F32 FADE_IN_TIME = 0.15f;
    constexpr F32 WIDEN_TIME = 0.12f;

    inline F32 clamp01(F32 v) { return std::clamp(v, 0.f, 1.f); }

    // Opacity from the seconds since the thumb was last shown from hidden and since its last activity.
    inline F32 alpha(F32 since_shown, F32 since_active, F32 fade_in, F32 delay, F32 fade_out)
    {
        F32 in = fade_in > 0.f ? clamp01(since_shown / fade_in) : 1.f;
        F32 out = 1.f;
        if (since_active > delay)
        {
            out = fade_out > 0.f ? clamp01(1.f - (since_active - delay) / fade_out) : 0.f;
        }
        return std::min(in, out);
    }

    // Moves the widening progress toward wide (1) or thin (0) over duration seconds.
    inline F32 stepWiden(F32 progress, bool wide, F32 dt, F32 duration)
    {
        F32 step = duration > 0.f ? dt / duration : 1.f;
        return clamp01(progress + (wide ? step : -step));
    }

    // Thumb width for a widening progress, eased at both ends.
    inline F32 width(F32 progress, F32 thin, F32 wide)
    {
        F32 p = clamp01(progress);
        return thin + (wide - thin) * p * p * (3.f - 2.f * p);
    }

    inline bool scrollable(S32 doc_size, S32 page_size) { return doc_size > page_size; }
}

#endif // SK_SCROLLBAROVERLAY_H
