/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cairo.h>
#include <glib.h>

/* The caller owns the decoded string. No result means no single usable QR. */
char *line_qr_scan(cairo_surface_t *surface, GError **error);
