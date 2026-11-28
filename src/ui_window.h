/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL.h>
#include <glib.h>

struct ui_window_export;

/* Keep the Wayland export alive until the portal closes its dialog. */
struct ui_window_export *ui_window_export(SDL_Window *window, char **parent);
void ui_window_unexport(struct ui_window_export *exported);
gboolean ui_window_activate(SDL_Window *window, const char *token);
void ui_window_shutdown(void);
