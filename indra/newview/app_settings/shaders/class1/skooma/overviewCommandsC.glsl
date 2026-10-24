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
// A patch capture keeps only slots whose screen rectangle overlaps the patch, as SKOverviewMovers::screenRectOf finds it.
uniform uint sk_patch_on;
uniform mat4 sk_view_proj;      // agent space to clip
uniform vec3 sk_offset_agent;   // the page's origin in agent space
uniform ivec4 sk_size;          // the capture's width and height in xy
uniform ivec4 sk_patch;         // left, bottom, right, top

// Whether the box's screen rectangle, a pixel's margin round it, overlaps the patch (both edges inclusive, as LLRect's).
bool onPatch(vec3 lo, vec3 hi)
{
    ivec2 size = sk_size.xy;
    uint behind = 0u;
    vec2 a = vec2(3.4e38);
    vec2 b = vec2(-3.4e38);
    for (int corner = 0; corner < 8; ++corner)
    {
        vec3 at = vec3((corner & 1) != 0 ? hi.x : lo.x, (corner & 2) != 0 ? hi.y : lo.y, (corner & 4) != 0 ? hi.z : lo.z);
        vec4 clip = sk_view_proj * vec4(at, 1.0);
        if (clip.w <= 1.0e-4)
        {
            ++behind;
            continue;
        }
        vec2 px = (clip.xy / clip.w * 0.5 + 0.5) * vec2(size);
        a = min(a, px);
        b = max(b, px);
    }
    if (behind == 8u)
    {
        return false;
    }
    // Straddling the eye's plane, a box projects without bound: the whole view.
    ivec4 r = ivec4(0, 0, size);
    if (behind == 0u)
    {
        r = clamp(ivec4(ivec2(floor(a)) - 1, ivec2(ceil(b)) + 1), ivec4(0), ivec4(size, size));
        if (r.x == r.z || r.y == r.w)
        {
            return false;
        }
    }
    return !(r.x > sk_patch.z || r.z < sk_patch.x || r.y > sk_patch.w || r.w < sk_patch.y);
}

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
    if (keep && sk_patch_on != 0u)
    {
        keep = onPatch(sk_offset_agent + slot.lo.xyz, sk_offset_agent + slot.hi.xyz);
    }
    sk_commands[i] = uvec4(keep ? slot.run.y : 0u, 1u, slot.run.x, 0u);
}
