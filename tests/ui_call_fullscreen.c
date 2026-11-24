// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_io.h"
#include "ui_gpu.h"
#include "ui_motion.h"
#include "ui_private.h"
#include "ui_sounds.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
	WIDTH = 64,
	HEIGHT = 32
};

static SDL_Surface *capture;
static gboolean capture_next;
void __real_SDL_RenderPresent(SDL_Renderer *renderer);

void __wrap_SDL_RenderPresent(SDL_Renderer *renderer)
{
	if (capture_next) {
		int w, h;

		g_assert_cmpint(SDL_GetRendererOutputSize(renderer, &w, &h), ==,
				0);
		capture = SDL_CreateRGBSurfaceWithFormat(
			0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		g_assert_nonnull(capture);
		g_assert_cmpint(SDL_RenderReadPixels(renderer, NULL,
						     SDL_PIXELFORMAT_ARGB8888,
						     capture->pixels,
						     capture->pitch),
				==, 0);
		capture_next = FALSE;
	}
	__real_SDL_RenderPresent(renderer);
}

static void call_state(const char *chat, const char *state, gboolean video)
{
	Json *p = json_object_new_object();

	json_object_object_add(p, "chat", json_object_new_string(chat));
	json_object_object_add(p, "state", json_object_new_string(state));
	json_object_object_add(p, "video", json_object_new_boolean(video));
	json_object_object_add(p, "cameraEnabled",
			       json_object_new_boolean(video));
	ui_media_call_event(p);
	json_object_put(p);
}

static void feed_picture(enum line_preview_source source)
{
	uint8_t image[HEIGHT * WIDTH * 3 / 2];
	struct line_preview_frame frame = {
		.decoded_at = g_get_monotonic_time(),
		.width = WIDTH,
		.height = HEIGHT,
		.source = source,
		.rotation = 90,
		.camera_enabled = TRUE,
		.chat = "friend",
	};
	int fd = memfd_create("fixture", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	g_assert_cmpint(fd, >=, 0);
	memset(image, 128, sizeof(image));
	for (int y = 0; y < HEIGHT; y++) {
		memset(image + WIDTH * y, 65, WIDTH / 2);
		memset(image + WIDTH * y + WIDTH / 2, 211, WIDTH / 2);
	}
	g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
	g_assert_cmpint(fcntl(fd, F_ADD_SEALS,
			      F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE |
				      F_SEAL_SEAL),
			==, 0);
	ui_media_preview_frame(&frame, fd);
	close(fd);
}

static void key(SDL_Keycode keycode)
{
	SDL_Event event = { .key = { .type = SDL_KEYDOWN,
				     .keysym.sym = keycode } };
	ui_media_event(&event);
	SDL_PumpEvents();
}

static void click(int x, int y, Uint8 clicks)
{
	SDL_Event e = { .button = { .type = SDL_MOUSEBUTTONDOWN,
				    .button = SDL_BUTTON_LEFT,
				    .clicks = clicks,
				    .x = x,
				    .y = y } };
	ui_media_event(&e);
	SDL_PumpEvents();
}

static Uint8 brightness(SDL_Surface *surface, int x, int y)
{
	Uint8 r, g, b;
	uint32_t pixel;
	const guint8 *src = (guint8 *)surface->pixels + y * surface->pitch +
			    x * surface->format->BytesPerPixel;

	g_assert_cmpint(surface->format->BytesPerPixel, ==, sizeof(pixel));
	memcpy(&pixel, src, sizeof(pixel));
	SDL_GetRGB(pixel, surface->format, &r, &g, &b);
	return g;
}

static void check_peer(void)
{
	int height = ui.height - 40;
	int w = MIN(ui.width, height / 2);
	int h = w * 2;
	int left = (ui.width - w) / 2;
	int top = 40 + (height - h) / 2;

	if (ui_gpu_active() || ui.renderer) {
		struct gpu_scene scene = { .width = ui.width,
					   .height = ui.height };

		ui_media_gpu_video(&scene, 0);
		g_assert_nonnull(scene.video[0].plane[0]);
		g_assert_null(scene.video[1].plane[0]);
		g_assert_null(scene.video[2].plane[0]);
		g_assert_cmpuint(scene.video[0].rotation, ==, 90);
		g_assert_cmpfloat_with_epsilon(scene.video[0].w, w, 1.0);
		g_assert_cmpfloat_with_epsilon(scene.video[0].h, h, 1.0);
		g_assert_cmpfloat_with_epsilon(scene.video[0].x, left, 1.0);
		g_assert_cmpfloat_with_epsilon(scene.video[0].y, top, 1.0);
	}
	if (!ui_gpu_active()) {
		SDL_Surface *surface =
			ui.renderer ? capture : SDL_GetWindowSurface(ui.window);
		g_assert_nonnull(surface);
		/* Clockwise rotation puts the dark left half above the light half. */
		g_assert_cmpint(brightness(surface, ui.width / 2, top + h / 4),
				>, 42);
		g_assert_cmpint(brightness(surface, ui.width / 2, top + h / 4),
				<, 85);
		g_assert_cmpint(brightness(surface, ui.width / 2,
					   top + h * 3 / 4),
				>, 200);
		if (left > 0) {
			g_assert_cmpint(brightness(surface, left / 2,
						   ui.height / 2),
					<, 40);
			g_assert_cmpint(brightness(surface,
						   ui.width - left / 2 - 1,
						   ui.height / 2),
					<, 40);
		}
	}
}

static void peer_fullscreen(void)
{
	const char *driver = g_getenv("LINE_FULLSCREEN_TEST_DRIVER");
	const char *backend = g_getenv("LINE_FULLSCREEN_TEST_RENDERER");
	SDL_setenv("SDL_VIDEODRIVER", driver ? driver : "dummy", 1);
	SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
	g_setenv("LINE_REDUCE_MOTION", "1", TRUE);
	if (!backend)
		g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	else if (!strcmp(backend, "sdl"))
		g_setenv("AVERNELLA_VULKAN", "sdl", TRUE);
	g_assert_cmpint(SDL_Init(SDL_INIT_VIDEO), ==, 0);
	ui.window = ui_gpu_window("Offline video fixture", 0, 0, 800, 500,
				  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	g_assert_nonnull(ui.window);
	g_assert_true(ui_draw_init());
	if (!g_strcmp0(backend, "native"))
		g_assert_true(ui_gpu_active());
	if (!g_strcmp0(backend, "sdl"))
		g_assert_nonnull(ui.renderer);
	ui_motion_refresh_preferences();
	ui.connected = ui.active = TRUE;
	ui.chats = json_object_new_array();
	ui.history = json_object_new_array();
	ui.visible = g_array_new(FALSE, FALSE, sizeof(guint));
	ui.sending = g_hash_table_new(g_str_hash, g_str_equal);
	ui.selected = g_strdup("friend");
	ui.name = g_strdup("Self");
	ui.message.value = g_string_new("");
	ui.search.value = g_string_new("");
	call_state("friend", "connected", TRUE);
	feed_picture(LINE_PREVIEW_REMOTE);
	feed_picture(LINE_PREVIEW_SELF);
	ui_draw();
	key(SDLK_F11);
	g_assert_true(ui_media_fullscreen());
	g_assert_cmphex(SDL_GetWindowFlags(ui.window) &
				SDL_WINDOW_FULLSCREEN_DESKTOP,
			==, SDL_WINDOW_FULLSCREEN_DESKTOP);
	capture_next = ui.renderer != NULL;
	ui_draw();
	check_peer();
	g_clear_pointer(&capture, SDL_FreeSurface);
	ui.dirty = ui.video_dirty = FALSE;
	feed_picture(LINE_PREVIEW_SELF);
	g_assert_false(ui.dirty || ui.video_dirty);
	feed_picture(LINE_PREVIEW_REMOTE);
	g_assert_true(ui.dirty || ui.video_dirty);
	g_assert_true(ui_media_fullscreen());
	key(SDLK_ESCAPE);
	g_assert_false(ui_media_fullscreen());
	g_assert_false(SDL_GetWindowFlags(ui.window) & SDL_WINDOW_FULLSCREEN);
	int width, height;
	SDL_GetWindowSize(ui.window, &width, &height);
	g_assert_cmpint(width, ==, 800);
	g_assert_cmpint(height, ==, 500);
	g_assert_cmpint(ui_media_stage_height(), >, 0);

	/* A window already expanded by the user keeps that state on exit. */
	g_assert_cmpint(SDL_SetWindowFullscreen(ui.window,
						SDL_WINDOW_FULLSCREEN_DESKTOP),
			==, 0);
	SDL_PumpEvents();
	ui_draw();
	if (ui_gpu_active() || ui.renderer) {
		struct gpu_scene stage = { .width = ui.width,
					   .height = ui.height };
		ui_media_gpu_video(&stage, 0);
		struct gpu_video *peer = &stage.video[0];
		g_assert_nonnull(peer->plane[0]);
		int x = peer->x + peer->w / 2, y = peer->y + peer->h / 2;
		click(x, y, 1);
		ui_draw();
		click(x, y, 2);
	} else {
		key(SDLK_F11);
	}
	g_assert_true(ui_media_fullscreen());
	ui_draw();
	click(ui.width - 24, 20, 1);
	g_assert_false(ui_media_fullscreen());
	g_assert_cmphex(SDL_GetWindowFlags(ui.window) &
				SDL_WINDOW_FULLSCREEN_DESKTOP,
			==, SDL_WINDOW_FULLSCREEN_DESKTOP);
	g_assert_cmpint(SDL_SetWindowFullscreen(ui.window, 0), ==, 0);
	SDL_PumpEvents();

	key(SDLK_F11);
	g_assert_true(ui_media_fullscreen());
	call_state("friend", "ended", TRUE);
	g_assert_false(ui_media_fullscreen());
	g_assert_false(SDL_GetWindowFlags(ui.window) & SDL_WINDOW_FULLSCREEN);
	ui_media_shutdown();
	ui_draw_shutdown();
	ui_motion_shutdown();
	ui_sound_shutdown();
	g_free(ui.selected);
	g_free(ui.name);
	g_free(ui.notice);
	g_string_free(ui.message.value, TRUE);
	g_string_free(ui.search.value, TRUE);
	g_hash_table_unref(ui.sending);
	g_array_unref(ui.visible);
	json_object_put(ui.chats);
	json_object_put(ui.history);
	SDL_DestroyWindow(ui.window);
	SDL_Quit();
}

static Json *share_request;
static guint share_tag;

void __real_ui_rpc(const char *method, Json *params, guint tag);

void __wrap_ui_rpc(const char *method, Json *params, guint tag)
{
	if (strcmp(method, "callShare")) {
		__real_ui_rpc(method, params, tag);
		return;
	}
	g_assert_null(share_request);
	share_request = params;
	share_tag = tag;
}

static void check_share_request(const char *chat, gboolean enable)
{
	g_autoptr(J) reply = obj();

	g_assert_nonnull(share_request);
	g_assert_cmpstr(ui_string(share_request, "chat", ""), ==, chat);
	g_assert_nonnull(ui_field(share_request, "enabled"));
	g_assert_cmpint(
		json_object_get_boolean(ui_field(share_request, "enabled")), ==,
		enable);
	json_object_object_add(reply, "pending", json_object_new_boolean(TRUE));
	json_object_object_add(reply, "selfSharing",
			       json_object_new_boolean(!enable));
	ui_media_reply("callShare", share_request, share_tag, reply, NULL);
	g_clear_pointer(&share_request, json_object_put);
}

static void group_share_controls(void)
{
	static const char chat[] = "c00000000000000000000000000000000";
	SDL_Event key = { .key = { .type = SDL_KEYDOWN,
				   .keysym = { .sym = SDLK_s,
					       .mod = KMOD_ALT } } };
	g_autoptr(J) share = obj();

	ui.connected = TRUE;
	call_state(chat, "connecting", TRUE);
	ui_media_event(&key);
	g_assert_null(share_request);
	call_state(chat, "connected", FALSE);
	ui_media_event(&key);
	g_assert_null(share_request);
	call_state(chat, "connected", TRUE);
	g_assert_true(ui_media_event(&key));
	check_share_request(chat, TRUE);
	ui_media_event(&key);
	g_assert_null(share_request);

	json_object_object_add(share, "event",
			       json_object_new_string("call_share"));
	json_object_object_add(share, "chat", json_object_new_string(chat));
	json_object_object_add(share, "shareInVideo",
			       json_object_new_boolean(TRUE));
	json_object_object_add(share, "selfSharing",
			       json_object_new_boolean(TRUE));
	ui_media_call_event(share);
	g_assert_true(ui_media_event(&key));
	check_share_request(chat, FALSE);
	json_object_object_add(share, "selfSharing",
			       json_object_new_boolean(FALSE));
	json_object_object_add(share, "shareInVideo",
			       json_object_new_boolean(FALSE));
	ui_media_call_event(share);
	g_assert_true(ui_media_event(&key));
	check_share_request(chat, TRUE);

	call_state(chat, "ended", FALSE);
	ui_media_event(&key);
	g_assert_null(share_request);
	ui_media_shutdown();
	ui.connected = FALSE;
	g_clear_pointer(&ui.notice, g_free);
}

static void group_call_capabilities(void)
{
	static const char group[] = "c00000000000000000000000000000000";
	static const char peer[] = "u00000000000000000000000000000001";
	static const char self[] = "u00000000000000000000000000000002";
	g_autoptr(J) login = obj();
	J *caps = obj();

	json_object_object_add(login, "capabilities", caps);
	json_object_object_add(login, "selfId", json_object_new_string(self));
	json_object_object_add(caps, "calls", json_object_new_boolean(TRUE));
	json_object_object_add(caps, "videoCalls",
			       json_object_new_boolean(TRUE));
	ui.selected = g_strdup(group);
	ui_media_login(login);
	g_assert_false(ui_media_can_call(FALSE));
	g_assert_false(ui_media_can_call(TRUE));

	json_object_object_add(caps, "groupVoiceCalls",
			       json_object_new_boolean(TRUE));
	ui_media_login(login);
	g_assert_true(ui_media_can_call(FALSE));
	g_assert_false(ui_media_can_call(TRUE));
	json_object_object_add(caps, "groupVideoCalls",
			       json_object_new_boolean(TRUE));
	ui_media_login(login);
	g_assert_true(ui_media_can_call(FALSE));
	g_assert_true(ui_media_can_call(TRUE));
	json_object_object_del(caps, "groupVoiceCalls");
	ui_media_login(login);
	g_assert_false(ui_media_can_call(FALSE));
	g_assert_true(ui_media_can_call(TRUE));
	g_free(ui.selected);
	ui.selected = g_strdup(peer);
	g_assert_true(ui_media_can_call(FALSE));
	g_assert_true(ui_media_can_call(TRUE));
	g_free(ui.selected);
	ui.selected = g_strdup(self);
	g_assert_false(ui_media_can_call(FALSE));
	g_assert_false(ui_media_can_call(TRUE));

	g_clear_pointer(&ui.selected, g_free);
	ui_media_shutdown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/call/group-call-capabilities",
			group_call_capabilities);
	g_test_add_func("/call/group-share-controls", group_share_controls);
	g_test_add_func("/call/fullscreen-peer", peer_fullscreen);
	return g_test_run();
}
