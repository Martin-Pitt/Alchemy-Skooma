/**
 * @file reflectionprobeF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

out vec4 frag_color;

in vec2 vary_fragcoord;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);

vec4 sampleReflectionProbesDebug(vec3 pos);
vec4 skProbePulseDebug(vec3 pos, vec3 norm); // <SK:Nexii> Probe volume display
vec4 getNorm(vec2 screenpos); // <SK:Nexii> Probe volume display
bool isFarDepth(float d); // <SK:Nexii> Probe volume display

void main()
{
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    vec4  pos          = getPositionWithDepth(tc, depth);

    // <SK:Nexii> Probe volume display: every probe as a frozen scanner pulse on surfaces, nothing in mid-air.
    if (isFarDepth(depth))
    {
        discard;
    }
    frag_color = max(skProbePulseDebug(pos.xyz, normalize(getNorm(tc).xyz)), vec4(0));
    // </SK:Nexii>
}
