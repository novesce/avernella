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

/*
 * Catmull-Rom luma for magnified call video. Each axis has four taps; the
 * middle two have non-negative weights and fold into one bilinear fetch,
 * so nine fetches give the exact 16-tap result. On decoded VP8 scaled up
 * from 480x270 or 320x180 it measured 0.7-1.0 dB PSNR above bilinear.
 */
float luma_bicubic(uint image, vec2 coord)
{
	vec2 size = vec2(textureSize(sampler2D(images[nonuniformEXT(image)],
					       filters[1]), 0));
	vec2 pos = coord * size - 0.5;
	vec2 base = floor(pos);
	vec2 f = pos - base;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 t0 = (base - 0.5) / size;
	vec2 t12 = (base + 0.5 + w2 / w12) / size;
	vec2 t3 = (base + 2.5) / size;
	float sum = 0.0;

	for (int j = 0; j < 3; j++) {
		vec2 row = j == 0 ? vec2(t0.y, w0.y) :
			   j == 1 ? vec2(t12.y, w12.y) :
				    vec2(t3.y, w3.y);
		for (int i = 0; i < 3; i++) {
			vec2 col = i == 0 ? vec2(t0.x, w0.x) :
				   i == 1 ? vec2(t12.x, w12.x) :
					    vec2(t3.x, w3.x);
			sum += textureLod(sampler2D(images[nonuniformEXT(image)],
						    filters[1]),
					  vec2(col.x, row.x), 0.0).r *
			       col.y * row.y;
		}
	}
	return clamp(sum, 0.0, 1.0);
}

void main()
{
	if (kind == 4u) {
		pixel = vec4(textureLod(sampler2D(images[nonuniformEXT(paint.z)],
						 filters[0]), uv, 0.0).rgb, 1.0);
		return;
	}
	if (opaque_batch || kind == 5u) {
		vec2 texels = fwidth(uv) * vec2(textureSize(
			sampler2D(images[nonuniformEXT(paint.z)], filters[1]), 0));
		// Bicubic only where the picture is magnified.
		float y = max(texels.x, texels.y) < 1.0 ?
				  luma_bicubic(paint.z, uv) :
				  textureLod(sampler2D(images[nonuniformEXT(paint.z)],
						       filters[1]), uv, 0.0).r;
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
