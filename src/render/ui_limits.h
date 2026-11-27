/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared with GLSL. One canvas and three I420 tiles use ten image slots. */
#define UI_TEXTURE_SLOTS 16
#define UI_SAMPLER_SLOTS 2
/* Primitive flags above the low eight-bit shape and two rotation bits. */
#define UI_VIDEO_BICUBIC (1u << 10)
