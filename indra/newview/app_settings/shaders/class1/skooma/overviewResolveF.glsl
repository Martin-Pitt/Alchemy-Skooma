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
// read. One slate ramp for everything built: class is a small shift in value, height above the agent's floor is
// brightness, shape comes from a key light, a sky ambient and screen-space occlusion rather than outlines. Terrain is
// dark teal, water and the void near-black navy; outside the agent's region everything dims and desaturates over a
// faint grid, and the region's edge is a pale glowing stroke.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform sampler2D classMap;     // R class code (skoverviewcensus.h), B an invisible face on top, G and A the census face normal
uniform mat4 sk_inv_vp;         // window to world: inverse of projection times view
uniform float sk_zo;            // 1 when window depth is NDC depth (zero-to-one), 0 for minus-one-to-one
uniform float sk_far_depth;     // the cleared depth: no surface
uniform vec3 sk_eye;
uniform float sk_top_down;      // 1 for the straight-down map: no distance fade
uniform float sk_agent_z;       // the floor the agent stands on
uniform float sk_level_top;     // the agent's cut height: nothing is shaded brighter than its level's top
uniform float sk_cut;
uniform float sk_water_z;
uniform float sk_water_depth;
uniform vec4 sk_regions[16];   // the territory: the regions styled as inside, min x, min y, max x, max y
uniform int sk_region_count;
uniform vec3 sk_invisible;
uniform int sk_debug_view;      // SKOverviewDebugView

const int CODE_NONE = 0;
const int CODE_FLOOR = 16;
const int CODE_WALL = 17;
const int CODE_COVER = 18;
const int CODE_RAMP = 19;
const int CODE_DOOR = 20;
const int CODE_LADDER = 21;
const int CODE_VEHICLE = 22;
const int CODE_SECTION = 23;
const int CODE_PLACEHOLDER = 24;
const int CODE_MOVER = 25;
const int CODE_VEH_CIVIL = 26;
const int CODE_VEH_ALLY = 27;
const int CODE_VEH_NEUTRAL = 28;
const vec3 VEH_ALLY = vec3(0.31, 0.62, 0.67);      // the mini-map's allied vehicle hull (VEH_ALLY_HULL)
const vec3 VEH_NEUTRAL = vec3(0.55, 0.58, 0.61);   // and neutral (VEH_NEUTRAL_HULL)
const vec3 MOVER_GREEN = vec3(0.31, 0.60, 0.35);   // the mini-map's civilian vehicle hull (VEH_CIVIL_HULL)
const int CODE_TERRAIN = 255;

// The style's knobs.
const vec3 SLATE_TOP = vec3(0.604, 0.651, 0.706);   // #9aa6b4: floors and roofs
const vec3 SLATE_WALL = vec3(0.45, 0.49, 0.54);
const vec3 GROUND = vec3(0.165, 0.192, 0.227);      // #2a313a
const vec3 TEAL = vec3(0.13, 0.22, 0.23);
const vec3 WATER_SHORE = vec3(0.08, 0.12, 0.18);
const vec3 WATER_DEEP = vec3(0.035, 0.05, 0.085);
const vec3 VOID = vec3(0.03, 0.04, 0.065);
const vec3 DOOR_TINT = vec3(0.62, 0.55, 0.74);
const vec3 EDGE = vec3(0.86, 0.90, 0.95);
const vec3 LIGHT = vec3(-0.45, 0.45, 0.77);         // from the north-west and above
const float GROUND_LIFT = 1.45;
const float ROOF_M = 5.0;                          // a floor this far above the agent's is roof-bright
const float HEIGHT_GAIN = 0.045;                    // brightness per metre above the agent's floor
const float AO_STRENGTH = 0.65;
const float FADE_START_M = 90.0;
const float FADE_END_M = 260.0;
const float OUTSIDE_DIM = 0.45;
const float GRID_M = 8.0;

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

vec3 skOctDecode(vec2 e)
{
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// The face normal the census wrote at p, false where none was (terrain, nothing).
bool geomNormal(ivec2 p, out vec3 n)
{
    p = clamp(p, ivec2(0), gSize - 1);
    vec4 cls = texelFetch(classMap, p, 0);
    if (cls.g == 0.0 && cls.a == 0.0)
    {
        n = vec3(0.0, 0.0, 1.0);
        return false;
    }
    n = skOctDecode(vec2(cls.g, cls.a));
    return true;
}

int codeAt(ivec2 p)
{
    p = clamp(p, ivec2(0), gSize - 1);
    return int(texelFetch(classMap, p, 0).r * 255.0 + 0.5);
}

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

bool inRect(vec2 p, vec4 r)
{
    return p.x >= r.x && p.y >= r.y && p.x <= r.z && p.y <= r.w;
}

bool inTerritory(vec2 p)
{
    for (int i = 0; i < sk_region_count; ++i)
    {
        if (inRect(p, sk_regions[i]))
        {
            return true;
        }
    }
    return false;
}

// Inside the territory (every connected region), outside it dimmed and greyed over a faint grid. The territory's
// outer edge is a glowing stroke; edges between its regions are a dashed line.
vec3 regionStyle(vec3 c, vec3 pos, float px_m)
{
    vec2 p = pos.xy;
    int k = -1;
    for (int i = 0; i < sk_region_count; ++i)
    {
        if (inRect(p, sk_regions[i]))
        {
            k = i;
            break;
        }
    }
    float outer = 1.0e9;    // metres to the territory's outer edge
    float inner_m = 1.0e9;  // metres to an edge between two of its regions
    float along = 0.0;      // position along that edge, for the dashes
    if (k >= 0)
    {
        vec4 r = sk_regions[k];
        const float STEP = 0.5;
        float dl = p.x - r.x, dr = r.z - p.x, db = p.y - r.y, dt = r.w - p.y;
        bool ol = !inTerritory(vec2(r.x - STEP, p.y)), or_ = !inTerritory(vec2(r.z + STEP, p.y));
        bool ob = !inTerritory(vec2(p.x, r.y - STEP)), ot = !inTerritory(vec2(p.x, r.w + STEP));
        if (ol) outer = min(outer, dl); else if (dl < inner_m) { inner_m = dl; along = p.y; }
        if (or_) outer = min(outer, dr); else if (dr < inner_m) { inner_m = dr; along = p.y; }
        if (ob) outer = min(outer, db); else if (db < inner_m) { inner_m = db; along = p.x; }
        if (ot) outer = min(outer, dt); else if (dt < inner_m) { inner_m = dt; along = p.x; }
    }
    else
    {
        c = mix(vec3(luma(c)), c, 0.35) * OUTSIDE_DIM;
        vec2 g = abs(fract(p / GRID_M + 0.5) - 0.5) * GRID_M;
        float line = 1.0 - smoothstep(0.0, max(px_m * 1.2, 0.08), min(g.x, g.y));
        c += vec3(0.05, 0.07, 0.10) * line;
        for (int i = 0; i < sk_region_count; ++i)
        {
            vec4 r = sk_regions[i];
            outer = min(outer, length(max(max(r.xy - p, p - r.zw), vec2(0.0))));
        }
    }
    float d = outer / max(px_m, 1e-3);   // pixels to the outer edge
    float stroke = 1.0 - smoothstep(1.5, 2.5, d);
    float glow = exp(-d / 6.0) * 0.35;
    float dark = (k >= 0 && d > 2.5 && d < 3.5) ? 1.0 : 0.0;
    c = mix(c, EDGE, max(stroke, glow));
    c = mix(c, VOID, dark * 0.6);
    // Between two regions of the territory: a thin dashed line, 4 m on, 4 m off.
    float di = inner_m / max(px_m, 1e-3);
    if (di < 1.2 && mod(along, 8.0) < 4.0)
    {
        c = mix(c, EDGE, 0.55);
    }
    return c;
}

float pixelMetres(vec3 pos)
{
    // About how many metres one pixel spans at this distance.
    vec3 a, b;
    vec4 w0 = sk_inv_vp * vec4(0.0, 0.0, sk_zo > 0.5 ? 1.0 : -1.0, 1.0);
    vec4 w1 = sk_inv_vp * vec4(2.0 / float(gSize.x), 0.0, sk_zo > 0.5 ? 1.0 : -1.0, 1.0);
    a = w0.xyz / w0.w;
    b = w1.xyz / w1.w;
    float near_span = length(b - a);
    float near_dist = length(a - sk_eye);
    return sk_top_down > 0.5 ? near_span : near_span * length(pos - sk_eye) / max(near_dist, 1e-3);
}

vec3 distanceFade(vec3 c, vec3 pos)
{
    if (sk_top_down > 0.5)
    {
        return c;
    }
    float f = smoothstep(FADE_START_M, FADE_END_M, length(pos.xy - sk_eye.xy));
    return mix(c, VOID, f * 0.75);
}

void main()
{
    gSize = textureSize(depthMap, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 pos;
    if (!surfaceAt(p, pos))
    {
        // Nothing drawn: the view ray meets the water plane, or the void.
        vec2 ndc_xy = (vec2(p) + 0.5) / vec2(gSize) * 2.0 - 1.0;
        float near_ndc = sk_zo > 0.5 ? 1.0 : -1.0;
        vec4 a = sk_inv_vp * vec4(ndc_xy, near_ndc, 1.0);
        vec4 b = sk_inv_vp * vec4(ndc_xy, near_ndc * 0.5, 1.0);
        vec3 ra = a.xyz / a.w;
        vec3 rb = b.xyz / b.w;
        float dz = rb.z - ra.z;
        float t = abs(dz) > 1e-6 ? (sk_water_z - ra.z) / dz : -1.0;
        if (t > 0.0 && sk_water_z < sk_cut)
        {
            vec3 w = ra + (rb - ra) * t;
            vec3 c = regionStyle(WATER_DEEP, w, pixelMetres(w));
            frag_color = vec4(distanceFade(c, w), 0.85);
            return;
        }
        frag_color = vec4(VOID, sk_top_down > 0.5 ? 0.0 : 1.0);
        return;
    }
    vec4 cls = texelFetch(classMap, p, 0);
    int code = int(cls.r * 255.0 + 0.5);
    bool invisible = cls.b > 0.5;
    // Uncut geometry above the agent's level (another region's roofs) shades as that level's top, not ever lighter.
    float rel = min(pos.z, sk_level_top) - sk_agent_z;
    float px_m = pixelMetres(pos);

    // The census's own face normals (classMap G, A); none (0, 0) for terrain, whose normal comes from depth.
    vec3 gn = vec3(0.0);
    bool has_gn = geomNormal(p, gn);

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
    if (has_gn)
    {
        n = gn;
    }
    if (dot(n, sk_eye - pos) < 0.0)
    {
        n = -n;
    }

    // Value by class: one slate ramp, a small shift each.
    vec3 c;
    float a = 0.92;
    bool built = true;
    if (code == CODE_TERRAIN)
    {
        c = mix(GROUND, TEAL, 0.6);
        built = false;
    }
    else if (code == CODE_FLOOR || code == CODE_RAMP)
    {
        // Ground at the agent's level is the dark play area; floors and roofs above it rise to the light slate.
        c = mix(GROUND * GROUND_LIFT, SLATE_TOP, smoothstep(0.5, ROOF_M, rel));
        c = code == CODE_RAMP ? mix(c, SLATE_WALL, 0.3) : c;
    }
    else if (code == CODE_WALL || code == CODE_SECTION)
    {
        c = SLATE_WALL;
    }
    else if (code == CODE_COVER)
    {
        c = SLATE_WALL * 1.07;
    }
    else if (code == CODE_DOOR)
    {
        c = mix(SLATE_WALL, DOOR_TINT, 0.45);
    }
    else if (code == CODE_LADDER)
    {
        c = SLATE_WALL * 1.12;
    }
    else if (code == CODE_MOVER || code == CODE_VEH_CIVIL)
    {
        c = MOVER_GREEN;
    }
    else if (code == CODE_VEH_ALLY)
    {
        c = VEH_ALLY;
    }
    else if (code == CODE_VEH_NEUTRAL)
    {
        c = VEH_NEUTRAL;
    }
    else if (code == CODE_PLACEHOLDER)
    {
        c = SLATE_WALL * 0.9;
    }
    else if (code == CODE_VEHICLE)
    {
        c = mix(SLATE_TOP, SLATE_WALL, 0.3);
    }
    else
    {
        // Nothing the overview draws writes another code; mark it plainly if one turns up.
        c = SLATE_WALL;
    }

    // Height reads as brightness: above the agent's floor lighter, below darker.
    c *= clamp(1.0 + rel * HEIGHT_GAIN, 0.5, 1.3);

    // How far off a plane two samples of one surface can sit from depth precision and coplanar overlaps alone.
    float coplanar_m = 0.02 + 0.002 * length(sk_eye - pos);

    // Key light, sky ambient and occlusion.
    float key = max(dot(n, normalize(LIGHT)), 0.0);
    float sky = n.z * 0.5 + 0.5;
    float occ = 0.0;
    float r_px = clamp(1.6 / max(px_m, 1e-3), 3.0, 28.0);    // about 1.6 m around the texel
    for (int i = 0; i < 10; ++i)
    {
        float ang = float(i) * 2.39996 + 0.3;
        float rad = r_px * (0.35 + 0.65 * fract(float(i) * 0.618));
        vec3 q;
        if (!surfaceAt(p + ivec2(round(vec2(cos(ang), sin(ang)) * rad)), q))
        {
            continue;
        }
        vec3 v = q - pos;
        float len = length(v);
        // Only geometry standing off this surface occludes: coplanar neighbours within depth precision never do.
        float rise = dot(v, n) - coplanar_m;
        occ += clamp(rise / (len + 0.05), 0.0, 1.0) * (1.0 - smoothstep(1.0, 3.0, len));
    }
    float ao = 1.0 - AO_STRENGTH * occ / 10.0;
    float debug_crease = 0.0;
    float debug_cut = 0.0;
    c *= (0.42 + 0.5 * key + 0.28 * sky) * ao;

    // A faint crease where faces meet, and the top edge of a wall the cut went through.
    if (built && code >= CODE_FLOOR)
    {
        vec3 nb;
        float crease = 0.0;
        // A crease is where two faces meet: both neighbours on the surface (no step), the faces turned apart. Flush or
        // overlapping coplanar parts differ only in depth noise, which never makes one.
        vec3 gx, gy;
        if (has_gn && geomNormal(p + ivec2(1, 0), gx) && geomNormal(p + ivec2(0, 1), gy))
        {
            // Where faces meet, from the faces' own normals: flush or overlapping parts on one plane share theirs.
            crease = 1.0 - min(abs(dot(gx, gn)), abs(dot(gy, gn)));
        }
        else if (hx1 && hy1 && abs(dot(px1 - pos, n)) < coplanar_m * 4.0 && abs(dot(py1 - pos, n)) < coplanar_m * 4.0)
        {
            vec3 m = normalize(cross(px1 - pos, py1 - pos) + vec3(1e-6));
            crease = 1.0 - abs(dot(m, n));
        }
        c *= 1.0 - 0.25 * smoothstep(0.25, 0.6, crease);
        debug_crease = smoothstep(0.25, 0.6, crease);
        if ((code == CODE_WALL || code == CODE_COVER || code == CODE_SECTION) && sk_cut - pos.z < max(0.12, px_m * 1.5))
        {
            c = SLATE_TOP * 1.1;
            debug_cut = 1.0;
        }
    }

    if (pos.z < sk_water_z)
    {
        // Shore water over geometry, deep at depth, ending on the open water's colour; walls fade over a short band.
        float fade = (code == CODE_FLOOR || code == CODE_TERRAIN || code == CODE_RAMP) ? sk_water_depth : 1.0;
        float t = clamp((sk_water_z - pos.z) / fade, 0.0, 1.0);
        c = mix(c, mix(WATER_SHORE, WATER_DEEP, t), mix(0.6, 1.0, t));
        a = mix(a, 0.85, t);
    }
    if (invisible && mod(gl_FragCoord.x + gl_FragCoord.y, 6.0) < 2.0)
    {
        c = mix(c, sk_invisible, 0.6);
        a = max(a, 0.85);
    }
    c = regionStyle(c, pos, px_m);
    c = distanceFade(c, pos);
    frag_color = vec4(clamp(c, 0.0, 1.0), a);

    // Debug views (SKOverviewDebugView), opaque, replacing the styled colour.
    if (sk_debug_view == 1)
    {
        vec3 k = code == CODE_TERRAIN     ? vec3(0.1, 0.5, 0.1)
               : code == CODE_FLOOR       ? vec3(0.6, 0.6, 0.6)
               : code == CODE_WALL        ? vec3(0.9, 0.2, 0.2)
               : code == CODE_COVER       ? vec3(1.0, 0.6, 0.1)
               : code == CODE_RAMP        ? vec3(0.9, 0.9, 0.2)
               : code == CODE_DOOR        ? vec3(0.6, 0.3, 1.0)
               : code == CODE_LADDER      ? vec3(0.2, 1.0, 1.0)
               : code == CODE_VEHICLE     ? vec3(0.2, 0.4, 1.0)
               : code == CODE_SECTION     ? vec3(0.5, 0.0, 0.0)
               : code == CODE_PLACEHOLDER ? vec3(1.0, 0.0, 1.0)
                                          : vec3(1.0, 1.0, 1.0);
        frag_color = vec4(invisible && mod(gl_FragCoord.x + gl_FragCoord.y, 4.0) < 2.0 ? vec3(0.0) : k, 1.0);
    }
    else if (sk_debug_view == 2)
    {
        float band = fract(length(pos - sk_eye) / 2.0);
        frag_color = vec4(vec3(band), 1.0);
    }
    else if (sk_debug_view == 3)
    {
        frag_color = vec4(n * 0.5 + 0.5, 1.0);
    }
    else if (sk_debug_view == 4)
    {
        frag_color = vec4(clamp(AO_STRENGTH * occ / 10.0 * 2.0, 0.0, 1.0), debug_crease, debug_cut, 1.0);
    }
}
