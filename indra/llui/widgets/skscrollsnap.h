/**
 * @file skscrollsnap.h
 * @brief How far a wheel notch moves a list of whole items, File Explorer style.
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

#ifndef SK_SKSCROLLSNAP_H
#define SK_SKSCROLLSNAP_H

#include "llmath.h"

namespace SKScrollSnap
{
    /// The distance a notch aims for, as the platform's 100 pixels.
    constexpr F32 NOTCH_PIXELS = 100.f;
    constexpr F32 EDGE_EPSILON = 0.001f;

    // Items a notch moves: short rows 3, tall items fewer, never none.
    inline S32 itemsPerNotch(F32 item_pixels, F32 notch_pixels = NOTCH_PIXELS)
    {
        if (item_pixels <= 0.f)
        {
            return 3;
        }
        return llclamp((S32)ll_round(notch_pixels / item_pixels), 1, 3);
    }

    // Where the view's top lands, in items, after moving `items` items from top `pos` in a view `page` items tall.
    // Down aligns the bottom edge to the end of an item, a partly shown edge item counting as one; up mirrors it.
    inline F32 target(F32 pos, F32 page, F32 max, S32 items, bool down)
    {
        if (items <= 0)
        {
            return llclamp(pos, 0.f, max);
        }
        F32 top;
        if (down)
        {
            const F32 bottom = floorf(pos + page + EDGE_EPSILON) + (F32)items;
            top = bottom - page;
        }
        else
        {
            top = ceilf(pos - EDGE_EPSILON) - (F32)items;
        }
        return llclamp(top, 0.f, llmax(max, 0.f));
    }
}

#endif // SK_SKSCROLLSNAP_H
