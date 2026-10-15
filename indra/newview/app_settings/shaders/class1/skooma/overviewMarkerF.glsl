/**
 * @file overviewMarkerF.glsl
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

// Tactical overview avatar markers, depth-tested by hand against the overview's own depth: where the census stands
// in front of a marker it is drawn as a faint see-through silhouette, so walls hide people without losing them.

out vec4 frag_color;

uniform sampler2D sceneDepth;   // the overview capture's depth, the same camera as the markers
uniform vec4 sk_viewport;       // x, y, width, height of the overview in window pixels
uniform float sk_zo;            // 1 when nearer is larger (reverse-Z), 0 when nearer is smaller
uniform float sk_ghost_alpha;   // the silhouette's opacity

in vec4 vertex_color;

void main()
{
    vec2 uv = (gl_FragCoord.xy - sk_viewport.xy) / sk_viewport.zw;
    float scene = texture(sceneDepth, uv).r;
    float d = gl_FragCoord.z;
    // A hair of slack so a marker standing on a floor is not hidden by that floor.
    bool hidden = sk_zo > 0.5 ? (d < scene - 1e-5) : (d > scene + 1e-5);
    frag_color = hidden ? vec4(vertex_color.rgb, vertex_color.a * sk_ghost_alpha) : vertex_color;
}
