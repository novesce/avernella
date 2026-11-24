// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_instance.h"
#include <SDL.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

/* Exec a fresh process, as a desktop launcher would. */
static void launch(const char *mode)
{
	const char *argv[] = { "/proc/self/exe", mode, NULL };
	g_auto(GStrv) env = g_get_environ();
	g_autoptr(GError) error = NULL;
	int status;

	/* The GLib test harness overrides its directory getters in-process. */
	env = g_environ_setenv(env, "XDG_STATE_HOME", g_get_user_state_dir(),
			       TRUE);
	env = g_environ_setenv(env, "XDG_RUNTIME_DIR", g_get_user_runtime_dir(),
			       TRUE);
	g_assert_true(g_spawn_sync(NULL, (char **)argv, env, 0, NULL, NULL,
				   NULL, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
}

static void activation(void)
{
	g_autoptr(GError) error = NULL;
	SDL_Event event;

	g_assert_cmpint(ui_instance_open(&error), ==, 1);
	g_assert_no_error(error);
	/* A second launch can arrive before the primary creates its window. */
	launch("--activate");
	g_assert_cmpint(SDL_Init(SDL_INIT_EVENTS), ==, 0);
	Uint32 type = SDL_RegisterEvents(1);

	g_assert_cmpuint(type, !=, (Uint32)-1);
	g_assert_true(ui_instance_watch(type, &error));
	g_assert_no_error(error);
	g_assert_cmpint(SDL_WaitEventTimeout(&event, 3000), ==, 1);
	g_assert_cmpuint(event.type, ==, type);
	ui_instance_close();
	SDL_Quit();
}

static void restart(void)
{
	g_autoptr(GError) error = NULL;

	/* This owner exits without running the cleanup path. */
	launch("--abandon");
	g_assert_cmpint(ui_instance_open(&error), ==, 1);
	g_assert_no_error(error);
	ui_instance_close();
	g_assert_cmpint(ui_instance_open(&error), ==, 1);
	g_assert_no_error(error);
	ui_instance_close();
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--activate"))
		return ui_instance_open(NULL) == 0 ? 0 : 1;
	if (argc == 2 && !strcmp(argv[1], "--abandon"))
		_exit(ui_instance_open(NULL) == 1 ? 0 : 1);

	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/instance/early-activation", activation);
	g_test_add_func("/instance/restart", restart);
	return g_test_run();
}
