// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_pixels.h"

static void exact(void)
{
	guint32 buffer[256];

	for (guint alpha = 0; alpha < 256; alpha++) {
		for (guint i = 0; i < 256; i++)
			buffer[i] = (alpha << 24) | (i << 16) |
				    ((255 - i) << 8) | (i ^ 85);
		ui_premultiply_argb(buffer, G_N_ELEMENTS(buffer));
		for (guint i = 0; i < 256; i++) {
			guint32 r = (i * alpha + 127) / 255,
				g = ((255 - i) * alpha + 127) / 255,
				b = ((i ^ 85) * alpha + 127) / 255;
			g_assert_cmphex(buffer[i], ==,
					(alpha << 24) | (r << 16) | (g << 8) |
						b);
		}
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pixels/alpha-rounding", exact);
	return g_test_run();
}
