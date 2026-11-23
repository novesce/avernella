// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "ui_gpu.h"
#include "ui_motion.h"
#include <gmodule.h>
#include <string.h>

enum {
	W = 910,
	H = 532
};
static uint32_t texture[H][W], presented[H][W];
static unsigned int sparse, full, uploaded;
static gboolean skip;
static gint64 scene_time = 1000000;
static struct gpu_rect dirty;

G_MODULE_EXPORT gint64 __wrap_g_get_monotonic_time(void)
{
	return scene_time;
}

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

	dirty = s->damage;
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
	/* The compositor samples RGB. Cairo leaves the high XRGB byte undefined. */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			presented[y][x] &= 0x00ffffff;
	return 1;
}

static void report_difference(int offset)
{
	g_autofree uint32_t (*actual)[W] =
		g_memdup2(presented, sizeof(presented));

	ui_draw();
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++)
			if (actual[y][x] != presented[y][x]) {
				g_test_message(
					"offset %d, pixel (%d,%d): %06x, full repaint %06x",
					offset, x, y, actual[y][x],
					presented[y][x]);
				return;
			}
	}
	g_test_message("offset %d also changed between full repaints", offset);
}

static void check_chat_bubbles(void)
{
	Json *received = json_tokener_parse(
		"{\"id\":\"100\",\"from\":\"Grace\",\"text\":\"Hi!\"}");
	Json *sent = json_tokener_parse(
		"{\"id\":\"101\",\"own\":true,\"senderId\":\"me\","
		"\"time\":1700000000000,\"text\":\"Hi Grace\"}");
	Json *card = json_tokener_parse(
		"{\"id\":\"102\",\"own\":true,\"senderId\":\"me\",\"time\":1700000001000,"
		"\"flex\":{\"type\":\"bubble\",\"size\":\"micro\","
		"\"body\":{\"type\":\"box\",\"layout\":\"vertical\",\"contents\":["
		"{\"type\":\"button\",\"action\":{\"type\":\"uri\",\"label\":\"Open\","
		"\"uri\":\"https://example.org/chat\"}}]}}}");
	gboolean left = FALSE, right = FALSE, link = FALSE;

	json_object_array_add(ui.history, received);
	json_object_array_add(ui.history, sent);
	json_object_array_add(ui.history, card);
	ui_layout();
	ui_draw();
	Rect area = ui.conversation;

	for (int y = area.y; y < area.y + area.h; y += 2) {
		for (int x = area.x; x < area.x + area.w; x += 2) {
			Json *hit = ui_row_at(x, y);

			if (hit == received) {
				g_assert_cmpint(x, <, area.x + area.w / 2);
				left |= presented[y][x] == UI_BUBBLE_IN;
			} else if (hit == sent) {
				g_assert_cmpint(x, >, area.x + area.w / 2);
				right |= presented[y][x] == UI_BUBBLE_OUT;
			}
			if (ui_flex_link_at(x, y)) {
				g_assert_true(hit == card);
				g_assert_cmpint(x, >, area.x + area.w / 2);
				link = TRUE;
			}
		}
	}
	g_assert_true(left);
	g_assert_true(right);
	g_assert_true(link);
}

static void check_message_receipts(void)
{
	g_autofree uint32_t (*before)[W] = g_malloc(sizeof(presented));
	Json *packet = json_tokener_parse(
		"{\"event\":\"message_reads\",\"chat\":\"other\","
		"\"counts\":{\"100\":1,\"101\":1,\"102\":1}}");
	gboolean changed[3] = { FALSE };
	/* Begin with no confirmed receipts in this fixture. */
	for (int i = 0; i < 3; i++)
		json_object_object_del(json_object_array_get_idx(ui.history, i),
				       "readCount");
	ui.stick_bottom = TRUE;
	ui_draw();
	scene_time += 300000;
	ui_draw();
	memcpy(before, presented, sizeof(presented));
	ui_message_reads(packet);
	ui_draw();
	g_assert_cmpmem(before, sizeof(presented), presented,
			sizeof(presented));

	json_object_object_add(packet, "chat",
			       json_object_new_string(ui.selected));
	ui_message_reads(packet);
	ui_draw();
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (presented[y][x] == before[y][x])
				continue;
			int lx = x * ui.width / W, ly = y * ui.height / H;
			int row = ui_message_at(lx, ly);

			g_assert_cmpint(row, >=, 0);
			g_assert_cmpint(row, <, 3);
			if (ui_theme_is_line())
				g_assert_null(ui_row_at(lx, ly));
			changed[row] = TRUE;
		}
	}
	g_assert_false(changed[0]);
	g_assert_true(changed[1]);
	g_assert_true(changed[2]);
	/* A stale count or a duplicate receipt must not undo a confirmed read. */
	json_object_object_add(packet, "counts",
			       json_tokener_parse("{\"101\":0,\"102\":1}"));
	ui.dirty = FALSE;
	ui_message_reads(packet);
	g_assert_false(ui.dirty);
	g_assert_true(
		ui_message_is_read(json_object_array_get_idx(ui.history, 1)));
	g_assert_true(
		ui_message_is_read(json_object_array_get_idx(ui.history, 2)));
	json_object_put(packet);
}

static void check_scroll_positions(void)
{
	int positions[] = { 0,	 3,   9,   76,	285, 304, 327,
			    448, 512, 405, 377, 305, 11 };
	char *reference[G_N_ELEMENTS(positions)];
	unsigned int sparse_before = sparse;

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
		if (strcmp(actual, reference[i]))
			report_difference(ui.history_offset);
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
	g_assert_cmpuint(sparse - sparse_before, >=, 8);
	for (guint i = 0; i < G_N_ELEMENTS(positions); i++)
		g_free(reference[i]);
}

static void check_row_animation(void)
{
	g_autofree guint8 *saved = g_malloc(sizeof(presented));

	ui.hover_x = ui.list_box.x + 24;
	/* LINE's taller row must also highlight at its lower edge. */
	ui.hover_y = ui.list_box.y +
		     (ui_theme_is_line() ? ui_chat_row_height() - 3 : 20);
	ui_draw();
	/* Settle pre-existing controls, then exit the hovered chat row. */
	scene_time += 300000;
	ui_draw();
	ui.hover_x = ui.hover_y = -1;
	ui_draw();
	g_assert_cmpint(ui_animation_deadline(scene_time), >, scene_time);
	for (int i = 0; i < 8; i++) {
		scene_time += 16667;
		ui_draw_animation();
		g_assert_cmpint(dirty.w, >, 0);
		g_assert_cmpint(dirty.w, <=, ui.list_box.w + 4);
		g_assert_cmpint(dirty.h, <=, ui_chat_row_height() + 4);
		memcpy(saved, presented, sizeof(presented));
		ui_draw();
		g_assert_cmpmem(saved, sizeof(presented), presented,
				sizeof(presented));
		if (!ui_animation_deadline(scene_time))
			break;
	}
	g_assert_cmpint(ui_animation_deadline(scene_time), ==, 0);
}

static Json *glide_row(int i)
{
	Json *row = json_object_new_object();
	char id[16], text[64];

	g_snprintf(id, sizeof(id), "g%d", i);
	g_snprintf(text, sizeof(text), "Glide message %d", i);
	json_object_object_add(row, "id", json_object_new_string(id));
	json_object_object_add(row, "text", json_object_new_string(text));
	json_object_object_add(row, "own", json_object_new_boolean(i % 2));
	json_object_object_add(row, "from", json_object_new_string("Grace"));
	json_object_object_add(
		row, "time",
		json_object_new_int64(1700000000000LL + i * 60000));
	return row;
}

static void settle(void)
{
	for (int i = 0; i < 60 && ui_animation_deadline(scene_time); i++) {
		scene_time += 16667;
		ui_draw_animation();
	}
	g_assert_cmpint(ui_animation_deadline(scene_time), ==, 0);
}

/* New rows glide up from the composer; an opened chat fades in. */
static void check_history_motion(void)
{
	g_autofree guint8 *saved = g_malloc(sizeof(presented));
	Json *fixture = ui.history;
	Json *rows = json_object_new_array();

	for (int i = 0; i < 30; i++)
		json_object_array_add(rows, glide_row(i));
	ui.hover_x = ui.hover_y = -1;
	ui.history = rows;
	ui.stick_bottom = TRUE;
	ui_forget_history();
	ui_draw();
	scene_time += 300000;
	ui_draw();
	settle();
	int before = ui.history_offset;

	g_assert_cmpint(before, ==, ui.max_history_offset);
	g_assert_cmpint(before, >, 0);
	json_object_array_add(rows, glide_row(30));
	ui_forget_history();
	ui_draw();
	g_assert_cmpint(ui.max_history_offset, >, before);
	g_assert_cmpint(ui.history_offset, ==, before);
	g_assert_cmpint(ui_animation_deadline(scene_time), >, scene_time);
	g_assert_cmpint(ui_animation_deadline(scene_time), <=,
			scene_time + 250000);
	int previous = before, frames = 0;

	while (ui_animation_deadline(scene_time)) {
		scene_time += 16667;
		ui_draw_animation();
		g_assert_cmpint(ui.history_offset, >=, previous);
		/* Only the conversation repaints, never the whole window. */
		g_assert_cmpint(dirty.y, >, 0);
		g_assert_cmpint(dirty.h, <, H);
		previous = ui.history_offset;
		g_assert_cmpint(++frames, <, 20);
	}
	g_assert_cmpint(frames, >, 3);
	g_assert_cmpint(ui.history_offset, ==, ui.max_history_offset);
	memcpy(saved, presented, sizeof(presented));
	ui_draw();
	g_assert_cmpmem(saved, sizeof(presented), presented, sizeof(presented));
	/* A scrolled-away reader keeps their place: nothing glides. */
	ui.stick_bottom = FALSE;
	ui.history_offset = 40;
	ui_draw();
	json_object_array_add(rows, glide_row(31));
	ui_forget_history();
	ui_draw();
	g_assert_cmpint(ui.history_offset, ==, 40);
	g_assert_cmpint(ui_animation_deadline(scene_time), ==, 0);

	/* Opening a chat: rows fade in once, when the first page arrives. */
	ui.stick_bottom = TRUE;
	ui.history = NULL;
	ui_forget_history();
	ui_draw();
	ui.history = rows;
	ui_draw();
	g_assert_cmpint(ui_animation_deadline(scene_time), >, scene_time);
	settle();
	memcpy(saved, presented, sizeof(presented));
	ui_draw();
	g_assert_cmpmem(saved, sizeof(presented), presented, sizeof(presented));
	ui_draw();
	g_assert_cmpint(ui_animation_deadline(scene_time), ==, 0);

	/* Reduced motion: the same end states with no frames scheduled. */
	g_setenv("LINE_REDUCE_MOTION", "1", TRUE);
	ui_motion_refresh_preferences();
	json_object_array_add(rows, glide_row(32));
	ui_forget_history();
	ui_draw();
	g_assert_cmpint(ui.history_offset, ==, ui.max_history_offset);
	g_assert_cmpint(ui_animation_deadline(scene_time), ==, 0);
	ui.history = NULL;
	ui_draw();
	ui.history = rows;
	ui_draw();
	g_assert_cmpint(ui_animation_deadline(scene_time), ==, 0);
	g_setenv("LINE_REDUCE_MOTION", "0", TRUE);
	ui_motion_refresh_preferences();
	ui.history = fixture;
	ui_forget_history();
	json_object_put(rows);
	ui_draw();
	settle();
}

static void history(void)
{
	g_autoptr(GError) error = NULL;

	ui_theme_init();
	g_assert_true(ui_theme_is_line());
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
		json_object_object_add(row, "own",
				       json_object_new_boolean(i % 3 != 0));
		json_object_object_add(row, "from",
				       json_object_new_string(i % 3 ? "Ada" :
								      "Grace"));
		json_object_object_add(
			row, "time",
			json_object_new_int64(1700000000000LL + i * 60000));
		json_object_array_add(ui.history, row);
	}
	/* Check both layouts, ending on LINE for the bubble checks. */
	check_scroll_positions();
	for (int i = 0; i < 2; i++) {
		int row_height = ui_chat_row_height();

		g_assert_true(ui_theme_toggle(&error));
		g_assert_no_error(error);
		g_assert_true(ui_theme_is_line() == (i == 1));
		ui_theme_changed(row_height);
		check_scroll_positions();
	}
	ui_forget_history();
	json_object_array_del_idx(ui.history, 0,
				  json_object_array_length(ui.history));
	ui.history_offset = 0;
	check_chat_bubbles();
	check_message_receipts();
	int index = 0, row_height = ui_chat_row_height();

	json_object_array_add(
		ui.chats,
		json_tokener_parse(
			"{\"id\":\"another\",\"name\":\"Fixture friend\"}"));
	g_array_append_val(ui.visible, index);
	g_setenv("LINE_REDUCE_MOTION", "0", TRUE);
	ui_motion_refresh_preferences();
	check_row_animation();
	check_history_motion();
	g_assert_true(ui_theme_toggle(&error));
	g_assert_no_error(error);
	ui_theme_changed(row_height);
	check_message_receipts();
	check_row_animation();
	check_history_motion();
	/* Exercise receipt widths with RTL shaping and a compact chat pane. */
	g_assert_true(ui_lang_set(UI_LANG_AR, &error));
	g_assert_no_error(error);
	ui_language_changed();
	SDL_SetWindowSize(ui.window, W / 2, H);
	ui.focus = F_MESSAGE;
	row_height = ui_chat_row_height();
	g_assert_true(ui_theme_toggle(&error));
	g_assert_no_error(error);
	ui_theme_changed(row_height);
	check_message_receipts();
	check_history_motion();
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
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui-scroll/scene-matches-full-repaint", history);
	return g_test_run();
}
