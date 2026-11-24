// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "state.h"
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int line_ui(int argc, char **argv);
int __real_SDL_PollEvent(SDL_Event *event);
pid_t __real_waitpid(pid_t pid, int *status, int options);

static GThread *ui_thread;
static GMutex lock;
static GCond finished;
static gboolean disconnected, reaped;

gboolean line_store_has_session(void)
{
	return TRUE;
}

/* A media worker can lose its IPC connection before its devices close. */
static int worker(void)
{
	g_autofree char *line = NULL;
	size_t size = 0;
	struct sigaction action;

	alarm(10);
	if (getline(&line, &size, stdin) < 0 ||
	    SDL_InitSubSystem(SDL_INIT_AUDIO) < 0 ||
	    sigaction(SIGTERM, NULL, &action))
		return 1;
	g_autoptr(J) login = json_tokener_parse(line);

	printf("{\"id\":%d,\"result\":{}}\n",
	       json_object_get_int(ui_field(login, "id")));
	printf("{\"event\":\"status\",\"message\":\"%s\"}\n",
	       action.sa_handler == SIG_DFL ? "Worker ready" : "TERM caught");
	fflush(stdout);
	/* Wait until the UI has checked the status before closing the socket. */
	for (;;) {
		if (getline(&line, &size, stdin) < 0)
			return 1;
		g_autoptr(J) req = json_tokener_parse(line);

		if (!strcmp(ui_string(req, "method", ""), "dropConnection"))
			break;
	}
	shutdown(STDIN_FILENO, SHUT_RDWR);
	for (;;)
		pause();
}

pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
	if (g_thread_self() == ui_thread) {
		g_assert_true((options & WNOHANG) != 0);
		return 0;
	}
	/* Hold the reap until the disconnected window handles another event. */
	g_mutex_lock(&lock);
	while (!disconnected)
		g_cond_wait(&finished, &lock);
	g_mutex_unlock(&lock);
	pid_t result = __real_waitpid(pid, status, options);

	if (result == pid) {
		g_mutex_lock(&lock);
		reaped = TRUE;
		g_cond_signal(&finished);
		g_mutex_unlock(&lock);
	}
	return result;
}

int __wrap_SDL_PollEvent(SDL_Event *event)
{
	static gboolean requested;

	g_assert_cmpstr(ui.notice, !=, "TERM caught");
	if (!requested && !g_strcmp0(ui.notice, "Worker ready")) {
		requested = TRUE;
		ui_rpc("dropConnection", json_object_new_object(), 0);
	} else if (!disconnected && requested && !ui.authenticating &&
		   !ui.connected) {
		SDL_Event quit = { .type = SDL_QUIT };

		g_mutex_lock(&lock);
		disconnected = TRUE;
		g_cond_signal(&finished);
		g_mutex_unlock(&lock);
		g_assert_cmpint(SDL_PushEvent(&quit), ==, 1);
	}
	return __real_SDL_PollEvent(event);
}

static void worker_disconnect(void)
{
	alarm(12);
	ui_thread = g_thread_self();
	g_assert_cmpint(line_ui(0, NULL), ==, 0);
	g_assert_true(disconnected);
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	g_mutex_lock(&lock);
	while (!reaped && g_cond_wait_until(&finished, &lock, deadline))
		;
	g_assert_true(reaped);
	g_mutex_unlock(&lock);
	alarm(0);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--worker"))
		return worker();
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	g_setenv("LINE_DISABLE_TRAY", "1", TRUE);
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui/worker/call-disconnect", worker_disconnect);
	return g_test_run();
}
