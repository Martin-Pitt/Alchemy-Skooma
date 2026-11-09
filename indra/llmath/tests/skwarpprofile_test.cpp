/**
 * @file skwarpprofile_test.cpp
 * @brief The warp profile against the camera's original arithmetic, and its trips and retargets
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
#include "../skwarpprofile.h"

using Catch::Matchers::WithinAbs;

namespace
{
    // The camera's fraction() as it was before it moved into llmath, kept verbatim as the reference.
    struct OldProfile
    {
        F32 mRampSeconds;
        F32 mBrakeSeconds;
        F32 mTopSpeed;
        F32 mPower = 0.f;
        F32 meanSpeed() const { return mPower > 0.f ? 1.f / (mPower + 1.f) : 0.5f; }
        F32 duration() const { return mRampSeconds + mBrakeSeconds; }
    };

    F32 old_eased(const OldProfile& p, F32 u)
    {
        u = llclamp(u, 0.f, 1.f);
        if (p.mPower > 0.f)
        {
            return std::pow(u, p.mPower + 1.f) / (p.mPower + 1.f);
        }
        return u * u * u - 0.5f * u * u * u * u;
    }

    F32 old_fraction(const OldProfile& p, F32 seconds, F32 length)
    {
        if (length <= 0.f || seconds >= p.duration())
        {
            return 1.f;
        }
        const F32 speed = llmin(p.mTopSpeed, length / (p.meanSpeed() * p.duration()));
        if (seconds < p.mRampSeconds)
        {
            return speed * p.mRampSeconds * old_eased(p, seconds / p.mRampSeconds) / length;
        }
        const F32 left = speed * p.mBrakeSeconds * old_eased(p, (p.duration() - seconds) / p.mBrakeSeconds);
        return 1.f - left / length;
    }

    // The camera's profiles: the mini-map's Cam, the world map, and the scroll profile.
    const OldProfile OLD[] = { { 0.35f, 0.5f, 80.f }, { 0.9f, 1.1f, 4000.f, 5.f }, { 0.04f, 0.14f, 6000.f } };
    const SKWarp::Profile NEW[] = { { 0.35f, 0.5f, 80.f }, { 0.9f, 1.1f, 4000.f, 5.f }, SKWarp::SCROLL };
}

TEST_CASE("SKWarp: fraction is the camera's, bit for bit", "[SKWarp]")
{
    for (S32 i = 0; i < 3; ++i)
    {
        for (F32 length : { 0.f, 0.5f, 5.f, 30.f, 100.f, 1000.f, 50000.f })
        {
            for (S32 step = -1; step <= 220; ++step)
            {
                const F32 t = step * NEW[i].duration() / 200.f;
                CHECK(SKWarp::fraction(NEW[i], t, length) == old_fraction(OLD[i], t, length));
            }
        }
    }
}

TEST_CASE("SKWarp: a trip from rest is fraction times its length, and its speed is the derivative", "[SKWarp]")
{
    for (const SKWarp::Profile& p : NEW)
    {
        for (F32 length : { 1.f, 30.f, 100.f, 5000.f, 50000.f })
        {
            const SKWarp::Trip trip = SKWarp::Trip::make(p, length);
            const F32 dt = p.duration() / 200.f; // coarse enough for float distances
            for (S32 step = 1; step < 200; ++step)
            {
                const F32 t = step * p.duration() / 200.f;
                CHECK_THAT(trip.distanceAt(t), WithinAbs(SKWarp::fraction(p, t, length) * length, length * 1e-5f));
                // A full trip jumps at the cut, so check the derivative away from it.
                if (std::fabs(t - trip.mRamp) > 2.f * dt)
                {
                    const F32 numeric = (trip.distanceAt(t + dt) - trip.distanceAt(t - dt)) / (2.f * dt);
                    CHECK_THAT(SKWarp::speedAt(p, t, length), WithinAbs(numeric, trip.mSpeed * 0.03f + 1e-3f));
                }
            }
        }
    }
}

TEST_CASE("SKWarp: a scroll notch is a short trip with no cut; a long spin cuts", "[SKWarp]")
{
    const SKWarp::Trip notch = SKWarp::Trip::make(SKWarp::SCROLL, 100.f);
    CHECK_FALSE(notch.mCut);
    CHECK_THAT(notch.duration(), WithinAbs(0.18f, 1e-5f));
    const SKWarp::Trip spin = SKWarp::Trip::make(SKWarp::SCROLL, 5000.f);
    CHECK(spin.mCut);
    CHECK(spin.mSpeed == SKWarp::SCROLL.mTopSpeed);
}

TEST_CASE("SKWarp: a trip from a moving start is continuous and lands on its length", "[SKWarp]")
{
    for (F32 v0 : { 100.f, 1000.f, 5000.f, 6000.f })
    {
        for (F32 length : { 1.f, 20.f, 100.f, 400.f, 5000.f })
        {
            const SKWarp::Trip trip = SKWarp::Trip::make(SKWarp::SCROLL, length, v0);
            CHECK_THAT(trip.speedAt(0.f), WithinAbs(v0, 1e-2f));
            CHECK(trip.distanceAt(trip.duration()) == length);
            if (!trip.mCut)
            {
                // No cut: the ramp's end meets the brake's start.
                CHECK_THAT(trip.distanceAt(trip.mRamp - 1e-5f), WithinAbs(trip.distanceAt(trip.mRamp), length * 1e-3f + 0.05f));
            }
            F32 last = 0.f;
            for (S32 step = 0; step <= 100; ++step)
            {
                const F32 d = trip.distanceAt(step * trip.duration() / 100.f);
                CHECK(d >= last - 1e-3f);
                last = d;
            }
        }
    }
}

TEST_CASE("SKWarp: retargeting while ramping toward a cut only moves the target", "[SKWarp]")
{
    const SKWarp::Trip trip = SKWarp::Trip::make(SKWarp::SCROLL, 3000.f);
    REQUIRE(trip.mCut);
    const F32 t = trip.mRamp * 0.5f;
    const SKWarp::Retarget r = SKWarp::retarget(trip, t, 3100.f);
    CHECK(r.mForward);
    CHECK(r.mOffset == 0.f);
    CHECK(r.mSeconds == t);
    CHECK(r.mTrip.distanceAt(t) == trip.distanceAt(t));
    CHECK(r.mTrip.speedAt(t) == trip.speedAt(t));
    CHECK(r.mTrip.mLength == 3100.f);
}

TEST_CASE("SKWarp: retargeting while braking ramps back up from the current speed", "[SKWarp]")
{
    for (F32 extra : { 10.f, 100.f, 3000.f })
    {
        const SKWarp::Trip trip = SKWarp::Trip::make(SKWarp::SCROLL, 3000.f);
        const F32 t = trip.mRamp + trip.mBrake * 0.3f;
        const F32 here = trip.distanceAt(t);
        const F32 speed = trip.speedAt(t);
        const SKWarp::Retarget r = SKWarp::retarget(trip, t, 3000.f + extra);
        CHECK(r.mForward);
        CHECK(r.mOffset == here);
        CHECK(r.mSeconds == 0.f);
        CHECK_THAT(r.mTrip.speedAt(0.f), WithinAbs(speed, speed * 1e-4f));
        CHECK_THAT(r.mOffset + r.mTrip.mLength, WithinAbs(3000.f + extra, 1e-2f));
        // Far enough to reach top speed again, it cuts again.
        CHECK(r.mTrip.mCut == (extra >= 3000.f));
    }
}

TEST_CASE("SKWarp: a target behind restarts from rest the other way", "[SKWarp]")
{
    const SKWarp::Trip trip = SKWarp::Trip::make(SKWarp::SCROLL, 100.f);
    const F32 t = trip.duration() * 0.5f;
    const F32 here = trip.distanceAt(t);
    const SKWarp::Retarget r = SKWarp::retarget(trip, t, -100.f);
    CHECK_FALSE(r.mForward);
    CHECK(r.mOffset == here);
    CHECK(r.mTrip.mStartSpeed == 0.f);
    CHECK_THAT(r.mTrip.mLength, WithinAbs(here + 100.f, 1e-3f));
}
