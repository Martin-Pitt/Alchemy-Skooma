/**
 * @file skscrollanimator.h
 * @brief One axis of a smooth scroll: wheels glide onto their target, precise devices move at once.
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

#ifndef SK_SKSCROLLANIMATOR_H
#define SK_SKSCROLLANIMATOR_H

#include "skwarpprofile.h"

// The position of one scrolling axis, in the widget's own units (pixels for most). A wheel's delta moves the
// target and the position glides onto it; another notch while gliding retargets without a stutter. A precise
// device's delta, or smoothing turned off, moves the position at once. The widget ticks it as it draws.
class SKScrollAnimator
{
public:
    enum EMode : S32
    {
        CLASSIC = 0, // Chrome's ease-in-out, 100-200 ms, keeping its speed across notches
        WARP = 1,    // SKWarp: ramp, cut out the cruise, brake
    };

    struct Config
    {
        bool mSmooth = true;
        S32 mMode = CLASSIC;
        F32 mDurationScale = 1.f;          // classic
        SKWarp::Profile mWarp = SKWarp::SCROLL; // warp, in pixels
    };

    // Moves the target by delta, held within [lo, hi]; now is in seconds.
    void addDelta(F32 delta, bool precise, F64 now, F32 lo, F32 hi, const Config& config);
    // Advances the glide to now and returns the position.
    F32 tick(F64 now);
    // Stops wherever it is asked to be: a scrollbar drag, a programmatic scroll.
    void jumpTo(F32 pos);
    // Holds position and target within [lo, hi], for content that shrank.
    void clamp(F32 lo, F32 hi);

    bool isAnimating() const { return mAnimating; }
    F32 position() const { return mPos; }
    F32 target() const { return mTarget; }
    // Speed at now in units per second, signed.
    F32 velocity(F64 now) const;

    // Chrome's wheel duration for a glide of distance units: shorter for bigger flicks, 100-200 ms.
    static F32 classicDuration(F32 distance, F32 scale = 1.f);
    // Progress through the classic curve at u of its duration, for an initial slope (0 eases in).
    static F32 classicCurve(F32 u, F32 slope);
    // The curve's slope (progress per unit of u) at u.
    static F32 classicSlope(F32 u, F32 slope);

private:
    void startClassic(F32 velocity, F32 scale);
    void startWarp(F64 now);

    F32 mPos = 0.f;
    F32 mTarget = 0.f;
    F32 mStart = 0.f;
    F64 mStartTime = 0.0;
    bool mAnimating = false;
    S32 mMode = CLASSIC;

    // Classic
    F32 mDuration = 0.f;
    F32 mSlope = 0.f;

    // Warp: the trip runs from mStart along mDirection.
    SKWarp::Trip mTrip;
    F32 mDirection = 1.f;
};

#endif // SK_SKSCROLLANIMATOR_H
