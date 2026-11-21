// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "ui_limits.h"

// vk_ctx maps the two SPIR-V bindings to separate descriptor heaps.
const uint image_slots = UI_TEXTURE_SLOTS;
const uint sampler_slots = UI_SAMPLER_SLOTS;
layout(set = 0, binding = 0) uniform texture2D images[image_slots];
layout(set = 0, binding = 1) uniform sampler filters[sampler_slots];

layout(constant_id = 0) const bool opaque_batch = false;

layout(location = 0) noperspective in vec2 local;
layout(location = 1) noperspective in vec2 uv;
layout(location = 2) flat in vec4 shape;
layout(location = 3) flat in uvec4 paint;
layout(location = 4) flat in uint kind;
layout(location = 5) flat in vec4 uv_bounds;
layout(location = 0) out vec4 pixel;

vec4 premultiply(vec4 color)
{
	return vec4(color.rgb * color.a, color.a);
}

void main()
{
	if (kind == 4u) {
		pixel = vec4(textureLod(sampler2D(images[nonuniformEXT(paint.z)],
						 filters[0]), uv, 0.0).rgb, 1.0);
		return;
	}
	if (opaque_batch || kind == 5u) {
		float y = textureLod(sampler2D(images[nonuniformEXT(paint.z)], filters[1]), uv, 0.0).r;
		float u = textureLod(sampler2D(images[nonuniformEXT(paint.x)], filters[1]), uv, 0.0).r;
		float v = textureLod(sampler2D(images[nonuniformEXT(paint.y)], filters[1]), uv, 0.0).r;
		y = max(y - 16.0 / 255.0, 0.0) * (298.0 / 256.0);
		u -= 128.0 / 255.0;
		v -= 128.0 / 255.0;
		pixel = vec4(y + v * (409.0 / 256.0),
			     y - u * (100.0 / 256.0) - v * (208.0 / 256.0),
			     y + u * (516.0 / 256.0), 1.0);
		return;
	}
	vec4 fill = unpackUnorm4x8(paint.x);
	if (kind != 0u) {
		if (paint.z >= image_slots || paint.w >= sampler_slots || kind > 3u)
			discard;
		// Both UI filters sample a single mip level.
		vec4 texel = textureLod(sampler2D(images[nonuniformEXT(paint.z)],
						  filters[nonuniformEXT(paint.w)]),
					clamp(uv, uv_bounds.xy, uv_bounds.zw), 0.0);
		if (kind == 1u) {
			pixel = premultiply(vec4(fill.rgb, fill.a * texel.r));
			return;
		}
		fill *= texel;
	}

	float radius = clamp(shape.z, 0.0, min(shape.x, shape.y));
	vec2 q = abs(local) - shape.xy + radius;
	float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
	float width = max(fwidth(distance), 0.001);
	float edge = clamp(0.5 - distance / width, 0.0, 1.0);
	if (kind == 0u && shape.w > 0.0) {
		float inner = clamp(0.5 - (distance + shape.w) / width,
				    0.0, 1.0);
		vec4 border = unpackUnorm4x8(paint.y);
		pixel = premultiply(fill) * inner +
			premultiply(border) * (edge - inner);
	} else
		pixel = premultiply(fill) * edge;
}
