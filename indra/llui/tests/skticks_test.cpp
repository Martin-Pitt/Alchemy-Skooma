/**
 * @file skticks_test.cpp
 * @brief A legend ramp's ticks against the Python oracle's sweep.
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

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    struct SKTickCase
    {
        F64         lo, hi;
        S32         bar_px, char_px;
        F64         step;
        S32         decimals, count;
        F64         first;
        const char* first_label;
    };

#include "skticks_oracle.inc"

    TEST_CASE("skticks: the picker matches the oracle across ranges and widths", "[skticks]")
    {
        for (const SKTickCase& c : TICK_CASES)
        {
            CAPTURE(c.lo, c.hi, c.bar_px, c.char_px);
            const SKTicks t = SKTicks::make(c.lo, c.hi, c.bar_px, c.char_px);
            CHECK(std::fabs(t.step - c.step) <= 1e-12 * std::fabs(c.step));
            CHECK(t.decimals == c.decimals);
            REQUIRE((S32)t.values.size() == c.count);
            CHECK(std::fabs(t.values[0] - c.first) <= 1e-9);
            CHECK(t.labels[0] == c.first_label);
        }
    }

    TEST_CASE("skticks: labels of adjacent ticks never come closer than the gap", "[skticks]")
    {
        for (S32 px = 20; px <= 400; px += 7)
        {
            const SKTicks t = SKTicks::make(0.0, 37.0, px, 6);
            for (size_t i = 0; i + 1 < t.values.size(); ++i)
            {
                const F64 spacing = (t.values[i + 1] - t.values[i]) / 37.0 * px;
                CHECK(spacing >= (F64)(std::max(t.labels[i].size(), t.labels[i + 1].size()) * 6 + 4));
            }
        }
    }

    TEST_CASE("skticks: decades and negative zero", "[skticks]")
    {
        const SKTicks d = SKTicks::decades(1.0, 1000.0);
        REQUIRE(d.values.size() == 4);
        CHECK(d.labels[3] == "1000");
        CHECK(SKTicks::format(-0.0001, 2) == "0.00");
        CHECK(SKTicks::make(1.0, 1.0, 100, 6).values.empty());
    }
}
