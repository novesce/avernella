// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_gpu.h"
#include "ui_motion.h"
#include "ui_private.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static char *picture_id;
static guint picture_tag;

void __wrap_ui_rpc(const char *method, Json *params, guint tag)
{
	if (!strcmp(method, "photo") && tag) {
		g_free(picture_id);
		picture_id = g_strdup(ui_string(params, "id", NULL));
		picture_tag = tag;
	}
	json_object_put(params);
}

static Json *message(const char *id, const char *type)
{
	Json *row = json_object_new_object();

	json_object_object_add(row, "id", json_object_new_string(id));
	json_object_object_add(row, "type", json_object_new_string(type));
	json_object_object_add(row, "from", json_object_new_string("Peer"));
	json_object_object_add(row, "text", json_object_new_string(""));
	return row;
}

static void key(SDL_Keycode code)
{
	SDL_Event event = { .key = { .type = SDL_KEYDOWN,
				     .keysym.sym = code } };

	g_assert_true(ui_media_event(&event));
}

static void button(Uint32 type, int x, int y)
{
	SDL_Event event = { .button = { .type = type,
					.button = SDL_BUTTON_LEFT,
					.x = x,
					.y = y,
					.clicks = 1 } };

	g_assert_true(ui_media_event(&event));
}

static guint8 green(SDL_Surface *surface, int x, int y)
{
	guint8 r, g, b;
	guint32 pixel;
	const guint8 *src = (guint8 *)surface->pixels + y * surface->pitch +
			    x * surface->format->BytesPerPixel;

	g_assert_cmpint(surface->format->BytesPerPixel, ==, sizeof(pixel));
	memcpy(&pixel, src, sizeof(pixel));
	SDL_GetRGB(pixel, surface->format, &r, &g, &b);
	return g;
}

static cairo_status_t write_png(void *data, const guint8 *bytes,
				unsigned int size)
{
	int fd = *(int *)data;

	return write(fd, bytes, size) == size ? CAIRO_STATUS_SUCCESS :
						CAIRO_STATUS_WRITE_ERROR;
}

static void pass_picture(void)
{
	int fd = memfd_create("image-fixture", MFD_CLOEXEC);
	cairo_surface_t *image =
		cairo_image_surface_create(CAIRO_FORMAT_RGB24, 320, 160);
	cairo_t *cr = cairo_create(image);

	g_assert_cmpint(fd, >=, 0);
	cairo_set_source_rgb(cr, 1, 0, 0);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 1, 0);
	cairo_rectangle(cr, 0, 0, 160, 160);
	cairo_fill(cr);
	g_assert_cmpint(cairo_surface_write_to_png_stream(image, write_png,
							  &fd),
			==, CAIRO_STATUS_SUCCESS);
	cairo_destroy(cr);
	cairo_surface_destroy(image);
	Json *params = message(picture_id, "photo");
	Json *result = json_object_new_object();

	json_object_object_add(params, "chat",
			       json_object_new_string(ui.selected));
	json_object_object_add(result, "fd", json_object_new_int(fd));
	ui_media_reply("photo", params, picture_tag, result, NULL);
	json_object_put(result);
	json_object_put(params);
}

static void open_first_photo(void)
{
	for (int y = ui.conversation.y; y < ui.composer.y; y += 4) {
		for (int x = ui.conversation.x; x < ui.width; x += 4) {
			Json *row = ui_row_at(x, y);

			if (!strcmp(ui_string(row, "id", ""), "1")) {
				button(SDL_MOUSEBUTTONDOWN, x, y);
				return;
			}
		}
	}
	g_assert_not_reached();
}

static SDL_Surface *canvas(void)
{
	return ui_gpu_active() ? ui.buffer : SDL_GetWindowSurface(ui.window);
}

static void viewer(void)
{
	const char *driver = g_getenv("AVERNELLA_TEST_VIEWER_VULKAN");

	SDL_setenv("SDL_VIDEODRIVER", driver ? driver : "dummy", 1);
	g_setenv("LINE_DISABLE_GPU", driver ? "0" : "1", TRUE);
	g_setenv("LINE_REDUCE_MOTION", "1", TRUE);
	g_assert_cmpint(SDL_Init(SDL_INIT_VIDEO), ==, 0);
	ui.window = ui_gpu_window("Offline viewer fixture", 0, 0, 800, 500,
				  SDL_WINDOW_SHOWN);
	g_assert_nonnull(ui.window);
	g_assert_true(ui_draw_init());
	if (driver)
		g_assert_true(ui_gpu_active());
	ui_motion_refresh_preferences();
	ui.connected = ui.active = ui.photos = ui.files = TRUE;
	ui.chats = json_object_new_array();
	ui.history = json_object_new_array();
	ui.visible = g_array_new(FALSE, FALSE, sizeof(guint));
	ui.sending = g_hash_table_new(g_str_hash, g_str_equal);
	ui.selected = g_strdup("u11111111111111111111111111111111");
	ui.name = g_strdup("Self");
	ui.message.value = g_string_new("");
	ui.search.value = g_string_new("");
	json_object_array_add(ui.history, message("1", "photo"));
	json_object_array_add(ui.history, message("2", "text"));
	json_object_array_add(ui.history, message("3", "photo"));
	ui_draw();
	open_first_photo();
	g_assert_true(ui_media_image_viewer());
	g_assert_cmpstr(picture_id, ==, "1");

	pass_picture();
	const gint64 until = g_get_monotonic_time() + 2000 * 1000;

	do {
		SDL_Event event;

		if (!SDL_WaitEventTimeout(&event, 100))
			continue;
		ui_media_event(&event);
		ui_draw();
	} while (green(canvas(), 200, 220) < 210 &&
		 g_get_monotonic_time() < until);
	SDL_Surface *surface = canvas();

	/* Clockwise rotation moves the clear green half from left to top. */
	g_assert_cmpuint(green(surface, 200, 220), >, 210);
	g_assert_cmpuint(green(surface, 600, 220), <, 45);
	key(SDLK_r);
	ui_draw();
	g_assert_cmpuint(green(surface, 400, 150), >, 210);
	g_assert_cmpuint(green(surface, 400, 330), <, 45);

	key(SDLK_RIGHT);
	g_assert_cmpstr(picture_id, ==, "3");
	key(SDLK_LEFT);
	g_assert_cmpstr(picture_id, ==, "1");
	ui_draw();
	button(SDL_MOUSEBUTTONDOWN, 0, 0);
	button(SDL_MOUSEBUTTONUP, 0, 0);
	g_assert_false(ui_media_image_viewer());
	open_first_photo();
	key(SDLK_ESCAPE);
	g_assert_false(ui_media_image_viewer());

	ui_media_shutdown();
	ui_draw_shutdown();
	ui_motion_shutdown();
	g_free(ui.selected);
	g_free(ui.name);
	g_free(ui.notice);
	g_free(picture_id);
	g_string_free(ui.message.value, TRUE);
	g_string_free(ui.search.value, TRUE);
	g_hash_table_unref(ui.sending);
	g_array_unref(ui.visible);
	json_object_put(ui.chats);
	json_object_put(ui.history);
	SDL_DestroyWindow(ui.window);
	SDL_Quit();
}

static void sticker_sound_policy(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed();
		return;
	}
	static const char player_script[] =
		"#!/bin/sh\n"
		"[ -r /proc/self/fd/3 ] || exit 1\n"
		"printf '%s\\n' \"$@\" > \"$AVERNELLA_TEST_PLAYER_LOG\"\n"
		": > \"$AVERNELLA_TEST_PLAYER_LOG.done\"\n";
	g_autofree char *dir = g_dir_make_tmp("avernella-player-XXXXXX", NULL);

	g_assert_nonnull(dir);
	g_autofree char *player = g_build_filename(dir, "ffplay", NULL);
	g_autofree char *log = g_build_filename(dir, "args", NULL);
	g_autofree char *done = g_strconcat(log, ".done", NULL);
	g_autofree char *args = NULL;
	int fd = memfd_create("audio-fixture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	Json *lease = json_object_new_object();

	g_assert_true(g_file_set_contents(player, player_script, -1, NULL));
	g_assert_cmpint(g_chmod(player, 0700), ==, 0);
	g_assert_true(g_setenv("PATH", dir, TRUE));
	g_assert_true(g_setenv("AVERNELLA_TEST_PLAYER_LOG", log, TRUE));
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(write(fd, "fixture", 7), ==, 7);
	g_assert_cmpint(fcntl(fd, F_ADD_SEALS,
			      F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK),
			==, 0);
	json_object_object_add(lease, "fd", json_object_new_int(fd));
	ui_media_reply("stickerSound", NULL, 0, lease, NULL);
	json_object_put(lease);
	while (!g_file_test(done, G_FILE_TEST_IS_REGULAR))
		g_usleep(1000);
	g_assert_true(g_file_get_contents(log, &args, NULL, NULL));
	g_assert_nonnull(strstr(args, "-protocol_whitelist\nfile\n"));
	g_assert_nonnull(strstr(args, "-format_whitelist\nmov\n-f\nmov\n"));
	g_assert_nonnull(
		strstr(args, "-enable_drefs\n0\n-use_absolute_path\n0\n"));
	g_assert_nonnull(strstr(args, "-i\n/proc/self/fd/3\n"));
	ui_media_shutdown();
	g_assert_cmpint(g_remove(done), ==, 0);
	g_assert_cmpint(g_remove(log), ==, 0);
	g_assert_cmpint(g_remove(player), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/image-overlay/interaction", viewer);
	g_test_add_func("/image-overlay/sticker-sound-policy",
			sticker_sound_policy);
	return g_test_run();
}
