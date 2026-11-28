// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_image_view.h"

static const struct ui_image_rect window = { 10, 50, 800, 500 };

static struct ui_image_view open_image(void)
{
	struct ui_image_view v = { 0 };

	ui_image_view_layout(&v, window, 1600, 1000, 1.);
	return v;
}

static void corner_anchor(void)
{
	struct ui_image_view v = open_image();
	const gint64 start = 1000;
	const double x = window.x + 348, y = window.y + 195;
	struct ui_image_rect old = ui_image_view_rect(&v);
	double u = (x - old.x) / old.w, w = (y - old.y) / old.h;

	ui_image_view_zoom(&v, 2., x, y, start, TRUE);
	g_assert_cmpint(v.zoom_end, >, start);
	g_assert_cmpfloat(v.scale, ==, .5);
	gint64 end = v.zoom_end;

	for (gint64 now = start; now <= end; now += 5000) {
		double prev = v.scale;

		ui_image_view_sample(&v, now);
		struct ui_image_rect rect = ui_image_view_rect(&v);

		g_assert_cmpfloat(v.scale, >=, prev);
		g_assert_cmpfloat_with_epsilon((x - rect.x) / rect.w, u, 1e-8);
		g_assert_cmpfloat_with_epsilon((y - rect.y) / rect.h, w, 1e-8);
	}
	g_assert_cmpint(v.zoom_end, ==, 0);
	g_assert_cmpfloat(v.scale, ==, .78125);
}

static void clamped_animation(void)
{
	struct ui_image_view v = { 0 };

	ui_image_view_layout(&v, window, 800, 250, 1.);
	struct ui_image_view instant = v;
	const double x = window.x + 320, y = window.y + 320;

	ui_image_view_zoom(&instant, 6., x, y, 0, FALSE);
	ui_image_view_zoom(&v, 6., x, y, 0, TRUE);
	gint64 end = v.zoom_end;

	for (gint64 now = 0; now <= end; now += 5000)
		ui_image_view_sample(&v, now);
	g_assert_cmpfloat_with_epsilon(v.x, instant.x, 1e-8);
	g_assert_cmpfloat_with_epsilon(v.y, instant.y, 1e-8);
	g_assert_cmpfloat_with_epsilon(v.scale, instant.scale, 1e-8);
}

static void pan_and_rotate(void)
{
	struct ui_image_view v = open_image();

	ui_image_view_rotate(&v);
	g_assert_cmpuint(v.rotation, ==, 90);
	struct ui_image_rect r = ui_image_view_rect(&v);

	g_assert_cmpfloat(r.w, ==, 312.5);
	g_assert_cmpfloat(r.h, ==, 500);
	ui_image_view_reset(&v);
	r = ui_image_view_rect(&v);
	g_assert_cmpfloat(r.w, ==, 1000);
	g_assert_cmpfloat(r.h, ==, 1600);
	ui_image_view_pan(&v, 1e4, -1e4);
	r = ui_image_view_rect(&v);
	g_assert_cmpfloat(r.x, ==, window.x);
	g_assert_cmpfloat(r.y + r.h, ==, window.y + window.h);
	ui_image_view_pan(&v, -1e4, 1e4);
	r = ui_image_view_rect(&v);
	g_assert_cmpfloat(r.x + r.w, ==, window.x + window.w);
	g_assert_cmpfloat(r.y, ==, window.y);
	ui_image_view_reset(&v);
	r = ui_image_view_rect(&v);
	g_assert_cmpfloat(r.x, ==, window.x + (window.w - r.w) / 2.);
	g_assert_cmpfloat(r.y, ==, window.y);
}

static void dpi_and_interrupt(void)
{
	struct ui_image_view v = open_image();
	struct ui_image_rect smaller = { 0, 0, 400, 250 };

	ui_image_view_layout(&v, smaller, 1600, 1000, 2.);
	g_assert_cmpfloat(v.scale, ==, .25);
	ui_image_view_reset(&v);
	g_assert_cmpfloat(v.scale, ==, .5);
	ui_image_view_zoom(&v, 1, 60, 90, 1000, TRUE);
	gint64 end = v.zoom_end;

	ui_image_view_sample(&v, (end + 1000) / 2);
	double midway = v.scale;

	ui_image_view_pan(&v, 20, -30);
	ui_image_view_sample(&v, end);
	g_assert_cmpfloat(v.scale, ==, midway);
	g_assert_cmpint(v.zoom_end, ==, 0);
	ui_image_view_zoom(&v, -8, 200, 125, end, FALSE);
	g_assert_cmpfloat(v.scale, ==, .25);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/image-view/cursor-anchor", corner_anchor);
	g_test_add_func("/image-view/clamped-animation", clamped_animation);
	g_test_add_func("/image-view/pan-and-rotate", pan_and_rotate);
	g_test_add_func("/image-view/dpi-and-interrupt", dpi_and_interrupt);
	return g_test_run();
}
