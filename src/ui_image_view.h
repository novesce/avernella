/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

struct ui_image_rect {
	double x, y, w, h;
};

struct ui_image_view {
	struct ui_image_rect viewport;
	double x, y, scale, fit, native;
	double zoom_from, zoom_target, zoom_x, zoom_y, anchor_x, anchor_y;
	gint64 zoom_start, zoom_end;
	int width, height;
	guint rotation;
	gboolean fitted;
};

void ui_image_view_layout(struct ui_image_view *v, struct ui_image_rect bounds,
			  int width, int height, double pixel_scale);
struct ui_image_rect ui_image_view_rect(const struct ui_image_view *v);
void ui_image_view_sample(struct ui_image_view *v, gint64 now);
void ui_image_view_zoom(struct ui_image_view *v, double steps, double x,
			double y, gint64 now, gboolean animate);
void ui_image_view_reset(struct ui_image_view *v);
void ui_image_view_rotate(struct ui_image_view *v);
void ui_image_view_pan(struct ui_image_view *v, double dx, double dy);
