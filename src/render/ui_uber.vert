// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

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

layout(location = 0) out vec2 local;
layout(location = 1) out vec2 uv;
layout(location = 2) flat out vec4 shape;
layout(location = 3) flat out uvec4 paint;
layout(location = 4) flat out uint kind;
layout(location = 5) flat out vec4 clip;
layout(location = 6) flat out vec4 uv_bounds;

void main()
{
	const vec2 corners[] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1),
				      vec2(0, 1), vec2(1, 0), vec2(1, 1));
	UiPrim prim = pc.prims.items[gl_InstanceIndex];
	vec2 corner = corners[gl_VertexIndex];
	vec2 half_size = prim.size * 0.5;
	float skirt = prim.prim_type == 1u ? 0.0 : 1.0;

	local = (2.0 * corner - 1.0) * (half_size + skirt);
	vec2 pixel = prim.pos + half_size + local;
	gl_Position = vec4(pixel * pc.inv_viewport - 1.0, 0.0, 1.0);
	uv = mix(prim.uv_min, prim.uv_max, 0.5 + local / max(prim.size, 0.001));
	uv_bounds = vec4(min(prim.uv_min, prim.uv_max), max(prim.uv_min, prim.uv_max));
	shape = vec4(half_size, prim.corner_radius, prim.border_width);
	paint = uvec4(prim.color, prim.border_color, prim.tex_index, prim.smp_index);
	kind = prim.prim_type;
	clip = vec4(0, 0, 2.0 / pc.inv_viewport);
	if (prim.clip_rect_idx < pc.clip_count)
		clip = pc.clips.bounds[prim.clip_rect_idx];
}
