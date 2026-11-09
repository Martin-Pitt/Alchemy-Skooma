/**
 * @file skticks.cpp
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

#include "linden_common.h"

#include "skticks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

std::string SKTicks::format(F64 value, S32 decimals)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    std::string s(buf);
    if (std::atof(buf) == 0.0 && !s.empty() && s[0] == '-')
    {
        s.erase(0, 1);
    }
    return s;
}

SKTicks SKTicks::make(F64 lo, F64 hi, S32 bar_px, S32 char_px, S32 gap, S32 max_ticks)
{
    SKTicks   out;
    const F64 range = hi - lo;
    if (!(range > 0.0) || bar_px <= 0)
    {
        return out;
    }
    static const F64 MANTISSAS[] = { 1.0, 2.0, 2.5, 5.0 };
    const S32        k0          = (S32)std::floor(std::log10(range)) - 2;
    for (S32 k = k0; k < k0 + 4; ++k)
    {
        for (F64 m : MANTISSAS)
        {
            const F64 step  = m * std::pow(10.0, k);
            const S32 d     = std::max(0, -(S32)std::floor(std::log10(step) + 1e-12)) + (m == 2.5 ? 1 : 0);
            const F64 first = std::ceil(lo / step - 1e-9);
            const F64 last  = std::floor(hi / step + 1e-9);
            const S32 n     = (S32)(last - first) + 1;
            if (n < 2 || n > max_ticks)
            {
                continue;
            }
            size_t widest = 0;
            std::vector<std::string> labels;
            for (S32 i = 0; i < n; ++i)
            {
                labels.push_back(format((first + i) * step, d));
                widest = std::max(widest, labels.back().size());
            }
            if (step / range * bar_px >= (F64)(widest * char_px + gap))
            {
                out.step     = step;
                out.decimals = d;
                out.labels   = std::move(labels);
                for (S32 i = 0; i < n; ++i)
                {
                    out.values.push_back((first + i) * step);
                }
                return out;
            }
        }
    }
    out.step     = range;
    out.decimals = std::max(0, -(S32)std::floor(std::log10(range) + 1e-12));
    out.values   = { lo, hi };
    out.labels   = { format(lo, out.decimals), format(hi, out.decimals) };
    return out;
}

SKTicks SKTicks::decades(F64 lo, F64 hi)
{
    SKTicks out;
    if (!(lo > 0.0) || !(hi > lo))
    {
        return out;
    }
    for (S32 k = (S32)std::ceil(std::log10(lo) - 1e-9); k <= (S32)std::floor(std::log10(hi) + 1e-9); ++k)
    {
        const F64 v = std::pow(10.0, k);
        out.values.push_back(v);
        out.labels.push_back(format(v, std::max(0, -k)));
    }
    return out;
}
