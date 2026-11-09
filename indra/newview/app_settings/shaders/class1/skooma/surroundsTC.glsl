/**
 * @file surroundsTC.glsl
 *
 * Tessellation levels for a distant region's map-tiled patch: a far node's (equal spacing), or a ring cell's drawn
 * with its map tile (SK_SURROUNDS_RING: the live spacing). An edge between patches of one size takes terrainTC's
 * level from the same numbers. An edge where the patch size halves takes the parent edge's level L, worked from the
 * coarse patch's corners: the fine half m = ceil(L / 2) points (odd where the fine side is fractional odd), the
 * coarse side 2m, so every fine vertex is a coarse one (SKSurrounds::fineHalfLevel).
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

layout(vertices = 4) out;

uniform sampler2D terrain_height_map;
uniform vec3  terrain_tess_origin;
uniform float terrain_tess_density;
uniform float terrain_grid_scale;
uniform mat4  terrain_tess_clip;
uniform vec4  terrain_tess_clip_origin;
uniform vec2  terrain_tess_raster;
uniform float region_scale;          // the node's edge in metres
uniform ivec4 sk_surrounds_edges;    // per side (west, east, south, north): 0 same size, 1 fine, 2 coarse, 3 coarse of an odd fine side
uniform uvec4 sk_surrounds_mask;     // the node's 8x8 patches that have heights, bit = patch index (x: 0-31, y: 32-63)

const int   TERRAIN_APRON      = 2;
const float TERRAIN_MAX_LEVEL  = 64.0;
const float TERRAIN_GRID_LEVEL = 16.0;
const float SK_SURROUNDS_NO_HEIGHT = -1.0e29;

#if defined(SK_SURROUNDS_RING) && !defined(TERRAIN_TESS_PINNED)
const bool SK_FINE_ODD = true;
#else
const bool SK_FINE_ODD = false;
#endif

float heightAt(vec2 p)
{
    ivec2 g = ivec2(round(p / terrain_grid_scale)) + TERRAIN_APRON;
    return texelFetch(terrain_height_map, g, 0).r;
}

vec3 corner(int i)
{
    vec2 p = gl_in[i].gl_Position.xy;
    return vec3(p, heightAt(p));
}

// terrainTC's edgeLevel, operation for operation, so a ring cell's edge against a live region's agrees bit for bit.
float edgeLevel(vec3 a, vec3 b)
{
    if (terrain_tess_density <= 0.0)
    {
        return TERRAIN_GRID_LEVEL;
    }
    precise float len   = distance(a, b);
    precise float d     = distance(0.5 * (a + b), terrain_tess_origin);
    precise float level = len * terrain_tess_density / max(d, 1.0);
    if (terrain_tess_raster.x > 0.0)
    {
        precise vec4 ca = terrain_tess_clip * vec4(a - terrain_tess_origin, 0.0) + terrain_tess_clip_origin;
        precise vec4 cb = terrain_tess_clip * vec4(b - terrain_tess_origin, 0.0) + terrain_tess_clip_origin;
        if (min(ca.w, cb.w) > 0.0)
        {
            precise vec2 span = (cb.xy / cb.w - ca.xy / ca.w) * terrain_tess_raster;
            level = min(level, length(span));
        }
    }
    return clamp(level, 1.0, TERRAIN_MAX_LEVEL);
}

float ceilOdd(float level)
{
    float n = ceil(level);
    return mod(n, 2.0) == 1.0 ? n : n + 1.0;
}

// SKSurrounds::fineHalfLevel: the parent edge's level halved, exact in float.
float fineHalfLevel(float parent_level, bool fine_odd)
{
    float h = parent_level * 0.5;
    return fine_odd ? min(ceilOdd(h), 31.0) : clamp(ceil(h), 1.0, 32.0);
}

// The level of the patch's edge a -> b on side s, where the patch lies on that side of the node.
float sideLevel(vec3 a, vec3 b, int s, bool on_side)
{
    int mode = on_side ? sk_surrounds_edges[s] : 0;
    if (mode == 1)
    {
        // The fine half of a parent edge twice as long, from the parent's corners, which lie on both grids.
        vec2  dir   = b.xy - a.xy;
        int   along = abs(dir.x) > 0.0 ? 0 : 1;
        float p     = abs(dir.x) + abs(dir.y);
        vec2  pa    = a.xy;
        pa[along]   = floor(a[along] / (2.0 * p)) * (2.0 * p);
        vec2  pb    = pa;
        pb[along]  += 2.0 * p;
        float parent = edgeLevel(vec3(pa, heightAt(pa)), vec3(pb, heightAt(pb)));
        return fineHalfLevel(parent, SK_FINE_ODD);
    }
    if (mode >= 2)
    {
        return 2.0 * fineHalfLevel(edgeLevel(a, b), mode == 3);
    }
    return edgeLevel(a, b);
}

void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    if (gl_InvocationID == 0)
    {
        vec3 c0 = corner(0);
        vec3 c1 = corner(1);
        vec3 c2 = corner(2);
        vec3 c3 = corner(3);
        uint word = gl_PrimitiveID < 32 ? sk_surrounds_mask.x : sk_surrounds_mask.y;
        bool has_heights = (word & (1u << uint(gl_PrimitiveID & 31))) != 0u;
        if (!has_heights || min(min(c0.z, c1.z), min(c2.z, c3.z)) < SK_SURROUNDS_NO_HEIGHT)
        {
            // An outer level of 0 discards the patch.
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelOuter[3] = 0.0;
            gl_TessLevelInner[0] = 0.0;
            gl_TessLevelInner[1] = 0.0;
            return;
        }
        gl_TessLevelOuter[0] = sideLevel(c0, c3, 0, c0.x == 0.0);            // u = 0, west
        gl_TessLevelOuter[1] = sideLevel(c0, c1, 2, c0.y == 0.0);            // v = 0, south
        gl_TessLevelOuter[2] = sideLevel(c1, c2, 1, c1.x == region_scale);   // u = 1, east
        gl_TessLevelOuter[3] = sideLevel(c3, c2, 3, c3.y == region_scale);   // v = 1, north
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
        gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
    }
}
