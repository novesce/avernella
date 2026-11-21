/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/*
 * Noise suppression for the microphone audio a call sends, as LINE's own
 * apps apply: fans, wind, and the hiss some laptops' microphones pick up
 * when the webcam beside them powers on. RNNoise, 48 kHz mono.
 */
struct line_denoise;

/* NULL when LINE_NOISE_SUPPRESSION=0 or RNNoise cannot start. */
struct line_denoise *line_denoise_new(void);
/* Cleans pcm in place; samples is a multiple of 480 (10 ms). */
void line_denoise_frame(struct line_denoise *d, gint16 *pcm, int samples);
void line_denoise_free(struct line_denoise *d);
