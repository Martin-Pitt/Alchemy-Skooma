/**
 * @file surroundsF.glsl
 *
 * A distant region's map tile into the G-buffer. The tile is a photograph already lit, so it is flagged to skip the
 * lighting (softenLightF's unlit branch) and the haze pass still fogs it.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

uniform sampler2D diffuseMap;

in vec3 vary_normal;
in vec2 vary_tile_uv;

vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

void main()
{
    vec3 color = texture(diffuseMap, vary_tile_uv).rgb;
    frag_data[0] = vec4(max(color, vec3(0.0)), 0.0);
    frag_data[1] = vec4(0.0, 0.0, 0.0, -1.0);
    frag_data[2] = encodeNormal(normalize(vary_normal), 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0.0);
#endif
}
