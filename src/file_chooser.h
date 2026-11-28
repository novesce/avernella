/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

/* Run off the UI thread. The caller owns the selected local file path. */
char *line_choose_file(const char *parent, const char *title,
		       const char *image_label, GCancellable *cancel,
		       GError **error);
