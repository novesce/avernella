/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

void ui_desktop_notify(const char *name, gboolean incoming_call);
void ui_desktop_notify_text(const char *summary, const char *body);
