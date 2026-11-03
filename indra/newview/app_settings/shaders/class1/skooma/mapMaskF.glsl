/**
 * @file mapMaskF.glsl
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

// The mouselook map: each layer masked by its own fade, its colour premultiplied; the radial gradients are two
// linear segments meeting at a middle stop.

out vec4 frag_color;

in vec2 vary_texcoord0;

uniform sampler2D diffuseMap;
uniform int sk_mode;
uniform float sk_side;
uniform float sk_outer;
uniform float sk_centre;
uniform float sk_self;

float ramp(float t, float a0, float tm, float am, float a1)
{
    t = clamp(t, 0.0, 1.0);
    return t < tm ? mix(a0, am, t / tm) : mix(am, a1, (t - tm) / (1.0 - tm));
}

void main()
{
    vec2 t = vary_texcoord0;
    vec4 p = texture(diffuseMap, t);
    float d = length((t - 0.5) * sk_side);
    float k = 1.0;
    if (sk_mode < 2)
    {
        float r = sk_side * 0.45;
        float inner = r * (1.0 - sk_outer);
        k *= ramp((d - inner) / max(r - inner, 1e-3), 1.0, 0.48, 0.58, 0.0);
    }
    if (sk_mode == 0)
    {
        float r0 = sk_centre * 0.18;
        k *= 1.0 - ramp((d - r0) / max(sk_centre - r0, 1e-3), 0.92, 0.38, 0.70, 0.0);
    }
    else if (sk_mode == 2)
    {
        float r0 = min(4.0, sk_self * 0.3);
        k *= 1.0 - ramp((d - r0) / max(sk_self - r0, 1e-3), 1.0, 0.5, 0.5, 0.0);
    }
    frag_color = p * k;
}
