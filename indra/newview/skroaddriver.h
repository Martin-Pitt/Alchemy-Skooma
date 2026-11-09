/**
 * @file skroaddriver.h
 * @brief Spike: drives the vehicle the agent sits in along the Linden road's right lane.
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

#ifndef SK_ROADDRIVER_H
#define SK_ROADDRIVER_H

#include "llsd.h"
#include "lluuid.h"

#include <functional>
#include <string>
#include <vector>

// While SKRoadAutopilot is on and the agent sits in a vehicle on a road, the driver presses the keys a
// person would (W held and gears, A/D as taps of the turn flags) to keep Nexii's line in the right lane:
// the predictive controller of the offline benchmark, ported (see skroaddriver.cpp's opening comment).
// It reads the road as its pieces' exact shapes, walks the lane's line ahead from cross-section to
// cross-section, replays its taps in flight through the car's measured response, steers through region
// borders' kicks, chooses each tap by trying 77 tap plans 2.5 s ahead, and learns the car's gain and
// delay as it goes. With no road in sight it coasts and waits; after 8 s it stops. Any movement key from
// the user (not up/down: gears) hands control back at once and turns the setting off. Each frame's view
// and decision go to sk-road-autopilot.csv in the logs folder.
namespace sk_road_driver
{
    void onSettingChanged(const LLSD& enabled);

    /// Called by LLAppViewer::idle() just before the agent update is sent: adds this frame's keys.
    void beforeAgentUpdate();

    /// Called for each line of local chat: the vehicle being driven announcing its gear ("Gear 2",
    /// "Reverse") is how the driver knows it.
    void noteChat(const LLUUID& from, const std::string& text);

    /// Draws what the driver saw and decided last frame (SKRoadAutopilotOverlay): every road part in reach
    /// (faint; the walked ones brighter), the session's trail behind the car (grey),
    /// cross-sections edge to edge (blue), the lane path it keeps (amber), the car
    /// predicted with no new tap (violet), a tap (red bar on the side it turns to), waiting (red ring).
    void renderDebug();

    /// Something that shoots the view (the kit snapshots) hides the overlay while it says so.
    void setOverlayHider(std::function<bool()> hidden);

    // For the drive mission (skroadmission.cpp), which places the car and starts and watches the driver.

    /// Whether the driver is driving now.
    bool isRunning();
    /// Whether it is holding through a region crossing, deciding nothing until the new region sends the car again.
    bool isCrossing();

    /// The unsafe crossing that ended this run ({region, from_region, at [x, y, z], edge_at, ownership, why}), or
    /// undefined: braked to a stop short of a region edge whose land across is not a Linden road parcel, with no
    /// region beyond, or with the car off the road at it.
    LLSD unsafeCrossing();

    /// Why it last stopped ("stuck three times in 30 s: stopping", "you took over"), and the last thing it said.
    const std::string& lastStopReason();
    const std::string& lastSaid();

    /// The road at a point, from the driver's own road model, for a car about to be rezzed there.
    struct RoadPlace
    {
        F64 centre_x, centre_y;     // the road's centre line across the point
        F64 lane_x, lane_y;         // the right lane's line for travel along (dir_x, dir_y): the line the driver keeps
        F64 z;                      // the road's surface
        F64 dir_x, dir_y;           // the road's way there, the side of the heading asked for
        F64 road_w;
        S32 lanes;
    };

    /// While the driver is off: reads the road pieces about the point (asking the census for them) and finds the
    /// road's cross-section there, for travel toward (hx, hy) and a car of the given half width. False until the
    /// pieces have loaded, or where there is no road. placingDone() lets the census go.
    bool placeOnRoad(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 half_width, RoadPlace& out);

    /// While the driver is off: how far (metres) the road goes on from the point toward (hx, hy), walked along its
    /// pieces as the driver would; -1 while it is running.
    F64 roadAhead(F64 x, F64 y, F64 z, F64 hx, F64 hy);
    void placingDone();

    // For the control channel (skroadbotapi.cpp).

    struct LinePoint
    {
        F64 x, y, z;                // global
        S32 access;                 // whose road is under it: 1 public, 0 private, -1 unknown
    };
    /// The lane line the driver keeps now, nearest first; empty while it has none.
    std::vector<LinePoint> currentLaneLine();

    /// Why the road model's walk ended where it did ("end: ..." at the road's end); empty while none.
    const std::string& walkEnd();

    struct RoadEnd
    {
        bool end = false;           // the line ends at the road's end (or a jump too far, or one placed across)
        F64 x = 0, y = 0, dx = 1, dy = 0;
        bool jump = false;          // a jump ahead the car can make (or whose top speed is not measured yet)
        F64 lip_x = 0, lip_y = 0, lip_z = 0, land_x = 0, land_y = 0, land_z = 0;
        F64 need_speed = 0;         // m/s the last jump needed
        F64 top_speed = 0;          // m/s the car's fastest gear measured, 0 not yet
    };
    RoadEnd roadEnd();

    /// A jump the driver will not drive (SKRoadAutopilotJumps off, the default): with at least 50 m of road going on
    /// past its landing, the driver stops short of the lip, holds the brake and asks to be placed across: on the
    /// lane at (x, y, z), about 8 m past the landing, facing (dx, dy). The mission moves the car there (above the
    /// lane, a small drop) and calls placedAcross(); the driver then drives on from wherever the car is. The ask
    /// stands (while the driver is off too) until placedAcross(), or the car is 60 m from the lip. A jump with less
    /// road past it is a road end (turned round at).
    struct PlaceAcross
    {
        F64 x = 0, y = 0, z = 0;    // global
        F64 dx = 1, dy = 0;         // the road's way there
        F64 road_on = 0;            // metres of road the walk saw past the landing
    };
    bool wantsPlaceAcross(PlaceAcross& out);
    void placedAcross();

    /// The gear the car announced last (0 neutral or not said, -1 reverse).
    S32 gear();

    /// Called on the driver's events: "line_stalled", "line_blip", "dead_end", "jump", "jump_refused",
    /// "refused_private", "place_across" ("to" [x, y, z], "heading" [dx, dy], "road_on"); data holds "at" (the car,
    /// global) and the event's own fields. One hook; empty clears it.
    void setEventHook(std::function<void(const std::string& kind, const LLSD& data)> hook);
}

#endif // SK_ROADDRIVER_H
