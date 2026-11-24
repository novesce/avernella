/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL.h>
#include <glib.h>

void ui_tray_start(void);
void ui_tray_stop(void);
void ui_present_window(SDL_Window *window);
gboolean ui_tray_hide(SDL_Window *window);
gboolean ui_tray_event(const SDL_Event *event, SDL_Window *window,
		       gboolean *quitting);
