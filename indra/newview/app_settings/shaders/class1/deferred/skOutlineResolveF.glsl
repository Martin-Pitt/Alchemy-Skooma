/**
 * @file skOutlineResolveF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// Outline capture: a depth-only coverage render (2x2 subsamples a cell) to the share of each cell covered.
// A signed distance field of the layer would be built here, after this average.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform ivec4 sk_slot;        // the slot's lower-left texel in the atlas, then the layer's cols and rows
uniform float sk_clear_depth; // what the capture target was cleared to: no surface
uniform int sk_count;         // 1 to count covered cells: uncovered ones are discarded and nothing is written

in vec2 vary_fragcoord;

float covered(ivec2 p)
{
    return texelFetch(depthMap, p, 0).r != sk_clear_depth ? 1.0 : 0.0;
}

void main()
{
    ivec2 cell = ivec2(gl_FragCoord.xy) - sk_slot.xy;
    float c = 0.0;
    if (cell.x < sk_slot.z && cell.y < sk_slot.w)
    {
        ivec2 s = cell * 2;
        c = 0.25 * (covered(s) + covered(s + ivec2(1, 0)) + covered(s + ivec2(0, 1)) + covered(s + ivec2(1, 1)));
    }
    if (sk_count != 0 && c == 0.0)
    {
        discard;
    }
    frag_color = vec4(c);
}
