/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_UI_BATCH_H
#define LINE_UI_BATCH_H

#include <stddef.h>
#include <stdint.h>

#define UI_TEXTURE_SLOTS 256
#define UI_SAMPLER_SLOTS 2
#define UI_NO_CLIP UINT32_MAX

enum ui_prim_kind {
	UI_PRIM_QUAD,
	UI_PRIM_GLYPH,
	UI_PRIM_IMAGE,
	UI_PRIM_DMABUF,
};

/* Matches the scalar layout in ui_uber.vert. Colors pack R in bits 0–7. */
struct ui_prim {
	_Alignas(64) float pos[2];
	float size[2];
	float uv_min[2];
	float uv_max[2];
	uint32_t color, border_color;
	float corner_radius, border_width;
	uint32_t tex_index, smp_index, prim_type, clip_rect_idx;
};

struct ui_clip {
	float bounds[4]; /* left, top, right, bottom in drawable pixels */
};

struct ui_push {
	uint64_t prims, clips;
	float inv_viewport[2];
	uint32_t clip_count;
};

_Static_assert(sizeof(struct ui_prim) == 64, "UI primitive stride");
_Static_assert(_Alignof(struct ui_prim) == 64, "UI primitive alignment");
_Static_assert(offsetof(struct ui_prim, tex_index) == 48, "UI texture offset");
_Static_assert(offsetof(struct ui_prim, clip_rect_idx) == 60, "UI clip offset");
_Static_assert(sizeof(struct ui_clip) == 16, "UI clip stride");
_Static_assert(offsetof(struct ui_push, inv_viewport) == 16,
	       "UI viewport offset");
_Static_assert(offsetof(struct ui_push, clip_count) == 24,
	       "UI clip count offset");

#endif
