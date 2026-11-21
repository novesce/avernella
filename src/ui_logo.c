// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_logo.h"
#include "ui_pixels.h"
#include <png.h>

static const guint8 mark_png[] = {
#include "avernella_mark.inc"
};

gboolean ui_logo_read(guint8 *pixels, gboolean network_order)
{
	png_image image = { .version = PNG_IMAGE_VERSION };
	gboolean ok = FALSE;

	if (png_image_begin_read_from_memory(&image, mark_png,
					     sizeof(mark_png)) &&
	    image.width == UI_LOGO_EDGE && image.height == UI_LOGO_EDGE) {
		image.format = network_order || !UI_LITTLE_ENDIAN ?
				       PNG_FORMAT_ARGB :
				       PNG_FORMAT_BGRA;
		ok = png_image_finish_read(&image, NULL, pixels, 0, NULL);
	}
	png_image_free(&image);
	return ok;
}

cairo_surface_t *ui_logo_load(SDL_Window *window)
{
	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, UI_LOGO_EDGE, UI_LOGO_EDGE);
	guint8 *pixels = cairo_image_surface_get_data(surface);

	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
	    !ui_logo_read(pixels, FALSE)) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	/* SDL copies straight-alpha pixels; Cairo then keeps the premultiplied image. */
	SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(
		pixels, UI_LOGO_EDGE, UI_LOGO_EDGE, 32,
		cairo_image_surface_get_stride(surface),
		SDL_PIXELFORMAT_ARGB8888);

	if (icon) {
		SDL_SetWindowIcon(window, icon);
		SDL_FreeSurface(icon);
	}
	ui_premultiply_argb((guint32 *)pixels, UI_LOGO_EDGE * UI_LOGO_EDGE);
	cairo_surface_mark_dirty(surface);
	return surface;
}
