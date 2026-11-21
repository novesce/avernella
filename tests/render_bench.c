// SPDX-License-Identifier: GPL-3.0-or-later
/* Repeatable offline render benchmark. All conversations and profiles are local fixtures. */
#include "ui_private.h"
#include "ui_motion.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int compare_us(const void *a, const void *b)
{
	gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;

	return (x > y) - (x < y);
}

int main(void)
{
	const char *driver = g_getenv("LINE_BENCH_VIDEODRIVER");

	SDL_setenv("SDL_VIDEODRIVER", driver ? driver : "dummy", 1);
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,
		    !g_strcmp0(driver, "wayland") ? "1" : "0");
	if (SDL_Init(SDL_INIT_VIDEO))
		return 1;
	ui.window = SDL_CreateWindow("LINE render benchmark", 0, 0, 920, 630,
				     SDL_WINDOW_SHOWN);
	if (!ui.window || !ui_draw_init()) {
		g_printerr("Benchmark display: %s\n", SDL_GetError());
		return 1;
	}
	ui_motion_refresh_preferences();
	ui.connected = ui.active = TRUE;
	ui.chats = json_object_new_array();
	ui.history = json_object_new_array();
	ui.visible = g_array_new(FALSE, FALSE, sizeof(int));
	ui.name = g_strdup("Jordan");
	ui.selected = g_strdup("contact-0899");
	ui.message.value = g_string_new("See you at 5");
	ui.search.value = g_string_new("");
	ui.sending = g_hash_table_new(g_str_hash, g_str_equal);
	for (int i = 0; i < 900; i++) {
		char id[32], name[64];

		g_snprintf(id, sizeof(id), "contact-%04d", i);
		g_snprintf(name, sizeof(name), "Fixture contact %d", i);
		Json *row = json_object_new_object();

		json_object_object_add(row, "id", json_object_new_string(id));
		json_object_object_add(row, "name",
				       json_object_new_string(name));
		json_object_array_add(ui.chats, row);
		g_array_append_val(ui.visible, i);
	}
	for (int i = 0; i < 50; i++) {
		Json *row = json_object_new_object();

		json_object_object_add(
			row, "from",
			json_object_new_string(i % 2 ? "Morgan" : "Jordan"));
		json_object_object_add(row, "own",
				       json_object_new_boolean(i % 2 == 0));
		json_object_object_add(
			row, "text",
			json_object_new_string(
				"The train arrives at half past four. Want to meet outside?"));
		json_object_object_add(
			row, "time",
			json_object_new_int64(1700000000000LL + i * 60000));
		json_object_array_add(ui.history, row);
	}

	enum {
		FRAMES = 360
	};

	gint64 durations[FRAMES];

	ui_draw();
	gboolean partial = !g_strcmp0(g_getenv("LINE_BENCH_DAMAGE"), "1");

	for (int i = -24; i < FRAMES; i++) {
		ui.history_offset = (i + 24) * 3;
		gint64 start = g_get_monotonic_time();

		if (partial)
			ui_draw_region(ui.conversation);
		else
			ui_draw();
		if (i >= 0)
			durations[i] = g_get_monotonic_time() - start;
	}
	qsort(durations, FRAMES, sizeof(*durations), compare_us);
	SDL_RendererInfo renderer;
	const char *backend =
		ui.renderer && !SDL_GetRendererInfo(ui.renderer, &renderer) ?
			renderer.name :
			"window surface";

	printf("%s (%s) 920x630, %d frames, render+submit p50/p95/p99 = %.3f / %.3f / %.3f ms\n",
	       SDL_GetCurrentVideoDriver(), backend, FRAMES,
	       durations[FRAMES / 2] / 1000.,
	       durations[FRAMES * 95 / 100] / 1000.,
	       durations[FRAMES * 99 / 100] / 1000.);
	if (partial) {
		SDL_Surface *surface = ui.renderer ?
					       ui.buffer :
					       SDL_GetWindowSurface(ui.window);

		g_assert_false(SDL_MUSTLOCK(surface));
		g_autofree guint8 *snapshot = g_memdup2(
			surface->pixels, (gsize)surface->pitch * surface->h);
		ui_draw();
		surface = ui.renderer ? ui.buffer :
					SDL_GetWindowSurface(ui.window);
		for (int y = 0; y < surface->h; y++) {
			size_t off = (size_t)y * surface->pitch;

			g_assert_cmpmem(
				snapshot + off,
				surface->w * surface->format->BytesPerPixel,
				(guint8 *)surface->pixels + off,
				surface->w * surface->format->BytesPerPixel);
		}
	}
	ui_draw_shutdown();
	ui_motion_shutdown();
	g_string_free(ui.message.value, TRUE);
	g_string_free(ui.search.value, TRUE);
	g_hash_table_unref(ui.sending);
	g_free(ui.selected);
	g_free(ui.name);
	g_array_unref(ui.visible);
	json_object_put(ui.chats);
	json_object_put(ui.history);
	SDL_DestroyWindow(ui.window);
	SDL_Quit();
}
