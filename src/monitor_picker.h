/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

enum {
	LINE_MONITOR_LIMIT = 32,
	LINE_MONITOR_SELECTED = 32,
};

char *line_pick_share_monitor(const char *const *connectors,
			      const char *const *labels, guint count,
			      GCancellable *cancel, GError **error);
int ui_pick_share_monitor(int count, char **labels);
