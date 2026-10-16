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
// normal whatever their depth differs by. Nothing above the cut plane, or its part's own cut height, is drawn.

out vec4 frag_color;

uniform vec4 color;             // R: the class code / 255
uniform vec4 clip_plane;        // eye space; keep dot(p, xyz) + w >= 0
uniform mat3 sk_eye_to_world;
uniform int sk_debug_cut;       // 1: G and A carry the part's cut height for debug view 5, not its normal

in vec3 vary_position;
#ifdef SK_CUT_ATTRIBUTE
in float vary_cut_z;
in float vary_z;
#endif

vec2 skOctEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

void main()
{
    if (dot(vary_position, clip_plane.xyz) + clip_plane.w < 0.0)
    {
        discard;
    }
#ifdef SK_CUT_ATTRIBUTE
    // Above its own part's cut; a hidden part (cut far below anything) only shows in debug view 5.
    if (vary_z > vary_cut_z && !(sk_debug_cut == 1 && vary_cut_z < -1.0e29))
    {
        discard;
    }
#endif
    vec3 n_eye = normalize(cross(dFdx(vary_position), dFdy(vary_position)));
    vec3 n = normalize(sk_eye_to_world * n_eye);
    // 0 in both channels means "no normal" (terrain); a real one is kept off it.
    vec2 e = clamp(skOctEncode(n), 1.0 / 255.0, 1.0);
    float flag = color.b;
#ifdef SK_CUT_ATTRIBUTE
    // B: 1 an invisible face, 0.25 a cut part's band just under its own cut, which the resolve caps.
    if (vary_cut_z < 1.0e29 && vary_cut_z - vary_z < max(0.12, fwidth(vary_z) * 1.5))
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
