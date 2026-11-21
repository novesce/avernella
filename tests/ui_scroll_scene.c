// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "ui_gpu.h"
#include "ui_motion.h"
#include <string.h>

enum {
	W = 910,
	H = 532
};
static uint32_t texture[H][W], presented[H][W];
static unsigned int sparse, full, uploaded;
static gboolean skip;

/* A deterministic consumer of the production scene's texel-copy commands. */
bool ui_gpu_active(void)
{
	return true;
}

void ui_gpu_close(void)
{
}

void ui_gpu_size(int *w, int *h)
{
	*w = W;
	*h = H;
}

static void copy(const uint8_t *source, int pitch, uint32_t target[H][W],
		 struct gpu_copy c)
{
	g_assert_cmpint(c.src.w, ==, c.dst.w);
	g_assert_cmpint(c.src.h, ==, c.dst.h);
	g_assert_true(c.src.x >= 0 && c.dst.x >= 0 && c.src.y >= 0 &&
		      c.dst.y >= 0 && c.src.x + c.src.w <= W &&
		      c.dst.x + c.dst.w <= W && c.src.y + c.src.h <= H &&
		      c.dst.y + c.dst.h <= H);
	for (int y = 0; y < c.src.h; y++)
		memcpy(&target[c.dst.y + y][c.dst.x],
		       source + (c.src.y + y) * pitch + c.src.x * 4,
		       c.src.w * 4);
}

int ui_gpu_draw(const struct gpu_scene *s)
{
	struct gpu_rect screen = { 0, 0, W, H };

	if (skip) {
		skip = FALSE;
		return 0;
	}
	uploaded = 0;
	if (s->upload_count) {
		sparse++;
		for (guint i = 0; i < s->upload_count; i++) {
			uploaded += s->uploads[i].src.w * s->uploads[i].src.h;
			copy(s->pixels, s->pitch, texture, s->uploads[i]);
		}
	} else {
		if (s->damage.w == W && s->damage.h == H)
			full++;
		copy(s->pixels, s->pitch, texture,
		     (struct gpu_copy){ s->damage, s->damage });
	}
	if (s->copy_count)
		for (guint i = 0; i < s->copy_count; i++)
			copy((const guint8 *)texture, sizeof(texture[0]),
			     presented, s->copies[i]);
	else
		copy((const guint8 *)texture, sizeof(texture[0]), presented,
		     (struct gpu_copy){ screen, screen });
	return 1;
}

static void history(void)
{
	int positions[] = { 0,	 3,   9,   76,	285, 304, 327,
			    448, 512, 405, 377, 305, 11 };
	char *reference[G_N_ELEMENTS(positions)];

	SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
	g_setenv("LINE_REDUCE_MOTION", "1", TRUE);
	g_assert_cmpint(SDL_Init(SDL_INIT_VIDEO), ==, 0);
	ui.window = SDL_CreateWindow("Fixture", 0, 0, W, H, 0);
	g_assert_nonnull(ui.window);
	g_assert_true(ui_draw_init());
	ui_motion_refresh_preferences();
	ui.connected = ui.active = TRUE;
	ui.chats = json_object_new_array();
	ui.history = json_object_new_array();
	ui.visible = g_array_new(FALSE, FALSE, sizeof(int));
	ui.sending = g_hash_table_new(g_str_hash, g_str_equal);
	ui.selected = g_strdup("contact");
	ui.name = g_strdup("Ada");
	ui.message.value = g_string_new("");
	ui.search.value = g_string_new("");
	for (int i = 0; i < 55; i++) {
		Json *row = json_object_new_object();
		char message[72];

		g_snprintf(message, sizeof(message),
			   "Message %d at a different scroll offset", i);
		json_object_object_add(row, "text",
				       json_object_new_string(message));
		json_object_object_add(row, "from",
				       json_object_new_string(i % 3 ? "Ada" :
								      "Grace"));
		json_object_object_add(
			row, "time",
			json_object_new_int64(1700000000000LL + i * 60000));
		json_object_array_add(ui.history, row);
	}
	ui_layout();
	ui.hover_x = ui.conversation.x + 73;
	ui.hover_y = ui.conversation.y + 89;
	for (guint i = 0; i < G_N_ELEMENTS(positions); i++) {
		ui.history_offset = positions[i];
		ui_draw();
		reference[i] = g_compute_checksum_for_data(
			G_CHECKSUM_SHA256, (const guint8 *)presented,
			sizeof(presented));
	}
	for (guint i = 0; i < G_N_ELEMENTS(positions); i++) {
		ui.history_offset = positions[i];
		if (i == 6) {
			unsigned int before = full;

			skip = TRUE;
			ui_draw_region(ui.conversation);
			ui_draw_region(ui.conversation);
			g_assert_cmpuint(full, ==, before + 1);
		} else
			ui_draw_region(ui.conversation);
		g_autofree char *actual = g_compute_checksum_for_data(
			G_CHECKSUM_SHA256, (const guint8 *)presented,
			sizeof(presented));
		g_assert_cmpstr(actual, ==, reference[i]);
		if (i == 1) {
			g_test_message(
				"3 px scroll uploaded %u of %d history texels",
				uploaded,
				ui.conversation.w * ui.conversation.h);
			g_assert_cmpuint(uploaded, >, 0);
			g_assert_cmpuint(uploaded, <,
					 ui.conversation.w * ui.conversation.h /
						 4);
		}
	}
	g_assert_cmpuint(sparse, >=, 8);
	for (guint i = 0; i < G_N_ELEMENTS(positions); i++)
		g_free(reference[i]);
	ui_draw_shutdown();
	ui_motion_shutdown();
	g_string_free(ui.message.value, TRUE);
	g_string_free(ui.search.value, TRUE);
	g_hash_table_unref(ui.sending);
	g_free(ui.name);
	g_free(ui.selected);
	g_array_unref(ui.visible);
	json_object_put(ui.chats);
	json_object_put(ui.history);
	SDL_DestroyWindow(ui.window);
	SDL_Quit();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ui-scroll/scene-matches-full-repaint", history);
	return g_test_run();
}
