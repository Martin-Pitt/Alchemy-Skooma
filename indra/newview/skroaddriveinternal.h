/**
 * @file skroaddriveinternal.h
 * @brief The road driver's own declarations, shared by the files it is split into.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef SK_ROADDRIVEINTERNAL_H
#define SK_ROADDRIVEINTERNAL_H

#include "skroadhost.h"
#include "skroadland.h"
#include "skroadmodel.h"

#include "llsd.h"

#include <array>
#include <deque>
#include <string>
#include <vector>

class LLUUID;
class LLVector3d;
class LLViewerRegion;

// Not an interface: what the driver's files (skroaddriver, skroadhost, skroadland, skroadoverlay,
// skroadrecord) share of one another.
namespace skroad::drive
{
    // skroadhost.cpp: the road model's host (skroadhost.h)
    using skroad::host::AIR_DEMAND;
    using skroad::host::askProperties;
    using skroad::host::learnTextures;
    using skroad::host::lindenOwner;
    using skroad::host::lindenOwnerState;
    using skroad::host::refreshAir;
    using skroad::host::sortAsks;
    using skroad::host::tickLdpw;

    // skroadland.cpp: the land the driver reads (skroadland.h)
    using namespace skroad::land;

    // skroadoverlay.cpp
    constexpr F64 OVERLAY_EVERY_S = 0.25;           // the overlay's lines built this often, drawn every frame
    constexpr F64 OVERLAY_FACES_M = 30.0;           // the road's faces outlined this near the car
    bool overlayHidden();                           // a view being shot (sk_road_driver::setOverlayHider)

    // skroadrecord.cpp: while the survey runs
    void writeView(F64 now, const LLVector3d& car, F64 yaw);                                    // once a second
    void watchLine(F64 now, const LLVector3d& pos, F64 yaw, F64 speed, std::string& note);      // stalls and blips

    // skroaddriver.cpp
    // What the driver knows of the car beyond what it measures (sk-road-vehicles.json in the user's settings).
    struct VehicleProfile
    {
        std::string key;                            // "name @ creator"; empty: no profile
        bool chat_gears = false;                    // shifts on Page Up/Down and says its gear in chat
        std::string gear_word = "Gear ";            // the chat before the gear's number
        std::string reverse_word = "Reverse";
    };
    const VehicleProfile& vehicleProfile();         // the car driven now
    U32 shiftKey(S32 dir);                          // the key that shifts up (+) or down (-); 0 for a car without gears
    void fireEvent(const std::string& kind, LLSD data, const LLVector3d& at);  // to the control channel
    extern bool sOn;
    extern bool sPreviewing;                        // the overlay drawn while stopped
    // The car's own track, a point every TRAIL_STEP_M, kept for the session (the overlay draws it).
    struct TrailPoint
    {
        F64 x, y, z;
    };
    constexpr F64 TRAIL_STEP_M = 2.0;
    extern std::deque<TrailPoint> sTrail;
    bool inCrossing();                              // a region crossing under way: nothing is decided in it
    std::vector<LanePoint> testCourse(const std::string& name, F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v);
}

#endif
