/**
 * @file overviewGeomF.glsl
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

// Overview census geometry: the class code in R and the triangle's own world normal, octahedron-packed, in G and A.
// The normal comes from the triangle being drawn, not from depth, so parts lying flush on one plane write the same
// normal whatever their depth differs by. Nothing above the cut plane, or its part's own cut height, is drawn: the
// vertex stage clips it (overviewGeomV.glsl). Only SK_DISSOLVE discards, and only SK_CAP_LIFT writes gl_FragDepth.

#ifdef SK_CAP_LIFT
// The lift only moves a fragment nearer: it cannot be rejected early, but saying so keeps the hierarchical depth valid
// for the draws after it where the context can be told; a 4.1 context without the extension builds without the qualifier.
#if __VERSION__ < 420 && defined(GL_ARB_conservative_depth)
#extension GL_ARB_conservative_depth : enable
#endif
#if __VERSION__ >= 420 || defined(GL_ARB_conservative_depth)
#ifdef REVERSE_Z
layout(depth_greater) out float gl_FragDepth;
#else
layout(depth_less) out float gl_FragDepth;
#endif
#endif
#endif

out vec4 frag_color;

uniform vec4 color;             // R: the class code / 255
uniform mat3 sk_eye_to_world;
uniform int sk_debug_cut;       // 1: G and A carry the part's cut height for debug view 5, not its normal

in vec3 vary_position;
#ifdef SK_CUT_ATTRIBUTE
#ifdef SK_CAP_LIFT
// Shared matrix stack + derived matrices, spliced from
// class1/deferred/matricesBlock.glsl and bound at UB_MATRICES.
//[ENGINE_BLOCK Matrices]
#endif

in float vary_cut_z;
in float vary_z;

#ifdef SK_DISSOLVE
#define SK_MAX_DISSOLVE 16
uniform int sk_dissolve_count;
uniform vec4 sk_dissolve[SK_MAX_DISSOLVE];       // window x, y in pixels, eye depth in metres, radius in pixels
uniform float sk_dissolve_floor[SK_MAX_DISSOLVE]; // their floor, agent z: nothing at or under it is dissolved
uniform float sk_dissolve_soft;                   // the fraction of the radius the edge fades over

// Whether the see-through round someone takes this fragment: nearer the eye than them, within their circle on screen,
// dithered on a 4x4 Bayer pattern across the circle's soft rim.
bool skDissolved()
{
    const float BAYER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 cell = ivec2(gl_FragCoord.xy) & 3;
    float threshold = (BAYER[cell.y * 4 + cell.x] + 0.5) / 16.0;
    float depth = -vary_position.z;
    for (int i = 0; i < sk_dissolve_count; ++i)
    {
        vec4 d = sk_dissolve[i];
        if (d.w <= 0.0 || depth > d.z - 0.6 || vary_z <= sk_dissolve_floor[i] + 0.15)
        {
            continue;
        }
        float r = length(gl_FragCoord.xy - d.xy) / d.w;
        float keep = smoothstep(1.0 - max(sk_dissolve_soft, 1.0e-3), 1.0, r);
        if (keep < threshold)
        {
            return true;
        }
    }
    return false;
}
#endif
#endif

vec2 skOctEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

void main()
{
#ifdef SK_DISSOLVE
    if (sk_dissolve_count > 0 && skDissolved())
    {
        discard;
    }
#endif
    vec3 n_eye = normalize(cross(dFdx(vary_position), dFdy(vary_position)));
    vec3 n = normalize(sk_eye_to_world * n_eye);
    bool cap = false;
#ifdef SK_CAP_LIFT
    // Pass 3: a back face seen through a part's cut is inside the solid: it stands for the section, so it moves up its
    // view ray onto the cut plane, above any floor that runs through the solid under it. The stencil keeps it to where
    // that ray is inside a cut solid, not behind the outside of one.
    float depth = gl_FragCoord.z;
    if (!gl_FrontFacing)
    {
        vec3 up = vec3(0.0, 0.0, 1.0) * sk_eye_to_world;
        bool ortho = projection_matrix[3][3] > 0.5;
        vec3 o = ortho ? vary_position : vec3(0.0);
        vec3 d = ortho ? vec3(0.0, 0.0, -1.0) : vary_position;
        float denom = dot(up, d);
        if (abs(denom) > 1.0e-6)
        {
            float t = (vary_cut_z - vary_z + dot(up, vary_position - o)) / denom;
            vec4 clip = projection_matrix * vec4(o + t * d, 1.0);
            float ndc = clip.z / clip.w;
            // Never farther than the face, as the qualifier promises: with the eye under the cut the lift would push
            // the fragment away, and the section stays on the back face, where the stencil already confines it.
#ifdef REVERSE_Z
            depth = max(clamp(ndc, 0.0, 1.0), gl_FragCoord.z);
#else
            depth = min(clamp(ndc * 0.5 + 0.5, 0.0, 1.0), gl_FragCoord.z);
#endif
            n = vec3(0.0, 0.0, 1.0);
            cap = true;
        }
    }
    gl_FragDepth = depth;
#endif
    // 0 in both channels means "no normal" (terrain); a real one is kept off it.
    vec2 e = clamp(skOctEncode(n), 1.0 / 255.0, 1.0);
    float flag = color.b;
#ifdef SK_CUT_ATTRIBUTE
    // B: 1 an invisible face, 0.25 a cut part's band just under its own cut, which the resolve caps.
    if (cap || (vary_cut_z < 1.0e29 && vary_cut_z - vary_z < max(0.12, fwidth(vary_z) * 1.5)))
    {
        flag = max(flag, 0.25);
    }
#endif
#ifdef SK_CUT_ATTRIBUTE
    if (sk_debug_cut == 1)
    {
        // G 1 for a part drawn whole, 0.25 for a hidden one, 0.5 for one with a finite cut; A the cut height in 4 m bands.
        bool whole = vary_cut_z >= 1.0e29;
        bool hidden = vary_cut_z <= -1.0e29;
        e = vec2(whole ? 1.0 : hidden ? 0.25 : 0.5, whole || hidden ? 0.0 : fract(vary_cut_z / 4.0));
    }
#endif
    frag_color = vec4(color.r, e.x, flag, e.y);
}
