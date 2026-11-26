// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_instance.h"
#include <errno.h>
#include <linux/sockios.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
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

enum owner {
	OWNER_LIVE, /* holds the record lock */
	OWNER_UNTRACKED, /* no lock: an older binary, or one already exiting */
	OWNER_DEAD, /* its process is a zombie */
};

/*
 * While owning the bound socket, hold a shared POSIX record lock on its
 * inode's byte in the private directory. F_GETLK identifies the owning
 * process in the caller's PID namespace, even if a driver thread outlives
 * the main thread and keeps the socket open. This module owns the only FD
 * for this directory in the UI process: closing another FD for the same
 * inode would release the POSIX lock.
 */
static enum owner owner_state(int dir, ino_t inode)
{
	struct flock lock = { .l_type = F_WRLCK,
			      .l_whence = SEEK_SET,
			      .l_start = (off_t)inode,
			      .l_len = 1 };
	g_autofree char *stat = NULL;
	char path[64];
	const char *end;

	if (fcntl(dir, F_GETLK, &lock) || lock.l_type == F_UNLCK)
		return OWNER_UNTRACKED;
	if (lock.l_type != F_RDLCK || lock.l_pid <= 0)
		return OWNER_LIVE;
	g_snprintf(path, sizeof(path), "/proc/%d/stat", lock.l_pid);
	if (!g_file_get_contents(path, &stat, NULL, NULL))
		return OWNER_LIVE;
	end = strrchr(stat, ')');
	return end && (g_str_has_prefix(end, ") Z ") ||
		       g_str_has_prefix(end, ") X ")) ?
		       OWNER_DEAD :
		       OWNER_LIVE;
}

/*
 * Whether an owner without a lock read our request. A Unix datagram stays
 * charged to its sender (SIOCOUTQ) until the receiver takes it. A process
 * that is exiting has dropped its lock but may keep the socket open for
 * as long as a device driver blocks its exit, and never reads again.
 */
static gboolean answered(int fd)
{
	for (int waited = 0; waited <= 500; waited += 5) {
		int queued;

		if (ioctl(fd, SIOCOUTQ, &queued) || !queued)
			return TRUE;
		g_usleep(5000);
	}
	return FALSE;
}

/* The directory lock serializes binding, probing and unlinking a dead
 * owner's socket. The kernel queues launches even before SDL is ready.
 */
static int claim_socket(int dir, int fd, const char *url)
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
	enum owner owner = owner_state(dir, st.st_ino);

	if (owner != OWNER_DEAD) {
		char packet[4098];
		size_t pkt_len = 1;

		packet[0] = 1;
		if (url && *url && strlen(url) <= 4096) {
			size_t ulen = strlen(url);

			packet[0] = 'U';
			memcpy(packet + 1, url, ulen);
			pkt_len = ulen + 1;
		}
		ssize_t sent = TEMP_FAILURE_RETRY(
			sendto(fd, packet, pkt_len, MSG_NOSIGNAL,
			       (struct sockaddr *)&addr, len));

		if (sent < 0 && errno != EAGAIN && errno != ECONNREFUSED)
			return -1;
		/* A full queue means an untracked owner stopped reading. */
		if ((sent >= 0 || errno == EAGAIN) &&
		    (owner == OWNER_LIVE || (sent >= 0 && answered(fd)))) {
			g_printerr(
				"avernella: already running; showing its window\n");
			return 0;
		}
		if (sent >= 0 || errno == EAGAIN)
			g_printerr(
				"avernella: the running window does not respond; starting a new one\n");
	} else {
		g_printerr(
			"avernella: the previous window is still exiting; starting a new one\n");
	}
	if (unlinkat(dir, ENDPOINT, 0))
		return -1;
	return bind(fd, (struct sockaddr *)&addr, len) ? -1 : 1;
}

int ui_instance_open_url(const char *url, GError **error)
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
		result = claim_socket(dir, fd, url);
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

int ui_instance_open(GError **error)
{
	return ui_instance_open_url(NULL, error);
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
		char buf[4098];
		ssize_t n = recv(instance.fd, buf, sizeof(buf) - 1, MSG_TRUNC);

		if (n == 1 && buf[0] == 1) {
			SDL_Event event = { .type = instance.event };

			SDL_PushEvent(&event);
		} else if (n > 1 && n < (ssize_t)sizeof(buf) && buf[0] == 'U') {
			buf[n] = '\0';
			SDL_Event event = { .type = instance.event };

			event.user.data1 = g_strdup(buf + 1);
			if (SDL_PushEvent(&event) != 1)
				g_free(event.user.data1);
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
