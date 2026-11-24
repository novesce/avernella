// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor_picker.h"
#include <SDL.h>
#include <unistd.h>

static const char *script;
static guint step;

/* Drive the actual picker in the subprocess with a dummy video device.
 * No compositor, D-Bus call, PipeWire connection or desktop is involved.
 */
int __wrap_SDL_WaitEvent(SDL_Event *event)
{
	memset(event, 0, sizeof(*event));
	if (strstr(script, "wait"))
		for (;;)
			pause();
	if (strstr(script, "mouse")) {
		event->type = step % 2 ? SDL_MOUSEBUTTONUP :
					 SDL_MOUSEBUTTONDOWN;
		event->button.button = SDL_BUTTON_LEFT;
		/* Press Share before picking, select row 2, then confirm. */
		event->button.x = step / 2 == 1 ? 100 : 420;
		event->button.y = step / 2 == 1 ? 140 : 200;
		return ++step <= 6;
	}
	event->type = SDL_KEYDOWN;
	if (strstr(script, "cancel")) {
		/* Enter with no selection and cancellation after a selection. */
		static const SDL_Keycode keys[] = { SDLK_RETURN, SDLK_DOWN,
						    SDLK_ESCAPE };

		if (step >= G_N_ELEMENTS(keys))
			return 0;
		event->key.keysym.sym = keys[step++];
	} else {
		/* Up starts at the last monitor, even beyond the first page. */
		event->key.keysym.sym = step++ == 0 ? SDLK_UP : SDLK_RETURN;
	}
	return 1;
}

static void keyboard(void)
{
	static const char *const ids[] = { "A", "B", "C", "D", "E", "F", "G" };
	static const char *const labels[] = { "One",  "Two", "Three", "Four",
					      "Five", "Six", "Seven" };

	g_autoptr(GError) error = NULL;
	g_autofree char *result =
		line_pick_share_monitor(ids, labels, 7, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "G");
}

static void mouse(void)
{
	static const char *const ids[] = { "eDP-1", "HDMI-1" };
	static const char *const labels[] = { "mouse", "External" };

	g_autoptr(GError) error = NULL;
	g_autofree char *result =
		line_pick_share_monitor(ids, labels, 2, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "HDMI-1");
}

static void decline(void)
{
	static const char *const ids[] = { "DP-1" };
	static const char *const labels[] = { "cancel" };

	g_autoptr(GError) error = NULL;
	g_autofree char *result =
		line_pick_share_monitor(ids, labels, 1, NULL, &error);

	g_assert_null(result);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static gpointer cancel_call(gpointer cancel)
{
	g_usleep(200000);
	g_cancellable_cancel(cancel);
	return NULL;
}

static void hang_up(void)
{
	static const char *const ids[] = { "DP-1" };
	static const char *const labels[] = { "wait" };

	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();

	GThread *thread = g_thread_new("hang-up", cancel_call, cancel);
	g_autofree char *result =
		line_pick_share_monitor(ids, labels, 1, cancel, &error);

	g_thread_join(thread);
	g_assert_null(result);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

int main(int argc, char **argv)
{
	alarm(15);
	if (argc > 2 && !strcmp(argv[1], "--pick-share-monitor")) {
		script = argv[2];
		return ui_pick_share_monitor(argc - 2, argv + 2);
	}
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/monitor-picker/keyboard", keyboard);
	g_test_add_func("/monitor-picker/mouse", mouse);
	g_test_add_func("/monitor-picker/decline", decline);
	g_test_add_func("/monitor-picker/hang-up", hang_up);
	return g_test_run();
}
