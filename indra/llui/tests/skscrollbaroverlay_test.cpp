/**
 * @file skscrollbaroverlay_test.cpp
 * @brief Tests for the overlay scroll bar's fade and widening arithmetic.
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

#include "linden_common.h"

#include "skscrollbaroverlay.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
namespace O = SKScrollbarOverlay;

TEST_CASE("skscrollbaroverlay: fades in, holds through the delay, then fades out", "[skscrollbaroverlay]")
{
    CHECK(O::alpha(0.f, 0.f, 0.15f, 1.f, 0.25f) == Approx(0.f));
    CHECK(O::alpha(0.075f, 0.f, 0.15f, 1.f, 0.25f) == Approx(0.5f));
    CHECK(O::alpha(0.5f, 0.5f, 0.15f, 1.f, 0.25f) == Approx(1.f));
    CHECK(O::alpha(5.f, 1.f, 0.15f, 1.f, 0.25f) == Approx(1.f));
    CHECK(O::alpha(5.f, 1.125f, 0.15f, 1.f, 0.25f) == Approx(0.5f));
    CHECK(O::alpha(5.f, 2.f, 0.15f, 1.f, 0.25f) == Approx(0.f));
}

TEST_CASE("skscrollbaroverlay: zero times show and hide at once", "[skscrollbaroverlay]")
{
    CHECK(O::alpha(0.f, 0.f, 0.f, 1.f, 0.25f) == Approx(1.f));
    CHECK(O::alpha(5.f, 1.01f, 0.15f, 1.f, 0.f) == Approx(0.f));
}

TEST_CASE("skscrollbaroverlay: widening steps toward its target and stays in range", "[skscrollbaroverlay]")
{
    CHECK(O::stepWiden(0.f, true, 0.06f, 0.12f) == Approx(0.5f));
    CHECK(O::stepWiden(0.9f, true, 0.06f, 0.12f) == Approx(1.f));
    CHECK(O::stepWiden(0.1f, false, 0.06f, 0.12f) == Approx(0.f));
    CHECK(O::stepWiden(0.f, true, 0.01f, 0.f) == Approx(1.f));
}

TEST_CASE("skscrollbaroverlay: width eases between thin and wide", "[skscrollbaroverlay]")
{
    CHECK(O::width(0.f, 4.f, 10.f) == Approx(4.f));
    CHECK(O::width(0.5f, 4.f, 10.f) == Approx(7.f));
    CHECK(O::width(1.f, 4.f, 10.f) == Approx(10.f));
    CHECK(O::width(0.1f, 4.f, 10.f) < 4.f + 0.1f * 6.f);
}

TEST_CASE("skscrollbaroverlay: only a document longer than its page scrolls", "[skscrollbaroverlay]")
{
    CHECK(O::scrollable(200, 100));
    CHECK_FALSE(O::scrollable(100, 100));
    CHECK_FALSE(O::scrollable(0, 0));
}
