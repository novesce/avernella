// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require

// vk_ctx maps the two SPIR-V bindings to separate descriptor heaps.
const uint image_slots = 256u;
const uint sampler_slots = 2u;
layout(set = 0, binding = 0) uniform texture2D images[image_slots];
layout(set = 0, binding = 1) uniform sampler filters[sampler_slots];

layout(location = 0) in vec2 local;
layout(location = 1) in vec2 uv;
layout(location = 2) flat in vec4 shape;
layout(location = 3) flat in uvec4 paint;
layout(location = 4) flat in uint kind;
layout(location = 5) flat in vec4 clip;
layout(location = 6) flat in vec4 uv_bounds;
layout(location = 0) out vec4 pixel;

vec4 premultiply(vec4 color)
{
	return vec4(color.rgb * color.a, color.a);
}

void main()
{
	float radius = clamp(shape.z, 0.0, min(shape.x, shape.y));
	vec2 q = abs(local) - shape.xy + radius;
	float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
	float width = max(fwidth(distance), 0.001);
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	vec2 position = gl_FragCoord.xy;
	if (any(lessThan(position, clip.xy)) ||
	    any(greaterThanEqual(position, clip.zw)))
		discard;

	vec4 fill = unpackUnorm4x8(paint.x);
	float edge = clamp(0.5 - distance / width, 0.0, 1.0);
	if (kind == 0u) {
		float inner = clamp(0.5 - (distance + max(shape.w, 0.0)) / width,
				    0.0, 1.0);
		vec4 border = unpackUnorm4x8(paint.y);
		pixel = premultiply(fill) * inner +
			premultiply(border) * (edge - inner);
	} else {
		if (paint.z >= image_slots || paint.w >= sampler_slots || kind > 3u)
			discard;
		vec4 texel = textureGrad(sampler2D(images[nonuniformEXT(paint.z)],
						filters[nonuniformEXT(paint.w)]),
					 clamp(uv, uv_bounds.xy, uv_bounds.zw), dx, dy);
		if (kind == 1u)
			pixel = premultiply(vec4(fill.rgb, fill.a * texel.r));
		else
			pixel = premultiply(fill * texel) * edge;
	}
}
