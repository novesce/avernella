// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
layout(set = 0, binding = 0) uniform sampler2D video;
layout(location = 1) noperspective in vec2 uv;
layout(location = 5) flat in vec4 uv_bounds;
layout(location = 0) out vec4 color;
void main()
{
	vec2 half_texel = 0.5 / vec2(textureSize(video, 0));
	vec2 sample_uv = clamp(uv, uv_bounds.xy + half_texel, uv_bounds.zw - half_texel);
	color = vec4(textureLod(video, sample_uv, 0.0).rgb, 1.0);
}
