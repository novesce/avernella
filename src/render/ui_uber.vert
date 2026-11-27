// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require
#include "ui_limits.h"

struct UiPrim {
	vec2 pos, size, uv_min, uv_max;
	uint color, border_color;
	float corner_radius, border_width;
	uint tex_index, smp_index, prim_type, clip_rect_idx;
};

layout(buffer_reference, scalar, buffer_reference_align = 64) readonly buffer PrimBuffer {
	UiPrim items[];
};

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ClipBuffer {
	vec4 bounds[];
};

layout(push_constant, scalar) uniform PushData {
	PrimBuffer prims;
	ClipBuffer clips;
	vec2 inv_viewport;
	uint clip_count;
} pc;

layout(constant_id = 0) const bool opaque_batch = false;

layout(location = 0) noperspective out vec2 local;
layout(location = 1) noperspective out vec2 uv;
layout(location = 2) flat out vec4 shape;
layout(location = 3) flat out uvec4 paint;
layout(location = 4) flat out uint kind;
layout(location = 5) flat out vec4 uv_bounds;

void main()
{
	UiPrim prim = pc.prims.items[gl_InstanceIndex];
	vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
	kind = prim.prim_type & 255u;
	paint = uvec4(prim.color, prim.border_color, prim.tex_index, prim.smp_index);
	if (opaque_batch || kind == 4u || kind == 5u) {
		vec2 pixel = prim.pos + prim.size * corner;
		gl_Position = vec4(pixel * pc.inv_viewport - 1.0, 0.0, 1.0);
		uint turns = (prim.prim_type >> 8) & 3u;
		if (turns == 1u)
			corner = vec2(corner.y, 1.0 - corner.x);
		else if (turns == 2u)
			corner = 1.0 - corner;
		else if (turns == 3u)
			corner = vec2(1.0 - corner.y, corner.x);
		uv = mix(prim.uv_min, prim.uv_max, corner);
		if (kind == 6u)
			uv_bounds = vec4(prim.uv_min, prim.uv_max);
		kind = prim.prim_type & (255u | UI_VIDEO_BICUBIC);
		return;
	}
	vec2 half_size = prim.size * 0.5;
	float skirt = prim.prim_type == 1u ? 0.0 : 1.0;

	local = (2.0 * corner - 1.0) * (half_size + skirt);
	vec2 center = prim.pos + half_size;
	vec2 pixel = center + local;
	if (prim.clip_rect_idx < pc.clip_count) {
		// A clip includes each pixel whose center is in [min, max).
		vec4 clip = ceil(pc.clips.bounds[prim.clip_rect_idx] - 0.5);
		pixel = min(max(pixel, clip.xy), clip.zw);
		local = pixel - center;
	}
	gl_Position = vec4(pixel * pc.inv_viewport - 1.0, 0.0, 1.0);
	uv = mix(prim.uv_min, prim.uv_max, 0.5 + local / max(prim.size, 0.001));
	uv_bounds = vec4(min(prim.uv_min, prim.uv_max), max(prim.uv_min, prim.uv_max));
	shape = vec4(half_size, prim.corner_radius, prim.border_width);
}
