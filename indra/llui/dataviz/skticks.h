/**
 * @file skticks.h
 * @brief Round tick values for a legend's ramp, spaced so their labels never touch.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_TICKS_H
#define SK_TICKS_H

#include "stdtypes.h"

#include <string>
#include <vector>

// LLStatBar's picker widens its range to take in zero, which a legend's domain must not do.
struct SKTicks
{
    F64                      step     = 0.0;
    S32                      decimals = 0;
    std::vector<F64>         values;
    std::vector<std::string> labels;

    // Steps of 1, 2, 2.5 or 5 times a power of ten, the finest whose monospace labels keep gap px apart.
    static SKTicks make(F64 lo, F64 hi, S32 bar_px, S32 char_px, S32 gap = 4, S32 max_ticks = 5);
    // Powers of ten within [lo, hi], for a log scale.
    static SKTicks decades(F64 lo, F64 hi);
    static std::string format(F64 value, S32 decimals);
};

#endif // SK_TICKS_H
