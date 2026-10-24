/**
 * @file skscrollsnap_test.cpp
 * @brief Lists scroll whole items a notch, the view's edge snapped onto an item's
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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "skscrollsnap.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("Items per notch follow the item's height", "[skscrollsnap]")
{
    CHECK(SKScrollSnap::itemsPerNotch(16.f) == 3);
    CHECK(SKScrollSnap::itemsPerNotch(48.f) == 2);
    CHECK(SKScrollSnap::itemsPerNotch(96.f) == 1);
    CHECK(SKScrollSnap::itemsPerNotch(256.f) == 1);
    CHECK(SKScrollSnap::itemsPerNotch(0.f) == 3);
}

TEST_CASE("Down aligns the bottom edge to an item's end", "[skscrollsnap]")
{
    // A 10 item view at the top of 100 items moves 3 whole items.
    CHECK_THAT(SKScrollSnap::target(0.f, 10.f, 90.f, 3, true), WithinAbs(3.f, 1e-4f));
    // The bottom edge cuts item 12 at 12.4: it counts as one, so items 12, 13 and 14 come in.
    CHECK_THAT(SKScrollSnap::target(2.4f, 10.f, 90.f, 3, true), WithinAbs(5.f, 1e-4f));
    // A partly shown edge item plus 2 more is the "2 files" Explorer moves.
    CHECK_THAT(SKScrollSnap::target(2.4f, 10.f, 90.f, 2, true), WithinAbs(4.f, 1e-4f));
}

TEST_CASE("Up mirrors down on the top edge", "[skscrollsnap]")
{
    CHECK_THAT(SKScrollSnap::target(10.f, 10.f, 90.f, 3, false), WithinAbs(7.f, 1e-4f));
    // The top edge cuts item 7 at 7.6: it counts as one, so items 7, 6 and 5 come in.
    CHECK_THAT(SKScrollSnap::target(7.6f, 10.f, 90.f, 3, false), WithinAbs(5.f, 1e-4f));
    // Down then up from an aligned position comes back.
    const F32 down = SKScrollSnap::target(20.f, 10.f, 90.f, 2, true);
    CHECK_THAT(SKScrollSnap::target(down, 10.f, 90.f, 2, false), WithinAbs(20.f, 1e-4f));
}

TEST_CASE("Targets stay within the list", "[skscrollsnap]")
{
    CHECK_THAT(SKScrollSnap::target(89.f, 10.f, 90.f, 3, true), WithinAbs(90.f, 1e-4f));
    CHECK_THAT(SKScrollSnap::target(1.f, 10.f, 90.f, 3, false), WithinAbs(0.f, 1e-4f));
    // A list shorter than its view does not move.
    CHECK_THAT(SKScrollSnap::target(0.f, 10.f, 0.f, 3, true), WithinAbs(0.f, 1e-4f));
}
