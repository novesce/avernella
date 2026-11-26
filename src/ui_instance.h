/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL.h>
#include <glib.h>

/* 1 owns the profile's window, 0 activated its owner, -1 on error. */
int ui_instance_open(GError **error);
int ui_instance_open_url(const char *url, GError **error);
gboolean ui_instance_watch(Uint32 event, GError **error);
void ui_instance_close(void);
