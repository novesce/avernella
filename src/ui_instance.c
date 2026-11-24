// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_instance.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define ENDPOINT "activate"

static struct {
	int dir, fd, stop;
	ino_t inode;
	GThread *thread;
	Uint32 event;
} instance = { .dir = -1, .fd = -1, .stop = -1 };

static void instance_error(GError **error)
{
	int reason = errno;

	g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(reason),
		    "Could not open Avernella's window activation channel: %s",
		    g_strerror(reason));
}

/*
 * While owning the bound socket, hold a shared POSIX record lock on its
 * inode's byte in the private directory. F_GETLK identifies the owning
 * process in the caller's PID namespace, even if a driver thread outlives
 * the main thread and keeps the socket open. This module owns the only FD
 * for this directory in the UI process: closing another FD for the same
 * inode would release the POSIX lock. Leave untracked owners alone.
 */
static gboolean dead_owner(int dir, ino_t inode)
{
	struct flock lock = { .l_type = F_WRLCK,
			      .l_whence = SEEK_SET,
			      .l_start = (off_t)inode,
			      .l_len = 1 };
	g_autofree char *stat = NULL;
	char path[64];
	const char *end;

	if (fcntl(dir, F_GETLK, &lock) || lock.l_type != F_RDLCK ||
	    lock.l_pid <= 0)
		return FALSE;
	g_snprintf(path, sizeof(path), "/proc/%d/stat", lock.l_pid);
	if (!g_file_get_contents(path, &stat, NULL, NULL))
		return FALSE;
	end = strrchr(stat, ')');
	return end &&
	       (g_str_has_prefix(end, ") Z ") || g_str_has_prefix(end, ") X "));
}

/* The directory lock serializes binding, probing and unlinking a dead
 * owner's socket. The kernel queues launches even before SDL is ready.
 */
static int claim_socket(int dir, int fd)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct stat st;
	int n = g_snprintf(addr.sun_path, sizeof(addr.sun_path),
			   "/proc/self/fd/%d/" ENDPOINT, dir);
	socklen_t len = offsetof(struct sockaddr_un, sun_path) + n + 1;

	if (TEMP_FAILURE_RETRY(flock(dir, LOCK_EX)))
		return -1;
	if (!bind(fd, (struct sockaddr *)&addr, len))
		return 1;
	if (errno != EADDRINUSE ||
	    fstatat(dir, ENDPOINT, &st, AT_SYMLINK_NOFOLLOW))
		return -1;
	if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
		errno = EEXIST;
		return -1;
	}
	if (!dead_owner(dir, st.st_ino)) {
		char show = 1;
		ssize_t sent = TEMP_FAILURE_RETRY(
			sendto(fd, &show, sizeof(show), MSG_NOSIGNAL,
			       (struct sockaddr *)&addr, len));

		if (sent >= 0 || errno == EAGAIN)
			return 0;
		if (errno != ECONNREFUSED)
			return -1;
	}
	if (unlinkat(dir, ENDPOINT, 0))
		return -1;
	return bind(fd, (struct sockaddr *)&addr, len) ? -1 : 1;
}

int ui_instance_open(GError **error)
{
	g_autofree char *path =
		g_build_filename(g_get_user_state_dir(), "avernella-ui", NULL);
	struct stat st;
	int dir = -1, fd = -1, result = -1;

	if (g_mkdir_with_parents(path, 0700))
		goto out;
	dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0 || fstat(dir, &st))
		goto out;
	if (st.st_uid != geteuid() || (st.st_mode & 0077)) {
		errno = EACCES;
		goto out;
	}
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd >= 0)
		result = claim_socket(dir, fd);
	if (result == 1 && fstatat(dir, ENDPOINT, &st, AT_SYMLINK_NOFOLLOW))
		result = -1;
	if (result == 1) {
		struct flock lock = { .l_type = F_RDLCK,
				      .l_whence = SEEK_SET,
				      .l_start = (off_t)st.st_ino,
				      .l_len = 1 };

		/* Filesystems without record locks retain ordinary activation. */
		fcntl(dir, F_SETLK, &lock);
		instance.dir = dir;
		instance.fd = fd;
		instance.inode = st.st_ino;
		flock(dir, LOCK_UN);
		return 1;
	}
out:
	if (result < 0)
		instance_error(error);
	if (fd >= 0)
		close(fd);
	if (dir >= 0)
		close(dir);
	return result;
}

static gpointer watch_launch(gpointer unused)
{
	struct pollfd fds[] = {
		{ .fd = instance.fd, .events = POLLIN },
		{ .fd = instance.stop, .events = POLLIN },
	};

	(void)unused;
	while (TEMP_FAILURE_RETRY(poll(fds, G_N_ELEMENTS(fds), -1)) > 0) {
		if (fds[1].revents || !(fds[0].revents & POLLIN))
			break;
		char show;

		if (recv(instance.fd, &show, sizeof(show), MSG_TRUNC) == 1 &&
		    show == 1) {
			SDL_Event event = { .type = instance.event };

			SDL_PushEvent(&event);
		}
	}
	return NULL;
}

gboolean ui_instance_watch(Uint32 event, GError **error)
{
	instance.stop = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (instance.stop < 0) {
		instance_error(error);
		return FALSE;
	}
	instance.event = event;
	instance.thread =
		g_thread_try_new("avernella-launch", watch_launch, NULL, error);
	return instance.thread != NULL;
}

void ui_instance_close(void)
{
	if (instance.thread) {
		eventfd_t stop = 1;

		TEMP_FAILURE_RETRY(eventfd_write(instance.stop, stop));
		g_thread_join(instance.thread);
		instance.thread = NULL;
	}
	if (instance.stop >= 0) {
		close(instance.stop);
		instance.stop = -1;
	}
	if (instance.dir >= 0) {
		struct stat st;

		if (!TEMP_FAILURE_RETRY(flock(instance.dir, LOCK_EX)) &&
		    !fstatat(instance.dir, ENDPOINT, &st,
			     AT_SYMLINK_NOFOLLOW) &&
		    S_ISSOCK(st.st_mode) && st.st_ino == instance.inode)
			unlinkat(instance.dir, ENDPOINT, 0);
		close(instance.fd);
		close(instance.dir);
		instance.fd = instance.dir = -1;
	}
}
