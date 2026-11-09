/**
 * @file skroadhost.h
 * @brief The road model's host in the viewer: what else the viewer's road code asks of it.
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

#ifndef SK_ROADHOST_H
#define SK_ROADHOST_H

#include "skroadmodel.h"

#include <functional>
#include <string>
#include <vector>

class LLUUID;
class LLVector3d;

// skroadmodel.h declares what the road model reads of the viewer (skroad::host); these are the rest of the
// host, for the viewer's own road code.
namespace skroad::host
{
    constexpr char AIR_DEMAND[] = "road autopilot";
    S32 lindenOwnerState(const LLUUID& owner);     // 1 a Linden or a mole, 0 known not, -1 still being learnt
    void askProperties();                           // a few linksets' properties a second, the road ahead first
    void refreshAir(const LLVector3d& car, F64 reach);  // the published air tiles about the car
    void tickLdpw();                                // names the members learnt nameless, saves the list now and then

    // Who lays a test course (SKRoadAutopilotTestCourse): unset, there is none.
    using TestCourse = std::function<std::vector<LanePoint>(const std::string& name, F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v)>;
    void setTestCourse(TestCourse course);
}

#endif
