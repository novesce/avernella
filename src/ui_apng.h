/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cairo.h>
#include <glib.h>

/*
 * Attach on the image decode worker before publishing the poster. The render
 * thread subsequently borrows a composed image and the absolute monotonic time
 * of the next change. Static and finished images return a zero deadline.
 */
void ui_apng_attach(cairo_surface_t *poster, const guint8 *encoded,
		    gsize bytes);
cairo_surface_t *ui_gif_decode(const guint8 *encoded, gsize bytes, int limit,
			       int max_edge, gboolean animate);
void ui_apng_replay(cairo_surface_t *poster);
cairo_surface_t *ui_apng_frame(cairo_surface_t *poster, gint64 now_us,
			       gint64 *until_us);
