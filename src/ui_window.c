// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_window.h"
#include "xdg-activation-v1-client.h"
#include "xdg-foreign-unstable-v1-client.h"
#include "xdg-foreign-unstable-v2-client.h"
#include <SDL_syswm.h>
#include <string.h>

struct ui_window_export {
	struct zxdg_exported_v1 *v1;
	struct zxdg_exported_v2 *v2;
	char *handle;
};

struct globals {
	uint32_t exporter_v1, exporter_v2, activation;
};

/* Wayland registries also consume compositor resources until disconnect. */
static struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_event_queue *queue;
	struct globals globals;
} wayland;

static void global(void *data, struct wl_registry *registry, uint32_t name,
		   const char *interface, uint32_t version)
{
	struct globals *g = data;

	(void)registry;
	(void)version;
	if (!strcmp(interface, zxdg_exporter_v1_interface.name))
		g->exporter_v1 = name;
	else if (!strcmp(interface, zxdg_exporter_v2_interface.name))
		g->exporter_v2 = name;
	else if (!strcmp(interface, xdg_activation_v1_interface.name))
		g->activation = name;
}

static void global_remove(void *data, struct wl_registry *registry,
			  uint32_t name)
{
	struct globals *g = data;

	(void)registry;
	if (name == g->exporter_v1)
		g->exporter_v1 = 0;
	if (name == g->exporter_v2)
		g->exporter_v2 = 0;
	if (name == g->activation)
		g->activation = 0;
}

static const struct wl_registry_listener registry_events = {
	.global = global,
	.global_remove = global_remove,
};

void ui_window_shutdown(void)
{
	if (wayland.registry)
		wl_registry_destroy(wayland.registry);
	if (wayland.queue)
		wl_event_queue_destroy(wayland.queue);
	memset(&wayland, 0, sizeof(wayland));
}

static gboolean window_registry(struct wl_display *display)
{
	if (wayland.display == display)
		return wayland.registry && wl_display_dispatch_queue_pending(
						   display, wayland.queue) >= 0;
	ui_window_shutdown();
	wayland.display = display;
	wayland.queue = wl_display_create_queue(display);
	if (!wayland.queue)
		return FALSE;
	struct wl_proxy *wrapper = wl_proxy_create_wrapper(display);

	if (!wrapper)
		return FALSE;
	wl_proxy_set_queue(wrapper, wayland.queue);
	wayland.registry =
		wl_display_get_registry((struct wl_display *)wrapper);
	wl_proxy_wrapper_destroy(wrapper);
	if (!wayland.registry)
		return FALSE;
	if (wl_registry_add_listener(wayland.registry, &registry_events,
				     &wayland.globals) ||
	    wl_display_roundtrip_queue(display, wayland.queue) < 0) {
		ui_window_shutdown();
		return FALSE;
	}
	return TRUE;
}

static void handle_v1(void *data, struct zxdg_exported_v1 *exported,
		      const char *handle)
{
	(void)exported;
	struct ui_window_export *out = data;

	g_free(out->handle);
	out->handle = g_strconcat("wayland:", handle, NULL);
}

static void handle_v2(void *data, struct zxdg_exported_v2 *exported,
		      const char *handle)
{
	(void)exported;
	struct ui_window_export *out = data;

	g_free(out->handle);
	out->handle = g_strconcat("wayland:", handle, NULL);
}

static const struct zxdg_exported_v1_listener exported_events_v1 = {
	.handle = handle_v1,
};
static const struct zxdg_exported_v2_listener exported_events_v2 = {
	.handle = handle_v2,
};

static void export_surface(struct ui_window_export *exported,
			   struct wl_registry *registry,
			   struct wl_surface *surface,
			   const struct globals *globals)
{
	if (globals->exporter_v2) {
		struct zxdg_exporter_v2 *proxy =
			wl_registry_bind(registry, globals->exporter_v2,
					 &zxdg_exporter_v2_interface, 1);

		if (!proxy)
			return;
		exported->v2 = zxdg_exporter_v2_export_toplevel(proxy, surface);
		if (exported->v2)
			zxdg_exported_v2_add_listener(
				exported->v2, &exported_events_v2, exported);
		zxdg_exporter_v2_destroy(proxy);
	} else if (globals->exporter_v1) {
		struct zxdg_exporter_v1 *proxy =
			wl_registry_bind(registry, globals->exporter_v1,
					 &zxdg_exporter_v1_interface, 1);

		if (!proxy)
			return;
		exported->v1 = zxdg_exporter_v1_export(proxy, surface);
		if (exported->v1)
			zxdg_exported_v1_add_listener(
				exported->v1, &exported_events_v1, exported);
		zxdg_exporter_v1_destroy(proxy);
	}
}

void ui_window_unexport(struct ui_window_export *exported)
{
	if (!exported)
		return;
	if (exported->v2)
		zxdg_exported_v2_destroy(exported->v2);
	if (exported->v1)
		zxdg_exported_v1_destroy(exported->v1);
	g_free(exported->handle);
	g_free(exported);
}

struct ui_window_export *ui_window_export(SDL_Window *window, char **parent)
{
	SDL_SysWMinfo native = { 0 };

	*parent = NULL;
	SDL_VERSION(&native.version);
	if (!SDL_GetWindowWMInfo(window, &native))
		return NULL;
#ifdef SDL_VIDEO_DRIVER_X11
	if (native.subsystem == SDL_SYSWM_X11) {
		*parent = g_strdup_printf("x11:%lx", native.info.x11.window);
		return NULL;
	}
#endif
#ifdef SDL_VIDEO_DRIVER_WAYLAND
	if (native.subsystem != SDL_SYSWM_WAYLAND || !native.info.wl.display ||
	    !native.info.wl.surface || !native.info.wl.xdg_toplevel)
		return NULL;
	struct wl_display *display = native.info.wl.display;

	if (!window_registry(display))
		return NULL;
	struct ui_window_export *exported = g_new0(struct ui_window_export, 1);

	export_surface(exported, wayland.registry, native.info.wl.surface,
		       &wayland.globals);
	if ((exported->v1 || exported->v2) &&
	    wl_display_roundtrip_queue(display, wayland.queue) >= 0 &&
	    exported->handle) {
		*parent = g_steal_pointer(&exported->handle);
		return exported;
	}
	ui_window_unexport(exported);
#endif
	return NULL;
}

gboolean ui_window_activate(SDL_Window *window, const char *token)
{
#ifdef SDL_VIDEO_DRIVER_WAYLAND
	SDL_SysWMinfo native = { 0 };

	if (!token || !*token)
		return FALSE;
	SDL_VERSION(&native.version);
	if (!SDL_GetWindowWMInfo(window, &native) ||
	    native.subsystem != SDL_SYSWM_WAYLAND || !native.info.wl.display ||
	    !native.info.wl.surface)
		return FALSE;
	struct wl_display *display = native.info.wl.display;

	if (!window_registry(display) || !wayland.globals.activation)
		return FALSE;
	struct xdg_activation_v1 *activation =
		wl_registry_bind(wayland.registry, wayland.globals.activation,
				 &xdg_activation_v1_interface, 1);

	if (!activation)
		return FALSE;
	xdg_activation_v1_activate(activation, token, native.info.wl.surface);
	xdg_activation_v1_destroy(activation);
	wl_display_flush(display);
	return TRUE;
#else
	(void)window;
	(void)token;
	return FALSE;
#endif
}
