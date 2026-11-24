// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera.h"
#include "media_io.h"
#include "ui_private.h"
#include "ui_gpu.h"
#include "ui_motion.h"
#include "ui_sounds.h"
#include <gmodule.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

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

const char *ui_gpu_device_name(void)
{
	return NULL;
}

uint32_t ui_gpu_device_vendor(void)
{
	return 0;
}

int ui_gpu_render_minor(void)
{
	return -1;
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

static void check_chat_event(void)
{
	Json *rows = json_tokener_parse(
		"[{\"id\":\"200\",\"senderId\":\"grace\",\"from\":\"Grace\","
		"\"time\":1700000000000,\"text\":\"Hello\"},"
		"{\"id\":\"201\",\"senderId\":\"grace\",\"from\":\"Grace\","
		"\"time\":1700000001000,\"type\":\"chat_event\",\"own\":true,"
		"\"readCount\":3,\"text\":\"Grace joined the group call\"},"
		"{\"id\":\"202\",\"senderId\":\"grace\",\"from\":\"Grace\","
		"\"time\":1700000002000,\"text\":\"Goodbye\"}]");
	Json *fixture = ui.history;
	Json *notice = json_object_array_get_idx(rows, 1);
	int offset = ui.history_offset;
	gboolean stick = ui.stick_bottom;

	ui.history = rows;
	ui.stick_bottom = FALSE;
	ui.history_offset = 0;
	ui_forget_history();
	ui_draw();
	Rect area = ui.conversation;
	int center = area.x + area.w / 2, first = -1, last = -1;
	gboolean rendered = FALSE, sender_after = FALSE;

	for (int y = area.y; y < area.y + area.h; y++) {
		if (ui_message_at(center, y) == 1) {
			if (first < 0)
				first = y;
			last = y;
			g_assert_null(ui_sender_at(area.x + 32, y, NULL));
			g_assert_null(ui_row_at(area.x + 8, y));
			if (ui_row_at(center, y) == notice)
				rendered = TRUE;
		}
		if (ui_message_at(center, y) == 2)
			sender_after |= ui_sender_at(area.x + 32, y, NULL) !=
					NULL;
	}
	g_assert_cmpint(first, >=, area.y);
	g_assert_cmpint(last - first + 1, <, 34);
	g_assert_true(rendered);
	g_assert_true(sender_after);

	/* A notice takes no space for an outgoing timestamp or receipt. */
	g_autofree uint32_t (*before)[W] =
		g_memdup2(presented, sizeof(presented));

	json_object_object_del(notice, "own");
	json_object_object_del(notice, "readCount");
	ui_forget_history();
	ui_draw();
	g_assert_cmpmem(before, sizeof(presented), presented,
			sizeof(presented));
	ui.history = fixture;
	ui.history_offset = offset;
	ui.stick_bottom = stick;
	ui_forget_history();
	json_object_put(rows);
	ui_draw();
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

static gboolean stage_has_color(guint shade)
{
	int top = (UI_HEADER_H + 1) * H / ui.height;
	int bottom = (UI_HEADER_H + 1 + ui.stage_h - 76) * H / ui.height;
	int left = ui.conversation.x * W / ui.width;

	for (int y = top; y < bottom; y++)
		for (int x = left; x < W; x++)
			if (presented[y][x] == shade)
				return TRUE;
	return FALSE;
}

static gboolean stage_has_avatar(const char *id)
{
	return stage_has_color(ui_avatar_shade(id));
}

static void group_preview(const char *chat, gint64 decoded,
			  enum line_preview_source source, gboolean screen)
{
	struct line_preview_frame preview = {
		.decoded_at = decoded,
		.width = 2,
		.height = 2,
		.source = source,
		.screen = screen,
	};
	const guint8 pixels[6] = { 80, 80, 200, 200, 128, 128 };
	struct line_camera_frame image;
	struct line_preview_frame received;
	int pair[2], fd;

	g_strlcpy(preview.chat, chat, sizeof(preview.chat));
	line_camera_i420_frame(&image, pixels, 2, 2);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
				   pair),
			==, 0);
	g_assert_true(line_media_send_frame(pair[0], &preview, &image));
	fd = line_media_recv_frame(pair[1], &received);
	g_assert_cmpint(fd, >=, 0);
	ui_media_preview_frame(&received, fd);
	close(fd);
	close(pair[1]);
	close(pair[0]);
}

static void group_picture(const char *chat, gint64 decoded, gboolean screen)
{
	group_preview(chat, decoded, LINE_PREVIEW_REMOTE, screen);
}

static void group_stage_picture(gboolean present)
{
	struct gpu_scene scene = { .width = W, .height = H };

	ui_draw();
	ui_media_gpu_video(&scene, 0);
	g_assert_cmpint(scene.video[0].plane[0] != NULL, ==, present);
}

/* Compare the caption below the video plane, where every backend displays it. */
static guint32 peer_caption_checksum(gboolean expect_badge)
{
	struct gpu_scene scene = { .width = W, .height = H };
	const struct gpu_video *view = &scene.video[0];
	int left = ui.conversation.x * W / ui.width;
	int bottom = (UI_HEADER_H + 1 + ui.stage_h - 76) * H / ui.height;
	guint32 sum = 0;
	gboolean badge = FALSE;

	ui_media_gpu_video(&scene, 0);
	g_assert_nonnull(view->plane[0]);
	int top = (int)(view->y + view->h) + 1;
	int right = (int)(view->x + view->w);

	g_assert_cmpint(top, <, bottom);
	g_assert_cmpint(left, <, right);
	for (int y = top; y < bottom; y++) {
		sum = crc32(sum, (const Bytef *)&presented[y][left],
			    (right - left) * sizeof(presented[0][0]));
		for (int x = left; x < W; x++)
			badge |= presented[y][x] == 0xda373c;
	}
	g_assert_cmpint(badge, ==, expect_badge);
	return sum;
}

static void check_group_presentation(const char *chat, Json *focus)
{
	g_autoptr(J) state = obj();
	struct gpu_scene scene = { .width = W, .height = H };
	gint64 frame_time = ++scene_time;

	json_object_object_add(state, "event",
			       json_object_new_string("call_share"));
	json_object_object_add(state, "chat", json_object_new_string(chat));
	json_object_object_add(state, "peerSharing",
			       json_object_new_boolean(TRUE));
	ui_media_call_event(state);

	/* Switching camera focus cannot invalidate an independent display. */
	json_object_object_add(focus, "changedAt",
			       json_object_new_int64(++scene_time));
	ui_media_call_event(focus);
	group_picture(chat, scene_time, FALSE);
	group_preview(chat, frame_time, LINE_PREVIEW_SCREEN, FALSE);
	ui_draw();
	ui_media_gpu_video(&scene, 0);
	g_assert_nonnull(scene.video[0].plane[0]);
	g_assert_nonnull(scene.video[2].plane[0]);
	g_assert_cmpfloat(scene.video[2].x + scene.video[2].w, <=,
			  scene.video[0].x);
	g_assert_true(stage_has_color(0xda373c));

	SDL_Event expand = { .key = { .type = SDL_KEYDOWN,
				      .keysym.sym = SDLK_F11 } };

	g_assert_true(ui_media_event(&expand));
	g_assert_true(ui_media_fullscreen());
	scene = (struct gpu_scene){ .width = W, .height = H };
	ui_draw();
	ui_media_gpu_video(&scene, 0);
	g_assert_null(scene.video[0].plane[0]);
	g_assert_nonnull(scene.video[2].plane[0]);

	/* STOP wins over a frame queued on the separate preview socket. */
	json_object_object_add(state, "peerSharing",
			       json_object_new_boolean(FALSE));
	json_object_object_add(state, "changedAt",
			       json_object_new_int64(++scene_time));
	ui_media_call_event(state);
	g_assert_false(ui_media_fullscreen());
	group_preview(chat, frame_time, LINE_PREVIEW_SCREEN, FALSE);
	scene = (struct gpu_scene){ .width = W, .height = H };
	ui_draw();
	ui_media_gpu_video(&scene, 0);
	g_assert_nonnull(scene.video[0].plane[0]);
	g_assert_null(scene.video[2].plane[0]);
	g_assert_false(stage_has_color(0xda373c));
}

static void check_group_call_roster(void)
{
	const guint live_badge = 0xda373c;
	static const char group[] = "c00000000000000000000000000000000";
	static const char ada[] = "u00000000000000000000000000000000";
	static const char grace[] = "u00000000000000000000000000000001";
	static const char edsger[] = "u00000000000000000000000000000002";
	Json *event = json_tokener_parse(
		"{\"event\":\"call\",\"state\":\"connected\","
		"\"name\":\"Test group\",\"participants\":["
		"{\"id\":\"u00000000000000000000000000000000\",\"name\":\"Ada\"},"
		"{\"id\":\"u00000000000000000000000000000001\",\"name\":\"Grace\"},"
		"{\"id\":\"u00000000000000000000000000000002\",\"name\":\"Edsger\"}]}");
	char *prior = ui.selected;
	Json *participants = ui_field(event, "participants");

	ui.selected = g_strdup(group);
	json_object_object_add(event, "chat", json_object_new_string(group));
	ui_media_call_event(event);
	ui_draw();
	settle();
	g_assert_cmpint(ui.stage_h, >, 100);
	g_assert_true(stage_has_avatar(ada));
	g_assert_true(stage_has_avatar(grace));
	g_assert_true(stage_has_avatar(edsger));

	/* A group voice call can receive a presentation before any camera. */
	group_preview(group, ++scene_time, LINE_PREVIEW_SCREEN, FALSE);
	ui_draw();
	settle();
	struct gpu_scene voice = { .width = W, .height = H };

	ui_media_gpu_video(&voice, 0);
	g_assert_null(voice.video[0].plane[0]);
	g_assert_nonnull(voice.video[2].plane[0]);
	g_autoptr(J) stop = json_tokener_parse(
		"{\"event\":\"call_share\",\"peerSharing\":false}");

	json_object_object_add(stop, "chat", json_object_new_string(group));
	ui_media_call_event(stop);

	/* Video keeps the full small roster beside the camera tiles. */
	json_object_object_add(event, "video", json_object_new_boolean(TRUE));
	json_object_object_add(event, "cameraEnabled",
			       json_object_new_boolean(TRUE));
	ui_media_call_event(event);
	ui_draw();
	settle();
	g_assert_true(stage_has_avatar(ada));
	g_assert_true(stage_has_avatar(grace));
	g_assert_true(stage_has_avatar(edsger));

	g_autoptr(J) focus = obj();
	json_object_object_add(focus, "event",
			       json_object_new_string("call_video_peer"));
	json_object_object_add(focus, "chat", json_object_new_string(group));
	json_object_object_add(focus, "id", json_object_new_string(ada));
	json_object_object_add(focus, "changedAt",
			       json_object_new_int64(scene_time));
	ui_media_call_event(focus);
	group_picture(group, scene_time, FALSE);
	group_stage_picture(TRUE);
	g_assert_false(stage_has_color(live_badge));
	guint32 caption = peer_caption_checksum(FALSE);

	group_picture(group, ++scene_time, TRUE);
	group_stage_picture(TRUE);
	g_assert_true(stage_has_color(live_badge));
	g_assert_cmpuint(peer_caption_checksum(TRUE), !=, caption);
	group_picture(group, ++scene_time, FALSE);
	group_stage_picture(TRUE);
	g_assert_false(stage_has_color(live_badge));
	g_assert_cmpuint(peer_caption_checksum(FALSE), ==, caption);

	/* A delayed frame reply must not revive the previous screen label. */
	group_picture(group, scene_time - 1, TRUE);
	group_stage_picture(TRUE);
	g_assert_false(stage_has_color(live_badge));
	group_picture(group, ++scene_time, TRUE);
	group_stage_picture(TRUE);
	g_assert_true(stage_has_color(live_badge));

	scene_time += 2000;
	json_object_object_add(focus, "id", json_object_new_string(grace));
	json_object_object_add(focus, "changedAt",
			       json_object_new_int64(scene_time));
	ui_media_call_event(focus);
	group_stage_picture(FALSE);
	g_assert_false(stage_has_color(live_badge));
	group_picture(group, scene_time - 1, TRUE);
	group_stage_picture(FALSE);
	g_assert_false(stage_has_color(live_badge));
	group_picture(group, scene_time, FALSE);
	group_stage_picture(TRUE);

	/* The next presenter’s frame can reach its socket before the focus event. */
	scene_time += 2000;
	group_picture(group, scene_time, TRUE);
	json_object_object_add(focus, "id", json_object_new_string(ada));
	json_object_object_add(focus, "changedAt",
			       json_object_new_int64(scene_time - 1));
	ui_media_call_event(focus);
	group_stage_picture(TRUE);
	g_assert_true(stage_has_color(live_badge));

	check_group_presentation(group, focus);
	json_object_object_add(event, "event",
			       json_object_new_string("call_participants"));
	json_object_object_del(event, "state");
	json_object_array_del_idx(participants, 2, 1);
	ui_media_call_event(event);
	ui_draw();
	g_assert_true(stage_has_avatar(ada));
	g_assert_true(stage_has_avatar(grace));
	g_assert_false(stage_has_avatar(edsger));

	json_object_object_add(event, "event", json_object_new_string("call"));
	json_object_object_add(event, "state", json_object_new_string("ended"));
	ui_media_call_event(event);
	g_clear_pointer(&ui.notice, g_free);
	g_free(ui.selected);
	ui.selected = prior;
	json_object_put(event);
	ui_draw();
	settle();
	g_assert_cmpint(ui.stage_h, ==, 0);
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
	SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
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
	check_chat_event();
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
	check_group_call_roster();
	g_assert_true(ui_theme_toggle(&error));
	g_assert_no_error(error);
	ui_theme_changed(row_height);
	check_message_receipts();
	check_chat_event();
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
	ui_sound_shutdown();
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
