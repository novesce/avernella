/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

char *line_desktop_screenshot(const char *parent, GCancellable *cancel,
			      GError **error);
