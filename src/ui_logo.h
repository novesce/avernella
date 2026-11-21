/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL.h>
#include <cairo.h>
#include <glib.h>

enum {
	UI_LOGO_EDGE = 64
};

/* Straight-alpha ARGB pixels for SDL, Cairo preparation, or a tray host. */
gboolean ui_logo_read(guint8 *pixels, gboolean network_order);
cairo_surface_t *ui_logo_load(SDL_Window *window);
