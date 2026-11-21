// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_gpu.h"
#include <dlfcn.h>
#include <glib.h>

static void *module, *state;
static const struct gpu_backend *api;

bool ui_gpu_active(void)
{
	return state != NULL;
}

void ui_gpu_close(void)
{
	if (state)
		api->close(state);
	state = NULL;
	api = NULL;
	if (module)
		dlclose(module);
	module = NULL;
}

static bool load_module(void)
{
	g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);

	if (!exe)
		return false;
	g_autofree char *dir = g_path_get_dirname(exe);
	const char *folders[] = { ".", "../lib/avernella" };

	for (size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
		g_autofree char *path = g_build_filename(
			dir, folders[i], "avernella-vulkan.so", NULL);

		module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (module) {
			api = dlsym(module, "avernella_gpu_v1");
			if (api)
				return true;
			ui_gpu_close();
		}
	}
	return false;
}

SDL_Window *ui_gpu_window(const char *title, int x, int y, int w, int h,
			  Uint32 flags)
{
	if (g_strcmp0(g_getenv("LINE_DISABLE_GPU"), "1") &&
	    g_strcmp0(g_getenv("AVERNELLA_VULKAN"), "sdl") && load_module()) {
		SDL_Window *window = SDL_CreateWindow(
			title, x, y, w, h, flags | SDL_WINDOW_VULKAN);

		bool log = !g_strcmp0(g_getenv("LINE_RENDER_LOG"), "1");
		if (window)
			state = api->open(window, log);
		if (state)
			return window;
		if (log)
			g_printerr(
				"Avernella: native Vulkan unavailable, trying SDL\n");
		ui_gpu_close();
		if (window)
			SDL_DestroyWindow(window);
	}
	return SDL_CreateWindow(title, x, y, w, h, flags);
}

void ui_gpu_size(int *w, int *h)
{
	api->size(state, w, h);
}

int ui_gpu_draw(const struct gpu_scene *scene)
{
	return api->draw(state, scene);
}
