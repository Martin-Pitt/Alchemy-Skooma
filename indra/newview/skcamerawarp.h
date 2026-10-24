/**
 * @file skcamerawarp.h
 * @brief The camera's warp profiles, in metres (the arithmetic is SKWarp's).
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

#include "skwarpprofile.h"

// A camera move that speeds up, cuts out its cruise, and brakes onto the target: SKWarp in metres.
namespace SKCameraWarp
{
    using Profile = SKWarp::Profile;
    using SKWarp::fraction;

    /// The mini-map's Cam onto an avatar.
    constexpr Profile AVATAR{ 0.35f, 0.5f, 80.f };
    /// Into the 3D world map, from the world camera up to the map's.
    constexpr Profile WORLD_MAP{ 0.9f, 1.1f, 4000.f, 5.f };
    /// A camera reset (Escape) warps home rather than smoothing when the camera is further than this from the avatar, in metres.
    constexpr F32 RESET_DISTANCE = 32.f;
}

#endif // SK_SKCAMERAWARP_H
