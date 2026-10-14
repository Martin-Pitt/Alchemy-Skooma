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

// Overview: depth and a class code per texel to a map, from straight above or from a tilted camera. Albedo is never
// read. The census layer is styled by its class (floor, wall, cover, ramp, door, ladder, vehicle, section); the ghost
// layer (rendered geometry) is a faint tint with outlines; terrain is green. Floors are shaded by height relative to
// the agent, outlines come from depth and normal breaks and class changes, invisible faces are hatched.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform sampler2D classMap;     // R class code (skoverviewcensus.h), B an invisible face on top
uniform mat4 sk_inv_vp;         // window to world: inverse of projection times view
uniform float sk_zo;            // 1 when window depth is NDC depth (zero-to-one), 0 for minus-one-to-one
uniform float sk_far_depth;     // the cleared depth: no surface
uniform vec3 sk_eye;
uniform float sk_top_down;      // 1 for the straight-down map: tops of ghost geometry by height read as cover
uniform float sk_agent_z;       // the floor the agent stands on
uniform float sk_cut;
uniform float sk_water_z;
uniform float sk_water_depth;
uniform vec3 sk_floor;
uniform vec3 sk_terrain;
uniform vec3 sk_water_shallow;
uniform vec3 sk_water_deep;
uniform vec3 sk_wall_full;
uniform vec3 sk_wall_half;
uniform vec3 sk_invisible;
uniform vec3 sk_door;
uniform vec3 sk_ladder;
uniform vec3 sk_vehicle;
uniform vec3 sk_ghost;

const int CODE_NONE = 0;
const int CODE_GHOST = 2;
const int CODE_GHOST_SECTION = 3;
const int CODE_FLOOR = 16;
const int CODE_WALL = 17;
const int CODE_COVER = 18;
const int CODE_RAMP = 19;
const int CODE_DOOR = 20;
const int CODE_LADDER = 21;
const int CODE_VEHICLE = 22;
const int CODE_SECTION = 23;
const int CODE_TERRAIN = 255;

const float HALF_COVER_M = 0.5;
const float FULL_COVER_M = 1.5;
const float CONTOUR_M = 2.0;
const vec3 INK = vec3(0.06, 0.07, 0.08);
const vec3 LIGHT = vec3(-0.35, -0.55, 0.76);

ivec2 gSize;

bool surfaceAt(ivec2 p, out vec3 pos)
{
    p = clamp(p, ivec2(0), gSize - 1);
    float d = texelFetch(depthMap, p, 0).r;
    if (d == sk_far_depth)
    {
        pos = vec3(0.0);
        return false;
    }
    vec2 uv = (vec2(p) + 0.5) / vec2(gSize);
    vec4 ndc = vec4(uv * 2.0 - 1.0, mix(d * 2.0 - 1.0, d, sk_zo), 1.0);
    vec4 w = sk_inv_vp * ndc;
    pos = w.xyz / w.w;
    return true;
}

int codeAt(ivec2 p)
{
    p = clamp(p, ivec2(0), gSize - 1);
    return int(texelFetch(classMap, p, 0).r * 255.0 + 0.5);
}

bool isCensus(int code)
{
    return code >= CODE_FLOOR && code <= CODE_SECTION;
}

void main()
{
    gSize = textureSize(depthMap, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 pos;
    if (!surfaceAt(p, pos))
    {
        frag_color = vec4(0.0);
        return;
    }
    vec4 cls = texelFetch(classMap, p, 0);
    int code = int(cls.r * 255.0 + 0.5);
    bool invisible = cls.b > 0.5;
    float rel = pos.z - sk_agent_z;

    // The normal from the nearer neighbour on each axis, so a silhouette does not bend it.
    vec3 px0, px1, py0, py1;
    bool hx0 = surfaceAt(p - ivec2(1, 0), px0), hx1 = surfaceAt(p + ivec2(1, 0), px1);
    bool hy0 = surfaceAt(p - ivec2(0, 1), py0), hy1 = surfaceAt(p + ivec2(0, 1), py1);
    vec3 dx0 = hx0 ? pos - px0 : vec3(1e6), dx1 = hx1 ? px1 - pos : vec3(1e6);
    vec3 dy0 = hy0 ? pos - py0 : vec3(1e6), dy1 = hy1 ? py1 - pos : vec3(1e6);
    vec3 dx = dot(dx0, dx0) < dot(dx1, dx1) ? dx0 : dx1;
    vec3 dy = dot(dy0, dy0) < dot(dy1, dy1) ? dy0 : dy1;
    vec3 n = cross(dx, dy);
    n = dot(n, n) > 1e-12 && dot(dx, dx) < 1e5 && dot(dy, dy) < 1e5 ? normalize(n) : vec3(0.0, 0.0, 1.0);
    if (dot(n, sk_eye - pos) < 0.0)
    {
        n = -n;
    }

    vec3 c;
    float a;
    bool flat_kind = false;
    if (code == CODE_TERRAIN)
    {
        c = sk_terrain;
        a = 0.72;
        flat_kind = true;
    }
    else if (code == CODE_FLOOR || code == CODE_RAMP)
    {
        c = code == CODE_RAMP ? mix(sk_floor, sk_wall_half, 0.35) : sk_floor;
        a = 0.85;
        flat_kind = true;
    }
    else if (code == CODE_WALL || code == CODE_SECTION)
    {
        c = code == CODE_SECTION ? sk_wall_full * 0.85 : sk_wall_full;
        a = 0.92;
    }
    else if (code == CODE_COVER)
    {
        c = sk_wall_half;
        a = 0.9;
    }
    else if (code == CODE_DOOR)
    {
        c = sk_door;
        a = 0.95;
    }
    else if (code == CODE_LADDER)
    {
        c = sk_ladder;
        a = 0.95;
    }
    else if (code == CODE_VEHICLE)
    {
        c = sk_vehicle;
        a = 0.92;
    }
    else
    {
        // Ghost: rendered geometry the census does not hold. Straight down, its tops by height read as cover as they
        // always have; tilted, it is a faint tint and its outlines.
        c = sk_ghost;
        a = code == CODE_GHOST_SECTION ? 0.4 : 0.22;
        if (sk_top_down > 0.5)
        {
            if (code == CODE_GHOST_SECTION || rel > FULL_COVER_M)
            {
                c = sk_wall_full;
                a = 0.6;
            }
            else if (rel > HALF_COVER_M)
            {
                c = sk_wall_half;
                a = 0.55;
            }
            else
            {
                c = sk_floor;
                a = 0.45;
                flat_kind = true;
            }
        }
    }

    // Floors by height: storeys below fade darker, a little per metre.
    if (flat_kind)
    {
        c *= clamp(1.0 + rel * 0.04, 0.45, 1.15);
    }
    // Faces turn to a light from the north-west and above, so walls read in the tilted view.
    c *= mix(0.72, 1.08, clamp(dot(n, normalize(LIGHT)) * 0.5 + 0.5, 0.0, 1.0));

    if (pos.z < sk_water_z && (flat_kind || code == CODE_NONE))
    {
        float t = clamp((sk_water_z - pos.z) / sk_water_depth, 0.0, 1.0);
        vec3 w = mix(sk_water_shallow, sk_water_deep, t);
        c = mix(c, w, mix(0.55, 0.85, t));
    }
    if (code == CODE_TERRAIN && floor(pos.z / CONTOUR_M) != floor((hx1 ? px1.z : pos.z) / CONTOUR_M))
    {
        c = mix(c, vec3(0.85, 0.88, 0.80), 0.3);
    }

    // Outlines: a neighbour off this texel's plane by more than its footprint allows, a crease, or a class change.
    float dist = length(sk_eye - pos);
    float tol = 0.12 + 0.012 * dist;
    float edge = 0.0;
    bool census_here = isCensus(code);
    for (int i = 0; i < 4; ++i)
    {
        ivec2 o = i == 0 ? ivec2(1, 0) : i == 1 ? ivec2(-1, 0) : i == 2 ? ivec2(0, 1) : ivec2(0, -1);
        vec3 q;
        if (!surfaceAt(p + o, q))
        {
            edge = 1.0;
            continue;
        }
        if (abs(dot(q - pos, n)) > tol)
        {
            edge = max(edge, 1.0);
        }
        int qc = codeAt(p + o);
        if (qc != code && (census_here || isCensus(qc)))
        {
            edge = max(edge, 0.7);
        }
    }
    if (edge > 0.0)
    {
        if (census_here || code == CODE_TERRAIN)
        {
            c = mix(c, INK, 0.6 * edge);
            a = max(a, 0.9);
        }
        else
        {
            // The ghost is its outlines: light lines over the dark.
            c = mix(c, sk_ghost * 1.2, 0.8 * edge);
            a = max(a, 0.65 * edge);
        }
    }
    if (invisible && mod(gl_FragCoord.x + gl_FragCoord.y, 6.0) < 2.0)
    {
        c = sk_invisible;
        a = 0.9;
    }
    frag_color = vec4(clamp(c, 0.0, 1.0), a);
}
