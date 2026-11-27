// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_gpu.h"
#include <dlfcn.h>
#include <glib.h>

static void *module, *state;
static const struct gpu_backend *api;
static const struct gpu_dmabuf_backend *dmabuf;
static struct gpu_device_info device;
static int render_minor = -1;

bool ui_gpu_active(void)
{
	return state != NULL;
}

const char *ui_gpu_device_name(void)
{
	return device.name[0] ? device.name : NULL;
}

uint32_t ui_gpu_device_vendor(void)
{
	return device.vendor;
}

int ui_gpu_render_minor(void)
{
	return render_minor;
}

void ui_gpu_close(void)
{
	if (state)
		api->close(state);
	state = NULL;
	api = NULL;
	dmabuf = NULL;
	device = (struct gpu_device_info){ 0 };
	render_minor = -1;
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
	static const char *const folders[] = { ".", "../lib/avernella" };

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
		if (state) {
			const struct gpu_device_query *query =
				dlsym(module, "avernella_gpu_device_v1");
			const struct gpu_drm_query *drm =
				dlsym(module, "avernella_gpu_drm_v1");

			dmabuf = dlsym(module, "avernella_gpu_dmabuf_v1");
			if (query)
				query->get(state, &device);
			if (drm)
				render_minor = drm->render_minor(state);
			return window;
		}
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

bool ui_gpu_dmabuf_supported(void)
{
	return state && dmabuf && dmabuf->supported(state);
}

int ui_gpu_draw_dmabuf(const struct gpu_scene *scene,
		       const struct gpu_dmabuf *const frames[3],
		       unsigned int *rejected)
{
	if (ui_gpu_dmabuf_supported())
		return dmabuf->draw(state, scene, frames, rejected);
	for (unsigned int i = 0; i < 3; i++)
		if (frames[i])
			*rejected |= 1u << i;
	return 0;
}
