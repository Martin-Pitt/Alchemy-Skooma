/**
 * @file sktextellipsis_bench.cpp
 * @brief What a middle cut costs a list row per frame: SKTextEllipsis on a
 *        label already in the shape cache.
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

#include "../sktextellipsis.h"

#include "sktextellipsis_fixture.h"

#include "altestbenchmark.h"

#include <string>

namespace
{
    TEST_CASE("sktextellipsis_bench: a forty character label cut in the middle", "[sktextellipsis_bench][!benchmark]")
    {
        ll_test::skipUnlessOptimised();
        const LLFontGL* font = ll_test::ellipsisTestFont();
        if (!font)
        {
            SKIP("the source tree's fonts.xml is not where the build said");
        }
        const std::string label = "Hair v2 - RIGGED (no copy) #12 Lumipro X";
        const F32 max_pixels = font->getWidthF32(label) * 0.6f;
        std::string out;
        out.reserve(label.size() + SKTextEllipsis::ELLIPSIS.size());
        SKTextEllipsis::shorten(*font, label, max_pixels, LLFontGL::ELLIPSIS_MIDDLE, out);

        BENCHMARK("middle cut, forty characters, shapes cached")
        {
            return SKTextEllipsis::shorten(*font, label, max_pixels, LLFontGL::ELLIPSIS_MIDDLE, out).drawnBytes();
        };
        BENCHMARK("end cut, forty characters, shapes cached")
        {
            return SKTextEllipsis::shorten(*font, label, max_pixels, LLFontGL::ELLIPSIS_END, out).drawnBytes();
        };
        BENCHMARK("label that fits, forty characters, shapes cached")
        {
            return SKTextEllipsis::shorten(*font, label, max_pixels * 2.f, LLFontGL::ELLIPSIS_MIDDLE, out).drawnBytes();
        };
    }
}
