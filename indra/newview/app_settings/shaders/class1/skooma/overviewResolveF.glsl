/**
 * @file overviewResolveF.glsl
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

// Overview capture: top-down depth and a class per texel to a map coloured by height relative to the agent.
// Albedo is never read. Floors near the agent's level are bright, storeys below darken, tops between the agent's
// knee and the cut are cover (half, then full), edges are drawn where the height jumps, floors darken beside taller
// neighbours, solids the cut goes through are full cover, and invisible faces are hatched over whatever is under them.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform sampler2D classMap;     // R terrain, G object, B invisible on top, A a solid cut through (back face on top)
uniform vec4 sk_depth;          // window depth at the near (cut) and far planes, then their heights
uniform float sk_mpp;           // metres per texel
uniform float sk_agent_z;
uniform float sk_water_z;
uniform float sk_water_depth;
uniform vec3 sk_floor;
uniform vec3 sk_terrain;
uniform vec3 sk_water_shallow;
uniform vec3 sk_water_deep;
uniform vec3 sk_wall_full;
uniform vec3 sk_wall_half;
uniform vec3 sk_invisible;

const float NO_SURFACE = -1.0e6;
const float HALF_COVER_M = 0.5;   // the gold's low cut above the floor
const float FULL_COVER_M = 1.5;   // and its high cut
const float EDGE_M = 0.35;        // a height jump this large is an edge
const float CONTOUR_M = 2.0;

ivec2 gSize;

float heightAt(ivec2 p)
{
    p = clamp(p, ivec2(0), gSize - 1);
    float d = texelFetch(depthMap, p, 0).r;
    if (d == sk_depth.y)
    {
        return NO_SURFACE;
    }
    // A solid the cut goes through stands to the cut.
    vec4 cls = texelFetch(classMap, p, 0);
    if (cls.g > 0.5 && cls.a > 0.5)
    {
        return sk_depth.z;
    }
    return mix(sk_depth.z, sk_depth.w, (d - sk_depth.x) / (sk_depth.y - sk_depth.x));
}

void main()
{
    gSize = textureSize(depthMap, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = heightAt(p);
    if (z == NO_SURFACE)
    {
        frag_color = vec4(0.0);
        return;
    }
    vec4 cls = texelFetch(classMap, p, 0);
    bool object = cls.g > 0.5;
    bool terrain = !object && cls.r > 0.5;
    bool invisible = cls.b > 0.5;
    float rel = z - sk_agent_z;

    // Base: terrain green, object floors grey, water over anything below its surface.
    vec3 c = terrain ? sk_terrain : sk_floor;
    float a = 0.72;
    if (rel > FULL_COVER_M)
    {
        c = sk_wall_full;
        a = 0.9;
    }
    else if (rel > HALF_COVER_M)
    {
        c = sk_wall_half;
        a = 0.85;
    }
    else
    {
        // Storeys below fade darker, a little per metre, as the floorplan's ramp gradient does.
        c *= clamp(1.0 + rel * 0.04, 0.45, 1.15);
    }
    if (z < sk_water_z)
    {
        float t = clamp((sk_water_z - z) / sk_water_depth, 0.0, 1.0);
        vec3 w = mix(sk_water_shallow, sk_water_deep, t);
        c = mix(c, w, mix(0.55, 0.85, t));
    }

    // Edges and wall shading from the height field around the texel.
    float r = max(1.0, 0.25 / sk_mpp);
    float step_max = 0.0;
    float above = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float ang = float(i) * 0.785398;
        ivec2 o = ivec2(round(vec2(cos(ang), sin(ang)) * r));
        float n = heightAt(p + o);
        float dz = (n == NO_SURFACE) ? 10.0 : n - z;
        step_max = max(step_max, abs(dz));
        // Taller neighbours a little further out shade the floor beside them.
        float n2 = heightAt(p + o * 3);
        float dz2 = (n2 == NO_SURFACE) ? 0.0 : n2 - z;
        above += clamp(dz2 / 2.0, 0.0, 1.0);
    }
    if (rel <= HALF_COVER_M)
    {
        c *= 1.0 - 0.35 * (above / 8.0);
    }
    if (terrain && floor(z / CONTOUR_M) != floor(heightAt(p + ivec2(1, 0)) / CONTOUR_M))
    {
        c = mix(c, vec3(0.85, 0.88, 0.80), 0.3);
    }
    if (step_max > EDGE_M)
    {
        c = mix(c, vec3(0.08, 0.09, 0.10), 0.6);
        a = max(a, 0.9);
    }
    if (invisible && mod(gl_FragCoord.x + gl_FragCoord.y, 6.0) < 2.0)
    {
        c = sk_invisible;
        a = 0.9;
    }
    frag_color = vec4(c, a);
}
