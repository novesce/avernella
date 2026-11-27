/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "render/gpu_backend.h"
#include "render/gpu_dmabuf.h"
#include <SDL.h>

/* Missing module, older driver and classic SDL all use the regular window. */
SDL_Window *ui_gpu_window(const char *title, int x, int y, int w, int h,
			  Uint32 flags);
bool ui_gpu_active(void);
const char *ui_gpu_device_name(void);
uint32_t ui_gpu_device_vendor(void);
int ui_gpu_render_minor(void);
void ui_gpu_close(void);
void ui_gpu_size(int *w, int *h);
int ui_gpu_draw(const struct gpu_scene *scene);
bool ui_gpu_dmabuf_supported(void);
int ui_gpu_draw_dmabuf(const struct gpu_scene *scene,
		       const struct gpu_dmabuf *const frames[3],
		       unsigned int *rejected);
