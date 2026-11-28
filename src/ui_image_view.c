// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Cursor anchors and edge clamping adapted from Telegram Desktop's
 * Media::View::OverlayWidget::setZoomLevel() and snapXY().
 * Copyright (c) 2014-2026 The Telegram Desktop Authors.
 * Adapted for Avernella's SDL and Cairo/Vulkan canvas in 2026.
 * The Telegram Desktop OpenSSL linking exception applies to this code.
 * https://github.com/telegramdesktop/tdesktop/blob/fca0d08b6852df2195e11db91f7a4e3be2cbc4a4/LEGAL
 * https://github.com/telegramdesktop/tdesktop/blob/fca0d08b6852df2195e11db91f7a4e3be2cbc4a4/Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp#L7871
 */
#include "ui_image_view.h"
#include <math.h>

enum {
	ZOOM_US = 135000,
	ZOOM_LIMIT = 8
};

static int image_width(const struct ui_image_view *v)
{
	return v->rotation % 180 ? v->height : v->width;
}

static int image_height(const struct ui_image_view *v)
{
	return v->rotation % 180 ? v->width : v->height;
}

static double clamp_center(double center, double start, double view,
			   double extent)
{
	double middle = start + view / 2.;
	double travel = MAX(0., (extent - view) / 2.);

	return CLAMP(center, middle - travel, middle + travel);
}

static void clamp_pan(struct ui_image_view *v)
{
	v->x = clamp_center(v->x, v->viewport.x, v->viewport.w,
			    image_width(v) * v->scale);
	v->y = clamp_center(v->y, v->viewport.y, v->viewport.h,
			    image_height(v) * v->scale);
}

static void fit_scale(struct ui_image_view *v)
{
	v->fit = MIN(v->viewport.w / image_width(v),
		     v->viewport.h / image_height(v));
	v->fit = MIN(v->fit, v->native * ZOOM_LIMIT);
}

void ui_image_view_layout(struct ui_image_view *v, struct ui_image_rect bounds,
			  int width, int height, double pixel_scale)
{
	if (width <= 0 || height <= 0 || bounds.w <= 0 || bounds.h <= 0)
		return;
	double native = 1. / MAX(1., pixel_scale);
	gboolean new_image = width != v->width || height != v->height;

	if (!new_image && native == v->native && bounds.x == v->viewport.x &&
	    bounds.y == v->viewport.y && bounds.w == v->viewport.w &&
	    bounds.h == v->viewport.h)
		return;
	if (new_image)
		*v = (struct ui_image_view){ .fitted = TRUE };
	v->x += bounds.x - v->viewport.x + (bounds.w - v->viewport.w) / 2.;
	v->y += bounds.y - v->viewport.y + (bounds.h - v->viewport.h) / 2.;
	v->zoom_end = 0;
	v->viewport = bounds;
	v->native = native;
	v->width = width;
	v->height = height;
	fit_scale(v);
	if (v->fitted)
		v->scale = v->fit;
	else
		v->scale = CLAMP(v->scale, MIN(v->fit, v->native),
				 MAX(v->fit, v->native) * ZOOM_LIMIT);
	clamp_pan(v);
}

struct ui_image_rect ui_image_view_rect(const struct ui_image_view *v)
{
	double w = image_width(v) * v->scale, h = image_height(v) * v->scale;

	return (struct ui_image_rect){ v->x - w / 2., v->y - h / 2., w, h };
}

static void scale_around(struct ui_image_view *v, double scale, double anchor_x,
			 double anchor_y)
{
	double ratio = scale / v->scale;

	v->x = anchor_x + (v->x - anchor_x) * ratio;
	v->y = anchor_y + (v->y - anchor_y) * ratio;
	v->scale = scale;
	clamp_pan(v);
}

void ui_image_view_sample(struct ui_image_view *v, gint64 now)
{
	if (!v->zoom_end)
		return;
	double time = CLAMP((double)(now - v->zoom_start) / ZOOM_US, 0., 1.);
	double remain = 1. - time;
	double t = 1. - remain * remain * remain;

	/*
	 * Sample from the original pose every time. Intermediate edge clamps
	 * cannot move the point under the cursor once that edge becomes free.
	 */
	v->x = v->zoom_x;
	v->y = v->zoom_y;
	v->scale = v->zoom_from;
	scale_around(v, v->zoom_from + (v->zoom_target - v->zoom_from) * t,
		     v->anchor_x, v->anchor_y);
	if (now >= v->zoom_end)
		v->zoom_end = 0;
}

void ui_image_view_zoom(struct ui_image_view *v, double steps, double x,
			double y, gint64 now, gboolean animate)
{
	if (v->scale <= 0. || !isfinite(steps) || steps == 0.)
		return;
	ui_image_view_sample(v, now);
	double start = v->zoom_end ? v->zoom_target : v->scale;
	double next = CLAMP(start * pow(1.25, CLAMP(steps, -8., 8.)),
			    MIN(v->fit, v->native),
			    MAX(v->fit, v->native) * ZOOM_LIMIT);

	if (fabs(next - start) < 1e-8)
		return;
	v->fitted = FALSE;
	if (!animate) {
		v->zoom_end = 0;
		scale_around(v, next, x, y);
		return;
	}
	v->zoom_start = now;
	v->zoom_end = now + ZOOM_US;
	v->zoom_from = v->scale;
	v->zoom_x = v->x;
	v->zoom_y = v->y;
	v->zoom_target = next;
	v->anchor_x = x;
	v->anchor_y = y;
}

void ui_image_view_reset(struct ui_image_view *v)
{
	v->zoom_end = 0;
	v->fitted = !v->fitted;
	v->scale = v->fitted ? v->fit : v->native;
	v->x = v->viewport.x + v->viewport.w / 2.;
	v->y = v->viewport.y + v->viewport.h / 2.;
}

void ui_image_view_rotate(struct ui_image_view *v)
{
	v->zoom_end = 0;
	v->rotation = (v->rotation + 90) % 360;
	fit_scale(v);
	if (v->fitted)
		v->scale = v->fit;
	clamp_pan(v);
}

void ui_image_view_pan(struct ui_image_view *v, double dx, double dy)
{
	v->zoom_end = 0;
	v->x += dx;
	v->y += dy;
	clamp_pan(v);
}
