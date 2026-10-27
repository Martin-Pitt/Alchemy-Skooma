/**
 * @file overviewCompositeF.glsl
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

// Tactical overview: the resolved overview over a near-black ground, blurred toward the top and bottom of the view
// (tilt-shift) so the band around the agent stays sharp, darkened toward the corners.

out vec4 frag_color;

uniform sampler2D diffuseMap;
uniform vec4 sk_viewport;       // x, y, width, height of the world view in window pixels
uniform float sk_tilt_px;       // blur radius at the top and bottom edges
uniform vec3 sk_ground;

vec3 over(vec4 s)
{
    return mix(sk_ground, s.rgb, s.a);
}

void main()
{
    vec2 uv = (gl_FragCoord.xy - sk_viewport.xy) / sk_viewport.zw;
    float band = clamp(abs(uv.y - 0.5) * 2.0 - 0.25, 0.0, 1.0) / 0.75;
    float r = sk_tilt_px * band * band;
    vec2 texel = 1.0 / sk_viewport.zw;
    float vignette = 1.0 - 0.35 * smoothstep(0.35, 0.85, length((uv - 0.5) * vec2(1.0, 0.8)));
    if (r < 0.5)
    {
        frag_color = vec4(over(texture(diffuseMap, uv)) * vignette, 1.0);
        return;
    }
    // Twelve taps on two rings, rotated per pixel to hide the pattern.
    vec3 sum = over(texture(diffuseMap, uv));
    float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    for (int i = 0; i < 12; ++i)
    {
        float ang = rot + float(i) * 0.5235988;
        float rad = r * (i < 6 ? 0.5 : 1.0);
        sum += over(texture(diffuseMap, uv + vec2(cos(ang), sin(ang)) * rad * texel));
    }
    frag_color = vec4(sum / 13.0 * vignette, 1.0);
}
