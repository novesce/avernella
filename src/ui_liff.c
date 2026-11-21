// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The client's side of LINE's in-app browser (avernella-liff, see
 * liff_browser.c). The browser is a separate process started on the first
 * link and gone when its window closes, so it costs nothing otherwise.
 *
 * It is spawned with pidfd_spawn() and supervised through its pidfd, and
 * talks over a SOCK_SEQPACKET socket: the kernel keeps message boundaries
 * and reports the peer's exit as end of file. A reader thread sleeps in
 * recv() and wakes the UI only when the browser speaks.
 */
#include "ui_liff.h"
#include "flex_asset.h"
#include "liff_url.h"
#include "ui_private.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum browser_backend {
	BROWSER_WPE = 1,
	BROWSER_GTK = 2,
};

static struct {
	int fd, pidfd;
	GThread *reader;
	Uint32 event;
	guint generation;
	unsigned int unavailable;
	enum browser_backend backend;
	char *pending;
	gboolean ready;
} lb = { .fd = -1, .pidfd = -1 };

struct reader {
	int fd;
	guint generation;
};

/*
 * Beside avernella in a build tree, or in PREFIX/lib/avernella when
 * installed beside PREFIX/bin/avernella, so any prefix works.
 */
static char *helper_path(const char *name)
{
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

	if (n <= 0)
		return NULL;
	self[n] = '\0';
	g_autofree char *dir = g_path_get_dirname(self);
	g_autofree char *path = g_build_filename(dir, name, NULL);

	if (!access(path, X_OK))
		return g_steal_pointer(&path);
	g_free(path);
	path = g_build_filename(dir, "..", "lib", "avernella", name, NULL);
	return access(path, X_OK) ? NULL : g_steal_pointer(&path);
}

static char *find_browser(enum browser_backend *backend)
{
	const char *wayland = g_getenv("WAYLAND_DISPLAY");
	const char *socket = g_getenv("WAYLAND_SOCKET");

	if (!(lb.unavailable & BROWSER_WPE) &&
	    ((wayland && *wayland) || (socket && *socket))) {
		char *path = helper_path("avernella-liff");

		if (path) {
			*backend = BROWSER_WPE;
			return path;
		}
	}
	if (lb.unavailable & BROWSER_GTK)
		return NULL;
	*backend = BROWSER_GTK;
	return helper_path("avernella-liff-gtk");
}

static gpointer read_browser(gpointer data)
{
	struct reader self = *(struct reader *)data;
	int fd = self.fd;
	guint generation = self.generation;
	char packet[LINE_LIFF_PACKET];

	g_free(data);

	for (;;) {
		ssize_t n = recv(fd, packet, sizeof(packet), 0);

		if (n < 0 && errno == EINTR)
			continue;
		SDL_Event e = { .type = lb.event };

		e.user.code = (Sint32)generation;
		e.user.data1 = n > 0 ? g_strndup(packet, n) : NULL;
		if (SDL_PushEvent(&e) != 1)
			g_free(e.user.data1);
		if (n <= 0)
			return NULL;
	}
}

/* Join the reader and collect the browser's exit through its pidfd. */
static void reap(void)
{
	if (lb.fd < 0)
		return;
	shutdown(lb.fd, SHUT_RDWR);
	if (lb.reader)
		g_thread_join(g_steal_pointer(&lb.reader));
	close(lb.fd);
	lb.fd = -1;
	lb.generation++;
	siginfo_t info = { 0 };

	if (waitid(P_PIDFD, lb.pidfd, &info, WEXITED | WNOHANG) < 0 ||
	    !info.si_pid) {
		/* It saw end of file and is exiting; never leave a zombie. */
		syscall(SYS_pidfd_send_signal, lb.pidfd, SIGTERM, NULL, 0);
		waitid(P_PIDFD, lb.pidfd, &info, WEXITED);
	}
	close(lb.pidfd);
	lb.pidfd = -1;
	g_clear_pointer(&lb.pending, g_free);
}

static int spawn_browser(const char *path, int fd, int *pidfd)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	sigset_t none, reset;
	char parent[24];

	g_snprintf(parent, sizeof(parent), "%d", (int)getpid());
	char *const args[] = { (char *)"avernella-liff", parent, NULL };
	int err = posix_spawn_file_actions_init(&actions);

	if (err)
		return err;
	err = posix_spawnattr_init(&attr);
	if (err) {
		posix_spawn_file_actions_destroy(&actions);
		return err;
	}
	/* WebKit reaps its own processes: SIGCHLD must not be ignored. */
	sigemptyset(&none);
	sigemptyset(&reset);
	sigaddset(&reset, SIGCHLD);
	sigaddset(&reset, SIGPIPE);
	sigaddset(&reset, SIGINT);
	sigaddset(&reset, SIGTERM);
	err = posix_spawnattr_setsigmask(&attr, &none);
	if (!err)
		err = posix_spawnattr_setsigdefault(&attr, &reset);
	if (!err)
		err = posix_spawnattr_setflags(
			&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	if (!err)
		err = posix_spawn_file_actions_adddup2(&actions, fd,
						       LINE_LIFF_FD);
	if (!err)
		err = posix_spawn_file_actions_addclosefrom_np(
			&actions, LINE_LIFF_FD + 1);
#if __GLIBC_PREREQ(2, 39)
	/* clone3(CLONE_PIDFD): the pidfd exists before the child runs. */
	if (!err)
		err = pidfd_spawn(pidfd, path, &actions, &attr, args, environ);
#else
	pid_t pid;

	if (!err)
		err = posix_spawn(&pid, path, &actions, &attr, args, environ);
	if (!err) {
		*pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
		if (*pidfd < 0) {
			err = errno;
			kill(pid, SIGTERM);
			waitpid(pid, NULL, 0);
		}
	}
#endif
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&actions);
	return err;
}

static gboolean start_browser(void)
{
	if (lb.fd >= 0)
		return TRUE;
	if (!lb.event) {
		Uint32 type = SDL_RegisterEvents(1);

		if (type == (Uint32)-1)
			return FALSE;
		lb.event = type;
	}
	int pair[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
		return FALSE;
	g_autofree char *path = NULL;

	while ((path = find_browser(&lb.backend))) {
		int err = spawn_browser(path, pair[1], &lb.pidfd);

		if (!err)
			break;
		lb.unavailable |= lb.backend;
		g_clear_pointer(&path, g_free);
	}
	close(pair[1]);
	if (!path) {
		close(pair[0]);
		lb.pidfd = -1;
		return FALSE;
	}
	struct reader *reader = g_new(struct reader, 1);

	lb.fd = pair[0];
	lb.ready = FALSE;
	reader->fd = lb.fd;
	reader->generation = ++lb.generation;
	lb.reader = g_thread_new("avernella-liff", read_browser, reader);
	return TRUE;
}

static gboolean send_browser_packet(const char *packet)
{
	gsize n = strlen(packet);

	if (n >= LINE_LIFF_PACKET)
		return FALSE;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!start_browser())
			return FALSE;
		ssize_t sent;

		do
			sent = send(lb.fd, packet, n,
				    MSG_NOSIGNAL | MSG_DONTWAIT);
		while (sent < 0 && errno == EINTR);
		if (sent == (ssize_t)n) {
			if (!lb.ready && packet[0] != 'R') {
				g_free(lb.pending);
				lb.pending = g_strdup(packet);
			}
			return TRUE;
		}
		/* The window was just closed: start a new one. */
		if (!lb.ready)
			lb.unavailable |= lb.backend;
		reap();
	}
	return FALSE;
}

static gboolean send_browser(char kind, const char *a, const char *b)
{
	g_autofree char *packet = g_strdup_printf("%c\t%s\t%s", kind, a, b);

	return send_browser_packet(packet);
}

static void open_outside(const char *url)
{
	if (SDL_OpenURL(url) < 0)
		ui_status("Could not open the link.");
}

void ui_liff_open_link(const char *url)
{
	if (!url)
		return;
	if (line_liff_url(url, NULL, NULL)) {
		enum browser_backend backend;
		g_autofree char *path = find_browser(&backend);

		if (lb.fd < 0 && !path) {
			if (line_flex_link_allowed(url))
				open_outside(url);
			return;
		}
		Json *args = json_object_new_object();

		json_object_object_add(args, "url",
				       json_object_new_string(url));
		if (ui.selected)
			json_object_object_add(
				args, "chat",
				json_object_new_string(ui.selected));
		ui_status("Opening the LINE app…");
		ui_rpc("liffView", args, 0);
		return;
	}
	if (line_liff_line_scheme(url)) {
		ui_status("Open this LINE link on your phone.");
		return;
	}
	if (!line_flex_link_allowed(url))
		return;
	if (!send_browser('W', "web", url))
		open_outside(url);
}

gboolean ui_liff_reply(const char *action, guint tag, Json *result,
		       const char *error)
{
	(void)tag;
	if (strcmp(action, "liffView"))
		return FALSE;
	if (error) {
		ui_status(error);
		return TRUE;
	}
	Json *url = NULL, *size = NULL;

	json_object_object_get_ex(result, "url", &url);
	json_object_object_get_ex(result, "size", &size);
	const char *page = json_object_get_string(url);

	if (!page ||
	    !send_browser('L', size ? json_object_get_string(size) : "full",
			  page))
		ui_status("Could not open the LINE app window.");
	else
		ui_status("");
	return TRUE;
}

/* alert() and confirm(), which WPE leaves to its embedder. */
static void show_dialog(const char *id, const char *kind, const char *text)
{
	gboolean confirm = !strcmp(kind, "c");
	const SDL_MessageBoxButtonData alert[] = {
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT |
			  SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
		  1, "OK" },
	};
	const SDL_MessageBoxButtonData ask[] = {
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK" },
	};
	g_autofree char *message =
		g_utf8_make_valid(text, MIN(strlen(text), 4000));
	const SDL_MessageBoxData data = {
		.flags = SDL_MESSAGEBOX_INFORMATION,
		.title = "LINE",
		.message = message,
		.numbuttons = confirm ? 2 : 1,
		.buttons = confirm ? ask : alert,
	};
	int hit = 0;

	if (SDL_ShowMessageBox(&data, &hit) < 0)
		hit = 0;
	send_browser('R', id, hit == 1 ? "1" : "0");
}

static gboolean desktop_scheme(const char *url)
{
	return !g_ascii_strncasecmp(url, "https://", 8) ||
	       !g_ascii_strncasecmp(url, "http://", 7) ||
	       !g_ascii_strncasecmp(url, "mailto:", 7) ||
	       !g_ascii_strncasecmp(url, "tel:", 4);
}

gboolean ui_liff_event(const SDL_Event *e)
{
	if (!lb.event || e->type != lb.event)
		return FALSE;
	g_autofree char *packet = e->user.data1;

	if ((guint)e->user.code != lb.generation)
		return TRUE;
	if (!packet) {
		g_autofree char *pending = g_steal_pointer(&lb.pending);

		if (!lb.ready)
			lb.unavailable |= lb.backend;
		reap();
		if (pending && !send_browser_packet(pending)) {
			if (pending[0] == 'W')
				open_outside(pending + sizeof("W\tweb\t") - 1);
			else
				ui_status(
					"Could not open the LINE app window.");
		}
		return TRUE;
	}
	g_auto(GStrv) part = g_strsplit(packet, "\t", 4);
	guint n = g_strv_length(part);

	if (n == 2 && !strcmp(part[0], "B") && !strcmp(part[1], "ready")) {
		lb.ready = TRUE;
		g_clear_pointer(&lb.pending, g_free);
	} else if (n == 2 && !strcmp(part[0], "O"))
		ui_liff_open_link(part[1]);
	else if (n == 2 && !strcmp(part[0], "X") && desktop_scheme(part[1]))
		open_outside(part[1]);
	else if (n == 4 && !strcmp(part[0], "D"))
		show_dialog(part[1], part[2], part[3]);
	return TRUE;
}

void ui_liff_close(void)
{
	reap();
}
