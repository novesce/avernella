/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

typedef gboolean (*call_vp8_probe_fn)(const char *node, guint vendor,
				      const char *pci, gpointer data);

gboolean call_vp8_try_render_nodes(gboolean allow_alternates,
				   call_vp8_probe_fn probe, gpointer data);
gboolean call_vp8_cuda_ordinal(const char *pci, char result[16]);
