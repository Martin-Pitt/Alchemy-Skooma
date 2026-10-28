/**
 * @file skwarpprofile.h
 * @brief A move that speeds up, cuts out its cruise, and brakes onto the target.
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

#ifndef SK_SKWARPPROFILE_H
#define SK_SKWARPPROFILE_H

#include "llmath.h"

// The move ramps up to a top speed, jumps to its braking distance short of the target, and brakes to a stop.
// Speed eases in and out (smoothstep), or with mPower as a power curve from rest that creeps away and slams up to
// speed; a trip shorter than the two phases together lowers the top speed instead, and has no cut.
// Units are the caller's: metres for the camera, pixels for scrolling.
namespace SKWarp
{
    struct Profile
    {
        F32 mRampSeconds;
        F32 mBrakeSeconds;
        F32 mTopSpeed; // units per second
        F32 mPower = 0.f; // speed goes as u^mPower over a phase; 0 is smoothstep

        // The phase's mean speed as a fraction of the top speed.
        constexpr F32 meanSpeed() const { return mPower > 0.f ? 1.f / (mPower + 1.f) : 0.5f; }

        constexpr F32 duration() const { return mRampSeconds + mBrakeSeconds; }
    };

    /// Scrolling by wheel, in pixels; the top speed is the viewer's setting.
    constexpr Profile SCROLL{ 0.04f, 0.14f, 6000.f };

    // Distance covered at u of a phase that eases from rest to unit speed, in units of the phase's length times speed.
    inline F32 eased(const Profile& p, F32 u)
    {
        u = llclamp(u, 0.f, 1.f);
        if (p.mPower > 0.f)
        {
            return std::pow(u, p.mPower + 1.f) / (p.mPower + 1.f);
        }
        return u * u * u - 0.5f * u * u * u * u;
    }

    // Speed at u of a phase that eases from rest to unit speed: the derivative of eased().
    inline F32 shape(const Profile& p, F32 u)
    {
        u = llclamp(u, 0.f, 1.f);
        if (p.mPower > 0.f)
        {
            return std::pow(u, p.mPower);
        }
        return u * u * (3.f - 2.f * u);
    }

    // Fraction of the way from start to target at time seconds, for a trip of length units.
    inline F32 fraction(const Profile& p, F32 seconds, F32 length)
    {
        if (length <= 0.f || seconds >= p.duration())
        {
            return 1.f;
        }
        const F32 speed = llmin(p.mTopSpeed, length / (p.meanSpeed() * p.duration()));
        if (seconds < p.mRampSeconds)
        {
            return speed * p.mRampSeconds * eased(p, seconds / p.mRampSeconds) / length;
        }
        const F32 left = speed * p.mBrakeSeconds * eased(p, (p.duration() - seconds) / p.mBrakeSeconds);
        return 1.f - left / length;
    }

    // One trip of a profile that may start moving: it ramps from its start speed to its top speed, cuts out the
    // cruise if it reached the profile's top speed with distance to spare, and brakes onto the target.
    // From rest it is exactly fraction() times the length.
    struct Trip
    {
        Profile mProfile{ 0.f, 0.f, 0.f };
        F32 mLength = 0.f;     // start to target, never negative
        F32 mStartSpeed = 0.f; // speed at the start
        F32 mSpeed = 0.f;      // speed at the end of the ramp
        F32 mRamp = 0.f;       // seconds
        F32 mBrake = 0.f;      // seconds
        bool mCut = false;     // jumps from the ramp's end to the brake's start

        F32 duration() const { return mRamp + mBrake; }

        static Trip make(const Profile& p, F32 length, F32 start_speed = 0.f)
        {
            Trip trip;
            trip.mProfile = p;
            if (length <= 0.f)
            {
                return trip;
            }
            trip.mLength = length;
            const F32 m = p.meanSpeed();
            const F32 v0 = llclamp(start_speed, 0.f, p.mTopSpeed);
            trip.mStartSpeed = v0;
            auto ramp_seconds = [&](F32 v) { return v > 0.f ? p.mRampSeconds * (1.f - v0 / v) : 0.f; };
            auto reach = [&](F32 v) { return ramp_seconds(v) * (v0 + (v - v0) * m) + p.mBrakeSeconds * v * m; };

            F32 v = p.mTopSpeed;
            if (reach(v) < length)
            {
                trip.mCut = true;
            }
            else if (v0 <= 0.f)
            {
                v = length / (m * p.duration());
            }
            else if (p.mBrakeSeconds * v0 * m >= length)
            {
                // Too close to even brake from here at the profile's rate: brake harder, over the distance left.
                trip.mSpeed = v0;
                trip.mBrake = length / (v0 * m);
                return trip;
            }
            else
            {
                // reach() grows with the top speed, so halve onto the speed that just arrives.
                F32 lo = v0;
                F32 hi = p.mTopSpeed;
                for (S32 i = 0; i < 40; ++i)
                {
                    v = 0.5f * (lo + hi);
                    (reach(v) < length ? lo : hi) = v;
                }
                v = 0.5f * (lo + hi);
            }
            trip.mSpeed = v;
            trip.mRamp = ramp_seconds(v);
            trip.mBrake = p.mBrakeSeconds;
            return trip;
        }

        // Distance from the start at seconds into the trip.
        F32 distanceAt(F32 seconds) const
        {
            if (seconds <= 0.f || mLength <= 0.f)
            {
                return 0.f;
            }
            if (seconds >= duration())
            {
                return mLength;
            }
            if (seconds < mRamp)
            {
                const F32 u = seconds / mRamp;
                return mRamp * (mStartSpeed * u + (mSpeed - mStartSpeed) * eased(mProfile, u));
            }
            return mLength - mBrake * mSpeed * eased(mProfile, (duration() - seconds) / mBrake);
        }

        // Speed at seconds into the trip.
        F32 speedAt(F32 seconds) const
        {
            if (seconds < 0.f || seconds >= duration() || mLength <= 0.f)
            {
                return 0.f;
            }
            if (seconds < mRamp)
            {
                return mStartSpeed + (mSpeed - mStartSpeed) * shape(mProfile, seconds / mRamp);
            }
            return mSpeed * shape(mProfile, (duration() - seconds) / mBrake);
        }
    };

    // Speed at seconds into a trip of length units from rest: the derivative of fraction() times the length.
    inline F32 speedAt(const Profile& p, F32 seconds, F32 length)
    {
        return Trip::make(p, length).speedAt(seconds);
    }

    // A trip moved onto a new target. The new trip starts mOffset from the old one's start, mSeconds in, and
    // runs the old direction when mForward, the other way otherwise.
    struct Retarget
    {
        Trip mTrip;
        F32 mOffset = 0.f;
        F32 mSeconds = 0.f;
        bool mForward = true;
    };

    // Moves trip, seconds in, onto a target new_length from its start along its direction (negative is behind
    // the start). Still ramping toward a cut, the trip keeps going and only the target moves; otherwise it ramps
    // back up from its current speed, cutting again if it reaches top speed, and brakes onto the new target.
    // A target behind where the trip is restarts from rest.
    inline Retarget retarget(const Trip& trip, F32 seconds, F32 new_length)
    {
        Retarget out;
        const F32 here = trip.distanceAt(seconds);
        const F32 left = new_length - here;
        out.mOffset = here;
        if (left <= 0.f)
        {
            out.mForward = false;
            out.mTrip = Trip::make(trip.mProfile, -left);
            return out;
        }
        if (trip.mCut && seconds < trip.mRamp)
        {
            const Trip longer = Trip::make(trip.mProfile, new_length, trip.mStartSpeed);
            if (longer.mCut)
            {
                out.mTrip = longer;
                out.mOffset = 0.f;
                out.mSeconds = seconds;
                return out;
            }
        }
        out.mTrip = Trip::make(trip.mProfile, left, trip.speedAt(seconds));
        return out;
    }
}

#endif // SK_SKWARPPROFILE_H
