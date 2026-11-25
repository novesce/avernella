// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_motion.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>

static void cadence(void)
{
	const int rates[] = { 60, 75, 120, 144, 165, 240, 360 };

	for (guint n = 0; n < G_N_ELEMENTS(rates); n++) {
		UiFrameClock clock = { 0 };
		int hz = rates[n];

		ui_frame_rate(&clock, hz);
		gint64 first = 100000, now = first;

		for (int i = 0; i < hz; i++) {
			g_assert_cmpint(ui_frame_wait_ms(&clock, now, TRUE), ==,
					0);
			ui_frame_started(&clock, now);
			/* Quantize each wake to milliseconds as SDL does. */
			int wait = ui_frame_wait_ms(&clock, now, TRUE);

			g_assert_cmpint(wait, >, 0);
			now += 1000 * wait;
		}
		g_assert_cmpint(now - first, ==, G_USEC_PER_SEC);
		g_test_message("%d Hz: %d frame intervals in %" G_GINT64_FORMAT
			       " us",
			       hz, hz, now - first);
	}
	UiFrameClock clock = { 0 };

	ui_frame_rate(&clock, 240);
	ui_frame_started(&clock, 123000);
	g_assert_cmpint(ui_frame_wait_ms(&clock, 128000, TRUE), ==,
			0); /* 5 ms in present. */
	ui_frame_started(&clock, 130000);
	g_assert_cmpint(clock.next_us, ==,
			131334); /* Missed frames do not queue. */
	ui_frame_rate(&clock, 90);
	g_assert_cmpint(ui_frame_wait_ms(&clock, 131000, TRUE), ==, 0);
	ui_frame_started(&clock, 131000);
	g_assert_cmpint(clock.next_us, ==, 142112);
	ui_frame_rate(&clock, 0);
	g_assert_cmpint(clock.hz, ==, 60);
	g_assert_cmpint(ui_frame_wait_ms(&clock, 900000, FALSE), ==, -1);
}

static void scrolling(void)
{
	int offset = 0;
	UiScroll scroll = { 0 };

	ui_scroll_by(&scroll, &offset, 400, 2400, 100000, FALSE);
	g_assert_cmpint(offset, ==, 0);
	g_assert_true(ui_scroll_sample(&scroll, &offset, 2400, 160000, FALSE));
	g_assert_cmpint(offset, ==, 350);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 2400, 220000, FALSE));
	g_assert_cmpint(offset, ==, 400);
	g_test_message("400 px page: offsets at 0/60/120 ms = 0/350/400 px");
	for (int i = 0; i < 8; i++)
		ui_scroll_by(&scroll, &offset, .125, 2400, 300000, TRUE);
	g_assert_cmpint(offset, ==, 401);
	ui_scroll_by(&scroll, &offset, 20000, 2400, 400000, FALSE);
	ui_scroll_sample(&scroll, &offset, 2400, 420000, FALSE);
	int interrupted = offset;

	ui_scroll_by(&scroll, &offset, -500, 2400, 420000, FALSE);
	g_assert_cmpint(offset, ==, interrupted);
	g_assert_true(ui_scroll_sample(&scroll, &offset, 2400, 440000, FALSE));
	g_assert_cmpint(offset, <, interrupted);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 2400, 540000, FALSE));
	g_assert_cmpint(offset, ==, interrupted - 500);
	ui_scroll_by(&scroll, &offset, 4000, 2400, 600000, FALSE);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 2100, 601000, TRUE));
	g_assert_cmpint(offset, ==, 2100);
	ui_scroll_by(&scroll, &offset, -10, 2400, 700000, FALSE);
	offset = 30; /* Search / keyboard selection reset the viewport. */
	g_assert_false(ui_scroll_sample(&scroll, &offset, 2400, 701000, FALSE));
	g_assert_cmpint(offset, ==, 30);

	/* Preserve fractional wheel input without waking for invisible motion. */
	ui_scroll_reset(&scroll, offset);
	for (int i = 1; i <= 3; i++) {
		ui_scroll_by(&scroll, &offset, .125, 100, 710000, FALSE);
		g_assert_cmpint(scroll.end_us, ==, 0);
		g_assert_cmpint(offset, ==, 30);
	}
	ui_scroll_by(&scroll, &offset, .125, 100, 710000, FALSE);
	g_assert_cmpint(scroll.end_us, >, 0);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 100, 830000, FALSE));
	g_assert_cmpint(offset, ==, 31);
	ui_scroll_reset(&scroll, offset);
	ui_scroll_by(&scroll, &offset, -1, 100, 900000, FALSE);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 100, 930000, FALSE));
	g_assert_cmpint(offset, ==, 30);
}

static void targets(void)
{
	int offset = 0;
	UiScroll scroll = { 0 };

	/* Keyboard selection eases to an absolute row and lands exactly. */
	ui_scroll_to(&scroll, &offset, 300, 1000, 100000, FALSE);
	g_assert_cmpint(offset, ==, 0);
	g_assert_true(ui_scroll_sample(&scroll, &offset, 1000, 140000, FALSE));
	g_assert_cmpint(offset, >, 150);
	g_assert_cmpint(offset, <, 300);
	/* A held key retargets from the moving position, never backwards. */
	int moving = offset;

	ui_scroll_to(&scroll, &offset, 372, 1000, 140000, FALSE);
	g_assert_true(ui_scroll_sample(&scroll, &offset, 1000, 150000, FALSE));
	g_assert_cmpint(offset, >=, moving);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 1000, 260000, FALSE));
	g_assert_cmpint(offset, ==, 372);
	ui_scroll_to(&scroll, &offset, 5000, 1000, 300000, FALSE);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 1000, 420000, FALSE));
	g_assert_cmpint(offset, ==, 1000);
	/* Reduced motion reaches the same end state in the same call. */
	ui_scroll_to(&scroll, &offset, 64, 1000, 500000, TRUE);
	g_assert_cmpint(offset, ==, 64);
	g_assert_cmpint(scroll.end_us, ==, 0);
	ui_scroll_to(&scroll, &offset, NAN, 1000, 510000, FALSE);
	g_assert_cmpint(offset, ==, 64);
	g_assert_cmpint(scroll.end_us, ==, 0);
}

static void glide(void)
{
	UiGlide g = { 0 };
	const gint64 start = 2000000;

	g_assert_cmpfloat(ui_glide_left(&g, start), ==, 0.);
	/* The first frame still shows the old position: nothing jumps. */
	ui_glide_add(&g, 60., 180000, start, FALSE);
	g_assert_cmpfloat(ui_glide_left(&g, start), ==, 60.);
	double half = ui_glide_left(&g, start + 90000);

	g_assert_cmpfloat(half, >, 0.);
	g_assert_cmpfloat(half, <, 10.);
	/* A second message mid-glide keeps what is left and adds its own. */
	ui_glide_add(&g, 40., 180000, start + 90000, FALSE);
	g_assert_cmpfloat(ui_glide_left(&g, start + 90000), ==, half + 40.);
	double previous = half + 40.;

	for (gint64 t = start + 90000; t <= start + 270000; t += 4167) {
		double left = ui_glide_left(&g, t);

		g_assert_cmpfloat(left, <=, previous);
		g_assert_cmpfloat(left, >=, 0.);
		previous = left;
	}
	g_assert_cmpfloat(ui_glide_left(&g, start + 270000), ==, 0.);
	g_assert_cmpint(g.end_us, ==, 0);
	ui_glide_add(&g, 60., 180000, start, TRUE);
	g_assert_cmpfloat(ui_glide_left(&g, start), ==, 0.);
	g_assert_cmpint(g.end_us, ==, 0);
	ui_glide_add(&g, INFINITY, 180000, start, FALSE);
	g_assert_cmpint(g.end_us, ==, 0);
}

static void wheel_direction(void)
{
	/* A flipped event carries the desktop's natural scrolling already. */
	SDL_MouseWheelEvent natural = { .type = SDL_MOUSEWHEEL,
					.y = -1,
					.direction = SDL_MOUSEWHEEL_FLIPPED };
	SDL_MouseWheelEvent traditional = { .type = SDL_MOUSEWHEEL,
					    .y = 1,
					    .direction =
						    SDL_MOUSEWHEEL_NORMAL };

	g_assert_cmpfloat(ui_wheel_y(&natural), ==, -1.);
	g_assert_cmpfloat(ui_wheel_y(&traditional), ==, 1.);
#if SDL_VERSION_ATLEAST(2, 0, 18)
	/* Touchpads: the precise delta wins over the rounded notch count. */
	natural.preciseY = -.25f;
	natural.y = 0;
	g_assert_cmpfloat(ui_wheel_y(&natural), ==, -.25);
	traditional.preciseY = 2.5f;
	g_assert_cmpfloat(ui_wheel_y(&traditional), ==, 2.5);
#endif
	g_assert_cmpfloat(ui_wheel_steps(3, 0.), ==, 3.);
	g_assert_cmpfloat(ui_wheel_steps(1, NAN), ==, 1.);

	/* Row lists add fractions up to whole rows in either direction. */
	double carry = 0.;
	int rows = 0;

	for (int i = 0; i < 10; i++)
		rows += ui_wheel_rows(.1, &carry);
	g_assert_cmpint(rows, ==, 1);
	for (int i = 0; i < 8; i++)
		rows += ui_wheel_rows(.125, &carry);
	g_assert_cmpint(rows, ==, 2);
	g_assert_cmpint(ui_wheel_rows(.75, &carry), ==, 0);
	/* Reversing drops the old remainder instead of fighting it. */
	g_assert_cmpint(ui_wheel_rows(-.5, &carry), ==, 0);
	g_assert_cmpint(ui_wheel_rows(-.5, &carry), ==, -1);
	g_assert_cmpint(ui_wheel_rows(-3., &carry), ==, -3);
	g_assert_cmpint(ui_wheel_rows(INFINITY, &carry), ==, 0);
	g_assert_cmpint(ui_wheel_rows(0., &carry), ==, 0);

	/* The smooth scroll follows both signs from the same mapping. */
	UiScroll scroll = { 0 };
	int offset = 500;

	ui_scroll_reset(&scroll, offset);
	ui_scroll_by(&scroll, &offset, -48. * ui_wheel_y(&natural), 1000,
		     100000, FALSE);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 1000, 300000, FALSE));
	g_assert_cmpint(offset, ==, 512);
	ui_scroll_by(&scroll, &offset, -48. * ui_wheel_y(&traditional), 1000,
		     400000, FALSE);
	g_assert_false(ui_scroll_sample(&scroll, &offset, 1000, 600000, FALSE));
	g_assert_cmpint(offset, ==, 392);
}

static int compare_us(gconstpointer a, gconstpointer b)
{
	const gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;

	return (x > y) - (x < y);
}

struct test_wakeup {
	Uint32 type;
	gint64 posted;
};

static gpointer post_wake(gpointer data)
{
	struct test_wakeup *wake = data;

	g_usleep(40000);
	SDL_Event e = { .type = wake->type };

	wake->posted = g_get_monotonic_time();
	g_assert_cmpint(SDL_PushEvent(&e), ==, 1);
	return NULL;
}

static void sdl_timing(void)
{
	/*
	 * Exercise the real SDL wait functions with no physical display, worker,
	 * account or GPU. Exact cadence and drift are checked under fake time.
	 */
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_assert_cmpint(SDL_Init(SDL_INIT_VIDEO), ==, 0);
	SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
	const int rates[] = { 60, 144, 240 };

	for (guint r = 0; r < G_N_ELEMENTS(rates); r++) {
		UiFrameClock clock = { 0 };

		ui_frame_rate(&clock, rates[r]);
		gint64 samples[40], first = g_get_monotonic_time(),
				    previous = first, now = first;
		for (guint i = 0; i < G_N_ELEMENTS(samples); i++) {
			ui_frame_started(&clock, now);
			int wait;

			while ((wait = ui_frame_wait_ms(&clock,
							g_get_monotonic_time(),
							TRUE)) > 0) {
				SDL_Event event;

				SDL_WaitEventTimeout(&event, wait);
			}
			now = g_get_monotonic_time();
			samples[i] = now - previous;
			previous = now;
		}
		qsort(samples, G_N_ELEMENTS(samples), sizeof(*samples),
		      compare_us);
		double actual = G_N_ELEMENTS(samples) * 1e6 / (now - first);

		g_test_message(
			"headless SDL %d Hz: %.1f frames/s, p50 %.2f ms, p95 %.2f ms",
			rates[r], actual, samples[20] / 1000.,
			samples[37] / 1000.);
	}
	struct test_wakeup wake = { .type = SDL_RegisterEvents(1) };

	g_assert_cmpuint(wake.type, !=, (Uint32)-1);
	GThread *sender = g_thread_new("fixture-input", post_wake, &wake);
	SDL_Event event;
	gint64 start = g_get_monotonic_time();

	do {
		g_assert_cmpint(SDL_WaitEvent(&event), ==, 1);
	} while (event.type != wake.type);
	gint64 arrival = g_get_monotonic_time();

	g_thread_join(sender);
	g_assert_cmpint(arrival - start, >=, 30000);
	g_test_message(
		"idle: 0 timed frame waits; injected input woke the loop %.2f ms after posting",
		(arrival - wake.posted) / 1000.);
	SDL_Quit();
}

static void preferences(void)
{
	g_setenv("LINE_REDUCE_MOTION", "1", TRUE);
	ui_motion_refresh_preferences();
	g_assert_true(ui_reduce_motion());
	g_setenv("LINE_REDUCE_MOTION", "0", TRUE);
	ui_motion_refresh_preferences();
	g_assert_false(ui_reduce_motion());
	ui_motion_shutdown();
}

static void control_motion(void)
{
	const gint64 start = 1000000;
	UiPress tap = { .down = start, .up = start + 5000 };
	gint64 deadline;

	g_assert_cmpfloat(ui_press_depth(tap, start, &deadline), ==, 0.);
	g_assert_cmpfloat(ui_press_depth(tap, start + 40000, &deadline), ==,
			  1.);
	g_assert_cmpint(deadline, ==, start + 185000);
	const int rates[] = { 60, 90, 120, 144, 240 };

	for (guint i = 0; i < G_N_ELEMENTS(rates); i++) {
		double peak = 0., rebound = 0.;
		for (int frame = 0; frame < rates[i] / 4; frame++) {
			gint64 elapsed =
				(gint64)frame * G_USEC_PER_SEC / rates[i];
			double depth =
				ui_press_depth(tap, start + elapsed, &deadline);

			peak = MAX(peak, depth);
			rebound = MIN(rebound, depth);
		}
		g_assert_cmpfloat(peak, >, .96);
		g_assert_cmpfloat(rebound, <, 0.);
		g_assert_cmpfloat(rebound, >, -.09);
		g_assert_cmpint(deadline, ==, 0);
	}
	tap.up = 0;
	g_assert_cmpfloat(ui_press_depth(tap, start + 900000, &deadline), ==,
			  1.);
	g_assert_cmpint(deadline, ==, 0);
	g_assert_cmpfloat(ui_settle(0.), ==, 1.);
	g_assert_cmpfloat(ui_settle(1.), ==, 0.);
	g_assert_cmpfloat(ui_settle(5.), ==, 0.);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/motion/clock", cadence);
	g_test_add_func("/motion/scroll", scrolling);
	g_test_add_func("/motion/scroll-target", targets);
	g_test_add_func("/motion/glide", glide);
	g_test_add_func("/motion/wheel-direction", wheel_direction);
	g_test_add_func("/motion/preference", preferences);
	g_test_add_func("/motion/control-feedback", control_motion);
	g_test_add_func("/motion/headless-sdl", sdl_timing);
	return g_test_run();
}
