// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_io.h"
#include "camera.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void close_lease(gpointer value)
{
	close(GPOINTER_TO_INT(value));
}

GHashTable *line_leases_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
				     close_lease);
}

static gboolean write_bytes(int fd, GBytes *bytes)
{
	gsize n;
	const guint8 *p = g_bytes_get_data(bytes, &n);

	while (n) {
		ssize_t wrote = write(fd, p, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return FALSE;
		n -= wrote;
		p += wrote;
	}
	return TRUE;
}

static gboolean seal(int fd)
{
	return fcntl(fd, F_ADD_SEALS,
		     F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE |
			     F_SEAL_SEAL) == 0;
}

gboolean line_media_send_share(int socket, const struct line_share_frame *frame,
			       const struct line_camera_frame *image)
{
	gsize size = line_camera_i420_size(image->width, image->height);
	int fd = memfd_create("line-share", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (fd < 0)
		return FALSE;
	gboolean sent = FALSE;

	void *map = ftruncate(fd, size) == 0 ?
			    mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0) :
			    MAP_FAILED;

	if (map != MAP_FAILED) {
		line_camera_pack_i420(map, image);
		munmap(map, size);
	}
	if (map != MAP_FAILED && seal(fd)) {
		union {
			struct cmsghdr align;
			char bytes[CMSG_SPACE(sizeof(fd))];
		} control = { 0 };
		struct iovec payload = { .iov_base = (void *)frame,
					 .iov_len = sizeof(*frame) };
		struct msghdr message = { .msg_iov = &payload,
					  .msg_iovlen = 1,
					  .msg_control = control.bytes,
					  .msg_controllen =
						  sizeof(control.bytes) };
		struct cmsghdr *c = CMSG_FIRSTHDR(&message);

		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(fd));
		memcpy(CMSG_DATA(c), &fd, sizeof(fd));
		sent = sendmsg(socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL) ==
		       (ssize_t)sizeof(*frame);
	}
	close(fd);
	return sent;
}

int line_media_recv_share(int socket, struct line_share_frame *frame)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = { 0 };
	struct iovec payload = { .iov_base = frame, .iov_len = sizeof(*frame) };
	struct msghdr message = { .msg_iov = &payload,
				  .msg_iovlen = 1,
				  .msg_control = control.bytes,
				  .msg_controllen = sizeof(control.bytes) };
	ssize_t n;

	do {
		n = recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return -1;
	int fd = -1;
	guint count = 0;

	for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c;
	     c = CMSG_NXTHDR(&message, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
		    c->cmsg_len < CMSG_LEN(sizeof(int)))
			continue;
		for (size_t i = 0; i + sizeof(int) <= c->cmsg_len - CMSG_LEN(0);
		     i += sizeof(int)) {
			int received;

			memcpy(&received, CMSG_DATA(c) + i, sizeof(received));
			if (count++ == 0)
				fd = received;
			else
				close(received);
		}
	}
	if (n != (ssize_t)sizeof(*frame) || count != 1 ||
	    (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
	    frame->chat[sizeof(frame->chat) - 1]) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

J *line_media_lease(GHashTable *leases, GBytes *bytes, GError **error)
{
	if (g_hash_table_size(leases) >= 4) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Finish opening another image first.");
		return NULL;
	}
	int fd = memfd_create("line-image", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (fd < 0 || !write_bytes(fd, bytes) || !seal(fd)) {
		g_set_error(error, LINE_ERROR, 1,
			    "Could not open private image memory: %s.",
			    g_strerror(errno));
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	char *lease = g_uuid_string_random();

	g_hash_table_insert(leases, lease, GINT_TO_POINTER(fd));
	g_autofree char *path =
		g_strdup_printf("/proc/%ld/fd/%d", (long)getpid(), fd);
	J *result = obj();

	json_object_object_add(result, "lease", json_object_new_string(lease));
	json_object_object_add(result, "path", json_object_new_string(path));
	return result;
}

gboolean line_save_media(const char *path, GBytes *bytes, gboolean replace,
			 GError **error)
{
	if (!path || !g_path_is_absolute(path) || !bytes) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Choose a destination file.");
		return FALSE;
	}
	g_autofree char *dir = g_path_get_dirname(path),
			*base = g_path_get_basename(path);
	g_autofree char *uuid = g_uuid_string_random(),
			*tmp = g_strconcat(".line-download-", uuid, NULL);
	int dfd = open(dir, O_DIRECTORY | O_CLOEXEC | O_RDONLY);

	if (dfd < 0)
		goto failed;
	int fd = openat(dfd, tmp,
			O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
			0600);
	if (fd < 0) {
		int saved = errno;

		close(dfd);
		errno = saved;
		goto failed;
	}
	gboolean ok = write_bytes(fd, bytes) && !fsync(fd);
	int saved = errno;

	if (close(fd) < 0 && ok) {
		ok = FALSE;
		saved = errno;
	}
	if (ok && renameat2(dfd, tmp, dfd, base,
			    replace ? 0 : RENAME_NOREPLACE) < 0) {
		saved = errno;
		ok = FALSE;
	}
	if (!ok)
		unlinkat(dfd, tmp, 0);
	close(dfd);
	if (ok)
		return TRUE;
	errno = saved;
failed:
	g_set_error(error, LINE_ERROR, 1, "Could not save the attachment: %s.",
		    g_strerror(errno));
	return FALSE;
}
