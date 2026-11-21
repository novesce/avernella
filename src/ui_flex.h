/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

struct ui_flex_layout;

/* Return a borrowed Cairo image surface, or NULL while the image is absent. */
typedef cairo_surface_t *(*UiFlexImage)(const char *url, gpointer data);

struct ui_flex_layout *ui_flex_new(PangoContext *context, Json *flex,
				   int max_width);
int ui_flex_width(const struct ui_flex_layout *layout);
int ui_flex_height(const struct ui_flex_layout *layout);
void ui_flex_draw(cairo_t *cr, const struct ui_flex_layout *layout, int x,
		  int y, UiFlexImage image, gpointer data);
const char *ui_flex_uri_at(const struct ui_flex_layout *layout, int x, int y);
void ui_flex_free(struct ui_flex_layout *layout);
