// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera.h"

static inline guint32 pixel(int y, int r, int g, int b)
{
	int luma = 298 * MAX(y - 16, 0);

	return (CLAMP((luma + r) >> 8, 0, 255) << 16) |
	       (CLAMP((luma + g) >> 8, 0, 255) << 8) |
	       CLAMP((luma + b) >> 8, 0, 255);
}

void line_camera_xrgb(const struct line_camera_frame *frame, guint32 *pixels)
{
	unsigned int width = frame->width, height = frame->height;
	unsigned int paired_width = width & ~1u;

	for (unsigned int row = 0; row < height; row++) {
		const guint8 *y = frame->plane[0] + row * frame->stride[0];
		const guint8 *u =
			frame->plane[1] + (row / 2) * frame->stride[1];
		const guint8 *v =
			frame->plane[2] + (row / 2) * frame->stride[2];

		for (unsigned int x = 0; x < paired_width; x += 2) {
			int cb = u[x / 2] - 128;
			int cr = v[x / 2] - 128;
			int r = 409 * cr + 128;
			int g = -100 * cb - 208 * cr + 128;
			int b = 516 * cb + 128;

			pixels[x] = pixel(y[x], r, g, b);
			pixels[x + 1] = pixel(y[x + 1], r, g, b);
		}
		if (paired_width != width) {
			int cb = u[paired_width / 2] - 128;
			int cr = v[paired_width / 2] - 128;

			pixels[paired_width] = pixel(y[paired_width],
						     409 * cr + 128,
						     -100 * cb - 208 * cr + 128,
						     516 * cb + 128);
		}
		pixels += width;
	}
}
