// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_instance.h"
#include <SDL.h>
#include <glib.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

static char **child_environment(void)
{
	char **env = g_get_environ();

	/* The GLib test harness overrides its directory getters in-process. */
	env = g_environ_setenv(env, "XDG_STATE_HOME", g_get_user_state_dir(),
			       TRUE);
	env = g_environ_setenv(env, "XDG_RUNTIME_DIR", g_get_user_runtime_dir(),
			       TRUE);
	return env;
}

/* Exec a fresh process, as a desktop launcher would. */
static void launch(const char *mode)
{
	g_auto(GStrv) env = child_environment();
	g_autoptr(GError) error = NULL;
	const char *const argv[] = { "/proc/self/exe", mode, NULL };
	int status;

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

/* Leave the activation socket open after the SDL main thread has exited. */
static gpointer hold_socket(gpointer unused)
{
	char path[64], signal = 'Z';

	(void)unused;
	g_snprintf(path, sizeof(path), "/proc/%d/stat", getpid());
	for (;;) {
		g_autofree char *stat = NULL;

		g_assert_true(g_file_get_contents(path, &stat, NULL, NULL));
		if (g_str_has_prefix(strrchr(stat, ')'), ") Z "))
			break;
		g_usleep(1000);
	}
	g_assert_cmpint(write(STDOUT_FILENO, &signal, 1), ==, 1);
	if (read(STDIN_FILENO, &signal, 1) == 1)
		ui_instance_close();
	_exit(0);
}

static void zombie_owner(void)
{
	g_auto(GStrv) env = child_environment();
	g_autoptr(GError) error = NULL;
	static const char *const argv[] = { "/proc/self/exe", "--lose-main",
					    NULL };
	int input, output, status;
	GPid pid;
	char ready;

	g_assert_true(g_spawn_async_with_pipes(
		NULL, (char **)argv, env, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
		&pid, &input, &output, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(read(output, &ready, 1), ==, 1);
	g_assert_cmpint(ready, ==, 'Z');
	close(output);
	g_assert_cmpint(ui_instance_open(&error), ==, 1);
	g_assert_no_error(error);
	/* Both locks coexist until the obsolete owner finishes cleanup. */
	launch("--activate");
	g_assert_cmpint(write(input, &ready, 1), ==, 1);
	close(input);
	g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
	launch("--activate");
	ui_instance_close();
}

/* An older binary binds the well-known datagram address directly. */
static void legacy_owner(void)
{
	g_autofree char *path =
		g_build_filename(g_get_user_state_dir(), "avernella-ui", NULL);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char show;
	int fd, dir;

	g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0);
	dir = open(path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(dir, >=, 0);
	g_snprintf(addr.sun_path, sizeof(addr.sun_path),
		   "/proc/self/fd/%d/activate", dir);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==,
			0);
	launch("--activate");
	g_assert_cmpint(recv(fd, &show, sizeof(show), 0), ==, 1);
	g_assert_cmpint(show, ==, 1);
	close(fd);
	close(dir);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--activate"))
		return ui_instance_open(NULL) == 0 ? 0 : 1;
	if (argc == 2 && !strcmp(argv[1], "--abandon"))
		_exit(ui_instance_open(NULL) == 1 ? 0 : 1);

	if (argc == 2 && !strcmp(argv[1], "--lose-main")) {
		alarm(8);
		if (ui_instance_open(NULL) != 1)
			return 1;
		g_thread_unref(g_thread_new("held-device", hold_socket, NULL));
		pthread_exit(NULL);
	}

	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/instance/early-activation", activation);
	g_test_add_func("/instance/restart", restart);
	g_test_add_func("/instance/zombie-owner", zombie_owner);
	g_test_add_func("/instance/legacy-owner", legacy_owner);
	return g_test_run();
}
