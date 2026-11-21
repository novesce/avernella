// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_denoise.h"
#include <math.h>
#include <string.h>

#define RATE 48000
#define FRAME 960

static double rms(const gint16 *pcm, gsize n)
{
	double sum = 0;

	for (gsize i = 0; i < n; i++)
		sum += (double)pcm[i] * pcm[i];
	return sqrt(sum / (double)n);
}

/* Wind-like noise: white noise through a one-pole low-pass. */
static void wind(GRand *rand, gint16 *pcm, gsize n, double *state)
{
	for (gsize i = 0; i < n; i++) {
		*state += 0.15 *
			  (g_rand_double_range(rand, -6000, 6000) - *state);
		pcm[i] = (gint16)*state;
	}
}

static void test_removes_wind(void)
{
	g_autoptr(GRand) rand = g_rand_new_with_seed(7);
	struct line_denoise *d;
	double state = 0, in = 0, out = 0;
	guint frames = 0;

	g_unsetenv("LINE_NOISE_SUPPRESSION");
	d = line_denoise_new();
	g_assert_nonnull(d);
	for (int i = 0; i < 3 * RATE / FRAME; i++) {
		gint16 pcm[FRAME];

		wind(rand, pcm, FRAME, &state);
		double before = rms(pcm, FRAME);

		line_denoise_frame(d, pcm, FRAME);
		/* Let the network settle for the first half second. */
		if (i >= RATE / FRAME / 2) {
			in += before;
			out += rms(pcm, FRAME);
			frames++;
		}
	}
	line_denoise_free(d);
	g_assert_cmpuint(frames, >, 0);
	g_assert_cmpfloat(in / frames, >, 300);
	/* At least 10 dB quieter. */
	g_assert_cmpfloat(20 * log10(in / out), >, 10);
}

static void test_can_be_turned_off(void)
{
	gint16 pcm[FRAME], copy[FRAME];

	for (int i = 0; i < FRAME; i++)
		pcm[i] = copy[i] = (gint16)(i * 37 % 4000 - 2000);
	g_setenv("LINE_NOISE_SUPPRESSION", "0", TRUE);
	struct line_denoise *d = line_denoise_new();

	g_assert_null(d);
	line_denoise_frame(d, pcm, FRAME);
	g_assert_cmpmem(pcm, sizeof(pcm), copy, sizeof(copy));
	line_denoise_free(d);
	g_unsetenv("LINE_NOISE_SUPPRESSION");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/denoise/removes-wind", test_removes_wind);
	g_test_add_func("/denoise/can-be-turned-off", test_can_be_turned_off);
	return g_test_run();
}
