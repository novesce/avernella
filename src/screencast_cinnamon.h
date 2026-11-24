/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

struct line_cinnamon_cast;

/* Called on the capture thread with context pushed as its thread default. */
struct line_cinnamon_cast *line_cinnamon_start(GMainContext *context,
					       GCancellable *cancel,
					       guint32 *node, GError **error);
void line_cinnamon_stop(struct line_cinnamon_cast *cast);
