/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/* Cairo ARGB32 pixels are B, G, R, A bytes in memory on little-endian hosts. */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define UI_LITTLE_ENDIAN TRUE
#else
#define UI_LITTLE_ENDIAN FALSE
#endif
void ui_premultiply_argb(guint32 *pixels, gsize count);
