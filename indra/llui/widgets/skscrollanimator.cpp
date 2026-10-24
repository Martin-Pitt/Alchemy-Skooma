/**
 * @file skscrollanimator.cpp
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

#include "linden_common.h"

#include "skscrollanimator.h"

namespace
{
    // Chrome's ease-in-out, cubic-bezier(0.42, 0, 0.58, 1), whose first control point rises with the initial slope.
    constexpr F32 X1 = 0.42f;
    constexpr F32 X2 = 0.58f;
    constexpr F32 MAX_SLOPE = 1000.f;
    constexpr F32 VELOCITY_DURATION_BOUND = 2.5f;

    F32 bezier(F32 s, F32 p1, F32 p2)
    {
        const F32 r = 1.f - s;
        return 3.f * r * r * s * p1 + 3.f * r * s * s * p2 + s * s * s;
    }

    F32 bezier_derivative(F32 s, F32 p1, F32 p2)
    {
        const F32 r = 1.f - s;
        return 3.f * r * r * p1 + 6.f * r * s * (p2 - p1) + 3.f * s * s * (1.f - p2);
    }

    // The curve's parameter where x is u: Newton's steps, falling back to halving.
    F32 solve_x(F32 u)
    {
        F32 s = u;
        for (S32 i = 0; i < 8; ++i)
        {
            const F32 err = bezier(s, X1, X2) - u;
            if (std::fabs(err) < 1e-6f)
            {
                return s;
            }
            const F32 d = bezier_derivative(s, X1, X2);
            if (d < 1e-6f)
            {
                break;
            }
            s -= err / d;
        }
        F32 lo = 0.f;
        F32 hi = 1.f;
        s = u;
        for (S32 i = 0; i < 30; ++i)
        {
            (bezier(s, X1, X2) < u ? lo : hi) = s;
            s = 0.5f * (lo + hi);
        }
        return s;
    }
}

F32 SKScrollAnimator::classicDuration(F32 distance, F32 scale)
{
    return llclamp(14.f - std::fabs(distance) / 60.f, 6.f, 12.f) / 60.f * scale;
}

F32 SKScrollAnimator::classicCurve(F32 u, F32 slope)
{
    if (u <= 0.f)
    {
        return 0.f;
    }
    if (u >= 1.f)
    {
        return 1.f;
    }
    return bezier(solve_x(u), slope * X1, 1.f);
}

F32 SKScrollAnimator::classicSlope(F32 u, F32 slope)
{
    if (u < 0.f || u >= 1.f)
    {
        return 0.f;
    }
    const F32 s = solve_x(u);
    const F32 dx = bezier_derivative(s, X1, X2);
    return dx > 1e-6f ? bezier_derivative(s, slope * X1, 1.f) / dx : 0.f;
}

F32 SKScrollAnimator::velocity(F64 now) const
{
    if (!mAnimating)
    {
        return 0.f;
    }
    const F32 t = (F32)(now - mStartTime);
    if (mMode == WARP)
    {
        return mDirection * mTrip.speedAt(t);
    }
    if (mDuration <= 0.f)
    {
        return 0.f;
    }
    return classicSlope(t / mDuration, mSlope) * (mTarget - mStart) / mDuration;
}

F32 SKScrollAnimator::tick(F64 now)
{
    if (!mAnimating)
    {
        return mPos;
    }
    const F32 t = (F32)(now - mStartTime);
    if (mMode == WARP)
    {
        if (t >= mTrip.duration())
        {
            jumpTo(mTarget);
        }
        else
        {
            mPos = mStart + mDirection * mTrip.distanceAt(t);
        }
        return mPos;
    }
    if (mDuration <= 0.f || t >= mDuration)
    {
        jumpTo(mTarget);
        return mPos;
    }
    mPos = mStart + (mTarget - mStart) * classicCurve(t / mDuration, mSlope);
    return mPos;
}

void SKScrollAnimator::jumpTo(F32 pos)
{
    mPos = pos;
    mTarget = pos;
    mStart = pos;
    mAnimating = false;
}

void SKScrollAnimator::clamp(F32 lo, F32 hi)
{
    if (hi < lo)
    {
        hi = lo;
    }
    if (mPos < lo || mPos > hi || mTarget < lo || mTarget > hi)
    {
        jumpTo(llclamp(mPos, lo, hi));
    }
}

void SKScrollAnimator::addDelta(F32 delta, bool precise, F64 now, F32 lo, F32 hi, const Config& config)
{
    if (hi < lo)
    {
        hi = lo;
    }
    const F32 here = tick(now);
    if (precise || !config.mSmooth)
    {
        // A precise device moves the content itself, so a glide under way gives way to it.
        jumpTo(llclamp(here + delta, lo, hi));
        return;
    }
    const F32 target = llclamp(mTarget + delta, lo, hi);
    if (target == mTarget)
    {
        return;
    }
    const F32 speed = velocity(now);
    const bool was_animating = mAnimating;
    mTarget = target;
    if (mMode != config.mMode || !was_animating)
    {
        mMode = config.mMode;
        mStart = here;
        mStartTime = now;
        mAnimating = true;
        if (mMode == WARP)
        {
            mDirection = target >= here ? 1.f : -1.f;
            mTrip = SKWarp::Trip::make(config.mWarp, std::fabs(target - here));
        }
        else
        {
            startClassic(0.f, config.mDurationScale);
        }
        return;
    }
    if (mMode == WARP)
    {
        startWarp(now);
    }
    else
    {
        mStart = here;
        mStartTime = now;
        startClassic(speed, config.mDurationScale);
    }
}

void SKScrollAnimator::startClassic(F32 velocity, F32 scale)
{
    // Chrome's retarget: from where the glide is, toward the new target, keeping its speed as the curve's
    // initial slope and finishing no later than 2.5 times the distance at that speed. A reversal starts over.
    const F32 remaining = mTarget - mStart;
    const F32 distance = std::fabs(remaining);
    mDuration = classicDuration(distance, scale);
    mSlope = 0.f;
    if (distance <= 0.f || mDuration <= 0.f)
    {
        jumpTo(mTarget);
        return;
    }
    if (velocity != 0.f && (velocity > 0.f) == (remaining > 0.f))
    {
        const F32 speed = std::fabs(velocity);
        mDuration = llmin(mDuration, VELOCITY_DURATION_BOUND * distance / speed);
        mSlope = llclamp(speed * mDuration / distance, -MAX_SLOPE, MAX_SLOPE);
    }
}

void SKScrollAnimator::startWarp(F64 now)
{
    // The running trip keeps its profile; the retarget works its cut and brake out against the new target.
    const F32 t = (F32)(now - mStartTime);
    const SKWarp::Retarget r = SKWarp::retarget(mTrip, t, (mTarget - mStart) * mDirection);
    mStart += mDirection * r.mOffset;
    if (!r.mForward)
    {
        mDirection = -mDirection;
    }
    mTrip = r.mTrip;
    mStartTime = now - r.mSeconds;
}
