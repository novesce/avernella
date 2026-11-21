/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "render/gpu_backend.h"
#include <SDL.h>

/* Missing module, older driver and classic SDL all use the regular window. */
SDL_Window *ui_gpu_window(const char *title, int x, int y, int w, int h,
			  Uint32 flags);
bool ui_gpu_active(void);
void ui_gpu_close(void);
void ui_gpu_size(int *w, int *h);
int ui_gpu_draw(const struct gpu_scene *scene);
