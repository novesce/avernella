// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_logo.h"
#include "ui_pixels.h"
#include <png.h>

static const guint8 mark_png[] = {
#include "avernella_mark.inc"
};

static const guint8 welcome_png[] = {
#include "avernella_welcome.inc"
};

static gboolean read_png(const guint8 *data, gsize size, guint edge,
			 guint8 *pixels, gboolean network_order)
{
	png_image image = { .version = PNG_IMAGE_VERSION };
	gboolean ok = FALSE;

	if (png_image_begin_read_from_memory(&image, data, size) &&
	    image.width == edge && image.height == edge) {
		image.format = network_order || !UI_LITTLE_ENDIAN ?
				       PNG_FORMAT_ARGB :
				       PNG_FORMAT_BGRA;
		ok = png_image_finish_read(&image, NULL, pixels, 0, NULL);
	}
	png_image_free(&image);
	return ok;
}

gboolean ui_logo_read(guint8 *pixels, gboolean network_order)
{
	return read_png(mark_png, sizeof(mark_png), UI_LOGO_EDGE, pixels,
			network_order);
}

static cairo_surface_t *load_surface(const guint8 *data, gsize size, guint edge,
				     SDL_Window *window)
{
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, edge, edge);
	guint8 *pixels = cairo_image_surface_get_data(surface);

	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
	    !read_png(data, size, edge, pixels, FALSE)) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	if (window) {
		/* Set the SDL icon before premultiplying the shared pixels for Cairo. */
		SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(
			pixels, edge, edge, 32,
			cairo_image_surface_get_stride(surface),
			SDL_PIXELFORMAT_ARGB8888);

		if (icon) {
			SDL_SetWindowIcon(window, icon);
			SDL_FreeSurface(icon);
		}
	}
	ui_premultiply_argb((guint32 *)pixels, edge * edge);
	cairo_surface_mark_dirty(surface);
	return surface;
}

cairo_surface_t *ui_logo_load(SDL_Window *window)
{
	return load_surface(mark_png, sizeof(mark_png), UI_LOGO_EDGE, window);
}

cairo_surface_t *ui_logo_portrait(void)
{
	return load_surface(welcome_png, sizeof(welcome_png), 384, NULL);
}
