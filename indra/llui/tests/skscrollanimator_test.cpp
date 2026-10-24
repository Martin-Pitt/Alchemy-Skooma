/**
 * @file skscrollanimator_test.cpp
 * @brief The smooth scroll's glide: Chrome's durations and retarget, the warp, and precise input
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
#include "skscrollanimator.h"
#include "skscrolldistance.h"

using Catch::Matchers::WithinAbs;

namespace
{
    constexpr F32 LO = 0.f;
    constexpr F32 HI = 100000.f;

    SKScrollAnimator::Config classic()
    {
        return SKScrollAnimator::Config();
    }

    SKScrollAnimator::Config warp()
    {
        SKScrollAnimator::Config config;
        config.mMode = SKScrollAnimator::WARP;
        return config;
    }
}

TEST_CASE("SKScrollAnimator: Chrome's wheel durations", "[SKScrollAnimator]")
{
    CHECK_THAT(SKScrollAnimator::classicDuration(120.f), WithinAbs(0.2f, 1e-6f));
    CHECK_THAT(SKScrollAnimator::classicDuration(60.f), WithinAbs(0.2f, 1e-6f));
    CHECK_THAT(SKScrollAnimator::classicDuration(300.f), WithinAbs(9.f / 60.f, 1e-6f));
    CHECK_THAT(SKScrollAnimator::classicDuration(480.f), WithinAbs(0.1f, 1e-6f));
    CHECK_THAT(SKScrollAnimator::classicDuration(5000.f), WithinAbs(0.1f, 1e-6f));
    CHECK_THAT(SKScrollAnimator::classicDuration(-120.f, 2.f), WithinAbs(0.4f, 1e-6f));
}

TEST_CASE("SKScrollAnimator: the ease-in-out curve is Chrome's", "[SKScrollAnimator]")
{
    CHECK(SKScrollAnimator::classicCurve(0.f, 0.f) == 0.f);
    CHECK(SKScrollAnimator::classicCurve(1.f, 0.f) == 1.f);
    CHECK_THAT(SKScrollAnimator::classicCurve(0.5f, 0.f), WithinAbs(0.5f, 1e-5f));
    // cubic-bezier(0.42, 0, 0.58, 1) is symmetric about its middle.
    for (F32 u : { 0.1f, 0.25f, 0.4f })
    {
        CHECK_THAT(SKScrollAnimator::classicCurve(u, 0.f) + SKScrollAnimator::classicCurve(1.f - u, 0.f), WithinAbs(1.f, 1e-5f));
    }
    CHECK_THAT(SKScrollAnimator::classicSlope(0.f, 0.f), WithinAbs(0.f, 1e-6f));
    CHECK_THAT(SKScrollAnimator::classicSlope(0.f, 3.f), WithinAbs(3.f, 1e-4f));
}

TEST_CASE("SKScrollAnimator: a notch glides onto its target and stops", "[SKScrollAnimator]")
{
    SKScrollAnimator a;
    a.addDelta(100.f, false, 10.0, LO, HI, classic());
    CHECK(a.isAnimating());
    CHECK(a.target() == 100.f);
    F32 last = 0.f;
    for (S32 i = 1; i < 12; ++i)
    {
        const F32 pos = a.tick(10.0 + i / 60.0);
        CHECK(pos >= last);
        CHECK(pos <= 100.f);
        last = pos;
    }
    CHECK(a.tick(10.21) == 100.f);
    CHECK_FALSE(a.isAnimating());
}

TEST_CASE("SKScrollAnimator: another notch while gliding keeps the speed it had", "[SKScrollAnimator]")
{
    for (F64 at : { 0.03, 0.08, 0.15 })
    {
        SKScrollAnimator a;
        a.addDelta(100.f, false, 0.0, LO, HI, classic());
        a.tick(at);
        const F32 before = a.velocity(at);
        const F32 pos = a.position();
        a.addDelta(100.f, false, at, LO, HI, classic());
        CHECK(a.position() == pos);
        CHECK(a.target() == 200.f);
        CHECK_THAT(a.velocity(at), WithinAbs(before, std::fabs(before) * 0.01f + 1e-2f));
        // It still arrives, and never overshoots.
        F32 last = pos;
        for (S32 i = 1; i <= 30; ++i)
        {
            const F32 p = a.tick(at + i / 60.0);
            CHECK(p >= last - 1e-3f);
            CHECK(p <= 200.f + 1e-3f);
            last = p;
        }
        CHECK(a.position() == 200.f);
    }
}

TEST_CASE("SKScrollAnimator: spinning a wheel never stalls between notches", "[SKScrollAnimator]")
{
    SKScrollAnimator a;
    F32 last = 0.f;
    for (S32 frame = 0; frame < 60; ++frame)
    {
        const F64 now = frame / 60.0;
        if (frame % 3 == 0 && frame < 45)
        {
            a.addDelta(100.f, false, now, LO, HI, classic());
        }
        const F32 pos = a.tick(now);
        if (frame > 0 && frame < 45)
        {
            CHECK(pos > last);
        }
        last = pos;
    }
}

TEST_CASE("SKScrollAnimator: a reversal starts over from rest", "[SKScrollAnimator]")
{
    SKScrollAnimator a;
    a.addDelta(300.f, false, 0.0, LO, HI, classic());
    a.tick(0.05);
    const F32 pos = a.position();
    a.addDelta(-600.f, false, 0.05, LO, HI, classic());
    CHECK(a.target() == 0.f);
    CHECK_THAT(a.velocity(0.05), WithinAbs(0.f, 1e-3f));
    a.tick(0.06);
    CHECK(a.position() < pos);
}

TEST_CASE("SKScrollAnimator: the target stays within the content", "[SKScrollAnimator]")
{
    SKScrollAnimator a;
    a.addDelta(-100.f, false, 0.0, 0.f, 500.f, classic());
    CHECK_FALSE(a.isAnimating());
    CHECK(a.position() == 0.f);
    a.addDelta(800.f, false, 0.0, 0.f, 500.f, classic());
    CHECK(a.target() == 500.f);
    a.clamp(0.f, 200.f);
    CHECK_FALSE(a.isAnimating());
    CHECK(a.position() <= 200.f);
}

TEST_CASE("SKScrollAnimator: precise input and smoothing off move at once", "[SKScrollAnimator]")
{
    SKScrollAnimator a;
    a.addDelta(7.5f, true, 0.0, LO, HI, classic());
    CHECK_FALSE(a.isAnimating());
    CHECK(a.position() == 7.5f);

    // A precise stream takes over from a glide where the glide is.
    a.addDelta(300.f, false, 0.0, LO, HI, classic());
    const F32 here = a.tick(0.05);
    a.addDelta(2.f, true, 0.05, LO, HI, classic());
    CHECK_FALSE(a.isAnimating());
    CHECK(a.position() == here + 2.f);

    SKScrollAnimator::Config off;
    off.mSmooth = false;
    a.addDelta(100.f, false, 0.1, LO, HI, off);
    CHECK_FALSE(a.isAnimating());
    CHECK(a.position() == here + 102.f);
}

TEST_CASE("SKScrollAnimator: the warp takes a notch as a short trip and keeps going when retargeted", "[SKScrollAnimator]")
{
    SKScrollAnimator a;
    a.addDelta(100.f, false, 0.0, LO, HI, warp());
    CHECK(a.isAnimating());
    a.tick(0.179);
    CHECK(a.isAnimating());
    CHECK(a.tick(0.181) == 100.f);
    CHECK_FALSE(a.isAnimating());

    // Retargeted while braking, it ramps up from the speed it has, without a jump.
    SKScrollAnimator b;
    b.addDelta(100.f, false, 0.0, LO, HI, warp());
    b.tick(0.1);
    const F32 pos = b.position();
    const F32 speed = b.velocity(0.1);
    b.addDelta(100.f, false, 0.1, LO, HI, warp());
    CHECK_THAT(b.tick(0.1), WithinAbs(pos, 1e-3f));
    CHECK_THAT(b.velocity(0.1), WithinAbs(speed, speed * 1e-3f + 1e-2f));
    for (S32 i = 1; i < 60; ++i)
    {
        b.tick(0.1 + i / 60.0);
    }
    CHECK(b.position() == 200.f);

    // A long spin cuts out the cruise.
    SKScrollAnimator c;
    c.addDelta(5000.f, false, 0.0, LO, HI, warp());
    CHECK(c.tick(0.045) > 1000.f);
}

TEST_CASE("SKScrollDistance: wheels go by lines, precise devices by their points", "[SKScrollAnimator]")
{
    LLScrollDelta notch(1, 1.f);
    CHECK_THAT(SKScrollDistance::pixels(notch, SKScrollDistance::DEFAULT_LINE, 400.f), WithinAbs(100.f, 1e-4f));
    notch.mLines = 5.f;
    CHECK(SKScrollDistance::pixels(notch, 16.f, 400.f) == 80.f);
    LLScrollDelta page(-1, -1.f);
    page.mPage = true;
    page.mLines = -1.f;
    CHECK(SKScrollDistance::pixels(page, 16.f, 400.f) == -384.f);
    LLScrollDelta pad(0, 0.1f);
    pad.mIsPrecise = true;
    pad.mPixels = 3.5f;
    CHECK(SKScrollDistance::pixels(pad, 16.f, 400.f) == 3.5f);
}
