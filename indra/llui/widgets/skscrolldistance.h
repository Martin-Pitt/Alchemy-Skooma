/**
 * @file skscrolldistance.h
 * @brief How far a scroll gesture moves a widget's content, and the smooth scroll's settings.
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

#ifndef SK_SKSCROLLDISTANCE_H
#define SK_SKSCROLLDISTANCE_H

#include "llscrolldelta.h"
#include "skscrollanimator.h"

namespace SKScrollDistance
{
    /// A line for content with none of its own: Chrome's, so a notch of three lines is 100 pixels.
    constexpr F32 DEFAULT_LINE = LLScrollDelta::PIXELS_PER_LINE;

    // Pixels a delta moves content whose lines are line tall (or characters wide) in a view page long:
    // a precise device's own points, else the OS's lines per notch, or a page less a line.
    inline F32 pixels(const LLScrollDelta& delta, F32 line, F32 page)
    {
        if (delta.mIsPrecise)
        {
            return delta.mPixels;
        }
        if (delta.mPage)
        {
            return delta.lines() * llmax(page - line, line);
        }
        return delta.lines() * line;
    }

    // The line a wheel moves content whose own line is content_line: Chrome's, unless the real lines are asked for.
    F32 lineFor(F32 content_line);

    // The smooth scroll's settings, as the viewer has them now.
    SKScrollAnimator::Config config();
    // The clock the animators run on, in seconds.
    F64 now();
}

#endif // SK_SKSCROLLDISTANCE_H
