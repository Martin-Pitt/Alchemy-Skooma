/**
 * @file overviewCommandsC.glsl
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

// One draw command per slot of a census page: its run where the shot's level keeps its linkset and the frustum holds
// its box, else a count of 0, as skoverview::buildCommands (skoverviewslots.cpp) builds them on the CPU.

layout(local_size_x = 64) in;

// skoverview::SlotCull: first, count, linkset id, on the ground; the page-local box, w its cluster's lowest and highest z.
struct SKSlot
{
    uvec4 run;
    vec4 lo;
    vec4 hi;
};

// Past the reserved storage blocks' points, which this program declares none of.
layout(std430, binding = 6) readonly buffer SKOverviewSlotBlock { SKSlot sk_slots[]; };
// ALDrawArraysIndirectCommand: count, instances, first, base instance.
layout(std430, binding = 7) writeonly buffer SKOverviewCommandBlock { uvec4 sk_commands[]; };

uniform uint sk_slot_count;
uniform vec4 sk_level;          // x all, y the ground's, z and w the heights a kept linkset overlaps
uniform vec4 sk_planes[6];      // page-local; a*x + b*y + c*z + d <= 0 inside

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= sk_slot_count)
    {
        return;
    }
    SKSlot slot = sk_slots[i];
    bool keep = sk_level.x > 0.5 || (sk_level.y > 0.5 && slot.run.w != 0u) || (slot.hi.w >= sk_level.z && slot.lo.w <= sk_level.w);
    vec3 centre = 0.5 * (slot.lo.xyz + slot.hi.xyz);
    vec3 half_size = 0.5 * (slot.hi.xyz - slot.lo.xyz);
    for (int p = 0; p < 6 && keep; ++p)
    {
        // The plane's value at the box's corner furthest inside it.
        keep = dot(sk_planes[p].xyz, centre) - dot(abs(sk_planes[p].xyz), half_size) + sk_planes[p].w <= 0.0;
    }
    sk_commands[i] = uvec4(keep ? slot.run.y : 0u, 1u, slot.run.x, 0u);
}
