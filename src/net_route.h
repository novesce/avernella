/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/* Optional Linux default-route notifications, drained by the worker poll loop. */
int line_route_open(void);
gboolean line_route_restored(int fd, gboolean *lost);
