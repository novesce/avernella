// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * RNNoise (BSD-3-Clause, https://gitlab.xiph.org/xiph/rnnoise): a small
 * recurrent network that estimates speech and noise bands every 10 ms and
 * gains them, which removes steady hiss and fan noise without the musical
 * artefacts of spectral subtraction. Its frames are 480 samples at 48 kHz,
 * so a 20 ms call frame is two calls. It reads and writes floats on the
 * 16-bit scale.
 */
#include "call_denoise.h"
#include <math.h>
#include <rnnoise.h>

struct line_denoise {
	DenoiseState *state;
	int frame;
};

struct line_denoise *line_denoise_new(void)
{
	if (!g_strcmp0(g_getenv("LINE_NOISE_SUPPRESSION"), "0"))
		return NULL;
	int frame = rnnoise_get_frame_size();
	DenoiseState *state = frame == 480 ? rnnoise_create(NULL) : NULL;

	if (!state)
		return NULL;
	struct line_denoise *d = g_new(struct line_denoise, 1);

	d->state = state;
	d->frame = frame;
	return d;
}

void line_denoise_frame(struct line_denoise *d, gint16 *pcm, int samples)
{
	float buffer[480];

	if (!d || !pcm)
		return;
	for (int at = 0; at + d->frame <= samples; at += d->frame) {
		for (int i = 0; i < d->frame; i++)
			buffer[i] = pcm[at + i];
		rnnoise_process_frame(d->state, buffer, buffer);
		for (int i = 0; i < d->frame; i++)
			pcm[at + i] =
				(gint16)CLAMP(lrintf(buffer[i]), -32768, 32767);
	}
}

void line_denoise_free(struct line_denoise *d)
{
	if (!d)
		return;
	rnnoise_destroy(d->state);
	g_free(d);
}
