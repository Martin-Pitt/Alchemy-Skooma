/**
 * @file skvizkey_test.cpp
 * @brief The hover key's LLSD round trip and row matching.
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

#include "skvizkey.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    TEST_CASE("skvizkey: every field survives LLSD and an empty key stays empty", "[skvizkey]")
    {
        SKVizKey k;
        CHECK(k.empty());
        CHECK(SKVizKey::fromLLSD(k.toLLSD()).empty());

        k.scale     = "update-type";
        k.category  = 3;
        k.value     = 2.5;
        k.id        = 0xFEDCBA9876543210ull;
        k.state     = "sealed";
        k.colour    = LLColor4(0.25f, 0.5f, 1.f, 1.f);
        k.hasColour = true;
        const SKVizKey back = SKVizKey::fromLLSD(k.toLLSD());
        CHECK(back == k);
        CHECK(back.id == 0xFEDCBA9876543210ull);
    }

    TEST_CASE("skvizkey: rows match by scale and class or state and never by colour", "[skvizkey]")
    {
        SKVizKey a;
        a.scale    = "s";
        a.category = 1;
        SKVizKey b = a;
        b.value    = 7.0;
        b.colour   = LLColor4::red;
        b.hasColour = true;
        CHECK(a.matchesRow(b));
        b.category = 2;
        CHECK_FALSE(a.matchesRow(b));
        SKVizKey s1, s2;
        s1.scale = s2.scale = "s";
        s1.state = s2.state = "blend";
        CHECK(s1.matchesRow(s2));
        s2.scale = "t";
        CHECK_FALSE(s1.matchesRow(s2));
    }
}
