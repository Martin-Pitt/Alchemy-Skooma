/**
 * @file surroundsTE.glsl
 *
 * A distant region's map-tiled patch, unlit. A vertex on an edge where this patch is the fine half of a coarser
 * neighbour's edge is placed as the coarse side places it: its index k on this half, the parent's index j, the
 * parent's parameter (SKSurrounds::parentParameter) and the height on the coarse grid, so the two sides share every
 * vertex.
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

//[ENGINE_BLOCK Matrices]

#if defined(SK_SURROUNDS_RING) && !defined(TERRAIN_TESS_PINNED)
layout(quads, fractional_odd_spacing, ccw) in;
#else
layout(quads, equal_spacing, ccw) in;
#endif

out vec3 vary_normal;
out vec2 vary_tile_uv;

uniform sampler2D terrain_height_map;
uniform float terrain_grid_scale;
uniform float region_scale;          // the node's edge in metres
uniform ivec4 sk_surrounds_edges;    // see surroundsTC.glsl
uniform vec4  sk_surrounds_tile;     // map tile uv = local / region_scale * xy + zw
uniform vec4  sk_surrounds_params;   // x: the coarse neighbour's grid over this one's

const int TERRAIN_APRON = 2;

// terrainSurface.glsl
vec2 terrain_patch_xy();
float terrain_height_linear(vec2 g);
float terrain_height(vec2 p_region);

float heightTexel(ivec2 g)
{
    return texelFetch(terrain_height_map, g + TERRAIN_APRON, 0).r;
}

// terrain_height_linear on the coarse neighbour's grid, every r-th sample of this map.
float coarseHeight(vec2 p)
{
    int   r = int(sk_surrounds_params.x);
    vec2  g = p / (terrain_grid_scale * float(r));
    ivec2 i = ivec2(floor(g));
    vec2  f = g - vec2(i);
    float lb = heightTexel(i * r);
    float rb = heightTexel((i + ivec2(1, 0)) * r);
    float lt = heightTexel((i + ivec2(0, 1)) * r);
    float rt = heightTexel((i + ivec2(1, 1)) * r);
    return (f.y > f.x) ? lb + f.y * (lt - lb) + f.x * (rt - lt)
                       : lb + f.x * (rb - lb) + f.y * (rt - rb);
}

float parentParameter(int k, int m, bool first_half)
{
    int j  = first_half ? k : k + m;
    int n2 = 2 * m;
    if (2 * j <= n2)
    {
        return float(j) / float(n2);
    }
    precise float q = float(n2 - j) / float(n2);
    return 1.0 - q;
}

float mixExact(float a, float b, float t)
{
    precise float r = a * (1.0 - t) + b * t;
    return r;
}

void main()
{
    vec2 xy = terrain_patch_xy();
    vec2 c0 = gl_in[0].gl_Position.xy;
    vec2 c2 = gl_in[2].gl_Position.xy;
    vec2 tc = gl_TessCoord.xy;

    // Which side of the node, if any, this vertex lies on as the fine half: west, east, south, north.
    int   side = -1;
    float t = 0.0;
    float level = 0.0;
    if (tc.x == 0.0 && c0.x == 0.0 && sk_surrounds_edges[0] == 1)                { side = 0; t = tc.y; level = gl_TessLevelOuter[0]; }
    else if (tc.x == 1.0 && c2.x == region_scale && sk_surrounds_edges[1] == 1)  { side = 1; t = tc.y; level = gl_TessLevelOuter[2]; }
    else if (tc.y == 0.0 && c0.y == 0.0 && sk_surrounds_edges[2] == 1)           { side = 2; t = tc.x; level = gl_TessLevelOuter[1]; }
    else if (tc.y == 1.0 && c2.y == region_scale && sk_surrounds_edges[3] == 1)  { side = 3; t = tc.x; level = gl_TessLevelOuter[3]; }

    float z;
    if (side >= 0)
    {
        int   along = side < 2 ? 1 : 0;
        float p     = c2[along] - c0[along];
        float start = floor(c0[along] / (2.0 * p)) * (2.0 * p);
        int   m     = int(round(level));
        int   k     = int(round(t * float(m)));
        float tp    = parentParameter(k, m, c0[along] == start);
        xy[along]   = mixExact(start, start + 2.0 * p, tp);
        z = coarseHeight(xy);
    }
    else
    {
#ifdef SK_SURROUNDS_RING
        z = terrain_height(xy); // the live surface's, smoothed where it is, so a ring cell meets a live region
#else
        z = terrain_height_linear(xy / terrain_grid_scale);
#endif
    }

    vec4 pre_pos = vec4(xy, z, 1.0);
    gl_Position  = modelview_projection_matrix * pre_pos;
    vary_normal  = normalize(normal_matrix * vec3(0.0, 0.0, 1.0));
    vary_tile_uv = xy / region_scale * sk_surrounds_tile.xy + sk_surrounds_tile.zw;
}
