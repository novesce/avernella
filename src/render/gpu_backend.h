/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gpu_scene.h"
#include <stdbool.h>

typedef struct SDL_Window SDL_Window;

/* Kept private to this build of Avernella and its renderer. */
struct gpu_backend {
	void *(*open)(SDL_Window *window, bool log);
	void (*close)(void *state);
	void (*size)(void *state, int *width, int *height);
	/* 1 presented, 0 temporarily unavailable, -1 device/surface failure. */
	int (*draw)(void *state, const struct gpu_scene *scene);
};

/* Optional export, independent of the original backend dispatch table. */
struct gpu_device_info {
	char name[256];
	uint32_t vendor;
};

struct gpu_device_query {
	void (*get)(void *state, struct gpu_device_info *device);
};

struct gpu_drm_query {
	int (*render_minor)(void *state);
};
