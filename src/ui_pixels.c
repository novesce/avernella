// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_pixels.h"

/*
 * Two independent 16-bit lanes hold red and blue. The rounded divide by 255
 * is exact across the 8-bit input range. GCC can vectorize across pixels.
 */
void ui_premultiply_argb(guint32 *restrict pixels, gsize count)
{
	for (gsize i = 0; i < count; i++) {
		guint32 argb = pixels[i], alpha = argb >> 24;
		guint32 rb = (argb & 0x00ff00ffu) * alpha + 0x00800080u;
		guint32 green = ((argb >> 8) & 255u) * alpha + 128u;

		rb = (rb + ((rb >> 8) & 0x00ff00ffu)) >> 8;
		green = (green + (green >> 8)) >> 8;
		pixels[i] = (alpha << 24) | (rb & 0x00ff00ffu) | (green << 8);
	}
}
