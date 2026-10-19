/**
 * @file skcamerawarp.h
 * @brief A camera move that speeds up, cuts out its cruise, and brakes onto the target.
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

#ifndef SK_SKCAMERAWARP_H
#define SK_SKCAMERAWARP_H

#include "llmath.h"

// The move ramps up to a top speed, jumps to its braking distance short of the target, and brakes to a stop.
// Speed eases in and out (smoothstep), so each phase covers half its time at top speed; a trip shorter than
// the two phases together lowers the top speed instead, and has no cut.
namespace SKCameraWarp
{
    struct Profile
    {
        F32 mRampSeconds;
        F32 mBrakeSeconds;
        F32 mTopSpeed; // metres per second

        constexpr F32 duration() const { return mRampSeconds + mBrakeSeconds; }
    };

    /// The mini-map's Cam onto an avatar.
    constexpr Profile AVATAR{ 0.35f, 0.5f, 80.f };
    /// Into the 3D world map, from the world camera up to the map's.
    constexpr Profile WORLD_MAP{ 0.45f, 0.75f, 600.f };
    /// Into the tactical overview; a key still held at the cut makes it a hold, which snaps.
    constexpr Profile TACTICAL{ 0.25f, 0.25f, 150.f };

    // Distance covered at u of a phase that eases from rest to unit speed, in units of the phase's length times speed.
    inline F32 eased(F32 u)
    {
        u = llclamp(u, 0.f, 1.f);
        return u * u * u - 0.5f * u * u * u * u;
    }

    // Fraction of the way from start to target at time seconds, for a trip of length metres.
    inline F32 fraction(const Profile& p, F32 seconds, F32 length)
    {
        if (length <= 0.f || seconds >= p.duration())
        {
            return 1.f;
        }
        const F32 speed = llmin(p.mTopSpeed, length / (0.5f * p.duration()));
        if (seconds < p.mRampSeconds)
        {
            return speed * p.mRampSeconds * eased(seconds / p.mRampSeconds) / length;
        }
        const F32 left = speed * p.mBrakeSeconds * eased((p.duration() - seconds) / p.mBrakeSeconds);
        return 1.f - left / length;
    }
}

#endif // SK_SKCAMERAWARP_H
