/**
 * @file overviewGeomV.glsl
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

// Overview census geometry: eye-space position for the face's own normal, and the clip distances that keep only what is
// under the cut plane, the ceiling and each part's own cut, so the fragment stage never discards for them.

// Shared matrix stack + derived matrices, spliced from
// class1/deferred/matricesBlock.glsl and bound at UB_MATRICES.
//[ENGINE_BLOCK Matrices]

uniform vec4 clip_plane;        // eye space; keep dot(p, xyz) + w >= 0
uniform vec4 sk_ceiling;        // eye space, the same form: nothing this far over the agent's floor is drawn

in vec3 position;

out vec3 vary_position;
out float gl_ClipDistance[4];   // 0 the clip plane, 1 the ceiling, 2 the part's own cut, 3 the cap pass's parts
#ifdef SK_CUT_ATTRIBUTE
uniform int sk_debug_cut;       // 1: debug view 5, where a hidden part shows whole
uniform int sk_cap_pass;        // 0 front faces, 1 back faces of uncut parts, 2 cut parts' faces counted in stencil, 3 their section
in float weight;        // the linkset id (SKOverviewCensus::Group::mId), whose row of SKOverviewCutTable holds its cut
out float vary_cut_z;
out float vary_z;
in vec2 texcoord0;      // x: 1 for an open part's triangle, which has no inside to section; y: 1 for one passed through
#ifdef SK_ID_WRITE
flat out int vary_group;        // the probes' id render: the linkset each fragment belongs to, no cut read
#else
#ifdef SK_CUT_TABLE_SSBO
layout(std430) readonly buffer SKOverviewCutBlock { vec4 rows[]; };
vec4 skCutRow(int group) { return rows[group]; }
#else
uniform samplerBuffer sk_cut_table;     // the same rows as a buffer texture, from texel sk_cut_base
uniform int sk_cut_base;
vec4 skCutRow(int group) { return texelFetch(sk_cut_table, sk_cut_base + group); }
#endif
#endif
#endif

void main()
{
    vary_position = (modelview_matrix * vec4(position.xyz, 1.0)).xyz;
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    gl_ClipDistance[0] = dot(vary_position, clip_plane.xyz) + clip_plane.w;
    gl_ClipDistance[1] = dot(vary_position, sk_ceiling.xyz) + sk_ceiling.w;
#ifdef SK_CUT_ATTRIBUTE
    // The cut is its linkset's (cut, flags, lowest z, highest z), agent z.
    int sk_group = int(weight + 0.5);
#ifdef SK_ID_WRITE
    vary_group = sk_group;
    // Nothing is cut for a probe; a passed-through triangle (an invisible part's) is clipped whole.
    gl_ClipDistance[2] = 1.0;
    gl_ClipDistance[3] = texcoord0.y > 0.5 ? -1.0 : 1.0;
#else
    vec4 row = skCutRow(sk_group);
    float cut = row.x;
    vary_cut_z = cut;
    vary_z = position.z; // region-local z is agent z
    // Above its own part's cut; a hidden part (cut far below anything) only shows in debug view 5.
    gl_ClipDistance[2] = (sk_debug_cut == 1 && cut < -1.0e29) ? 1.0 : cut - position.z;
    // A cut part's back faces are its section, drawn by passes 2 and 3 only; an uncut or open part's by pass 1.
    // A part's vertices share its cut and its openness, so the sign is the triangle's.
    float cut_part = (cut < 1.0e29 && texcoord0.x < 0.5) ? 1.0 : -1.0;
    gl_ClipDistance[3] = sk_cap_pass == 0 ? 1.0 : sk_cap_pass == 1 ? -cut_part : cut_part;
#endif
#else
    gl_ClipDistance[2] = 1.0;
    gl_ClipDistance[3] = 1.0;
#endif
}
