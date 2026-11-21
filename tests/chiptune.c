// SPDX-License-Identifier: GPL-3.0-or-later
#include "chiptune.h"
#include <math.h>
#include <string.h>

static double rms(const gint16 *pcm, gsize from, gsize to)
{
	double sum = 0;

	for (gsize i = from; i < to; i++)
		sum += (double)pcm[i] * pcm[i];
	return to > from ? sqrt(sum / (double)(to - from)) : 0;
}

/* Rising zero crossings per second about the window's mean. */
static double pitch(const gint16 *pcm, gsize from, gsize to)
{
	double mean = 0;
	guint crossings = 0;

	for (gsize i = from; i < to; i++)
		mean += pcm[i];
	mean /= (double)(to - from);
	for (gsize i = from + 1; i < to; i++)
		if (pcm[i - 1] - mean < 0 && pcm[i] - mean >= 0)
			crossings++;
	return crossings * (double)CHIP_RATE / (double)(to - from);
}

static void test_every_sound(void)
{
	for (int s = 0; s < CHIP_SOUNDS; s++) {
		gsize n = 0, again = 0;
		int peak = 0;
		gint64 took = g_get_monotonic_time();

		g_autofree gint16 *pcm = chip_render(s, &n);
		g_autofree gint16 *copy = NULL;

		took = g_get_monotonic_time() - took;
		copy = chip_render(s, &again);

		g_assert_nonnull(pcm);
		/* Between a blip and a ringtone period. */
		g_assert_cmpuint(n, >, CHIP_RATE / 10);
		g_assert_cmpuint(n, <, CHIP_RATE * 5);
		for (gsize i = 0; i < n; i++)
			peak = MAX(peak, ABS(pcm[i]));
		g_assert_cmpint(peak, >, 2000);
		g_assert_cmpint(peak, <, 30000);
		/* Rendered on first use from the UI thread. */
		g_assert_cmpint(took, <, 100000);
		g_assert_cmpuint(again, ==, n);
		g_assert_cmpmem(pcm, n * sizeof(*pcm), copy,
				again * sizeof(*copy));
		if (!chip_loops(s))
			/* One-shots fade out rather than stop on a click. */
			g_assert_cmpfloat(rms(pcm, n - CHIP_RATE / 100, n), <,
					  400);
	}
}

static void test_loops(void)
{
	g_assert_true(chip_loops(CHIP_RINGTONE));
	g_assert_true(chip_loops(CHIP_RINGBACK));
	g_assert_false(chip_loops(CHIP_MESSAGE));
	g_assert_false(chip_loops(CHIP_HANGUP));
	g_assert_false(chip_loops(CHIP_SOUNDS));
}

static void test_tuning(void)
{
	gsize n;
	g_autofree gint16 *pcm = chip_render(CHIP_MESSAGE, &n);
	/* The chime opens on B5 (987.77 Hz) on a 25% pulse. */
	double hz = pitch(pcm, CHIP_RATE / 100, CHIP_RATE * 6 / 100);

	g_assert_cmpfloat(fabs(hz - 987.77) / 987.77, <, 0.03);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/chiptune/every-sound", test_every_sound);
	g_test_add_func("/chiptune/loops", test_loops);
	g_test_add_func("/chiptune/tuning", test_tuning);
	return g_test_run();
}
