// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_io.h"
#include "camera.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* Linux 6.0 uAPI. An older running kernel selects the I420 fallback. */
#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file {
	__u32 flags;
	__s32 fd;
};
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE \
	_IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#endif

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

/*
 * Pixels and media are never code. Linux 6.3 and later seal the memory
 * non-executable at creation, which vm.memfd_noexec=2 also requires; older
 * kernels refuse the flag and get the plain sealable memfd.
 */
static int sealable_memfd(const char *name)
{
	int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING |
					    MFD_NOEXEC_SEAL);

	if (fd < 0 && errno == EINVAL)
		fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	return fd;
}

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

static gboolean write_rows(int fd, struct iovec *rows, guint count, gsize bytes,
			   off_t offset)
{
	ssize_t n;

	do {
		n = pwritev(fd, rows, count, offset);
	} while (n < 0 && errno == EINTR);
	return n == (ssize_t)bytes;
}

/* Copy decoder rows straight to the memfd. One vector covers a packed plane. */
static gboolean write_i420(int fd, const struct line_camera_frame *image)
{
	struct iovec rows[128];
	guint count = 0;
	gsize bytes = 0;
	off_t offset = 0;

	for (guint i = 0; i < 3; i++) {
		guint w = i ? (image->width + 1) / 2 : image->width;
		guint h = i ? (image->height + 1) / 2 : image->height;

		if (image->stride[i] == (int)w) {
			w *= h;
			h = 1;
		}
		for (guint y = 0; y < h; y++) {
			rows[count++] = (struct iovec){
				.iov_base =
					(void *)(image->plane[i] +
						 (gssize)y * image->stride[i]),
				.iov_len = w,
			};
			bytes += w;
			if (count == G_N_ELEMENTS(rows)) {
				if (!write_rows(fd, rows, count, bytes, offset))
					return FALSE;
				offset += bytes;
				count = 0;
				bytes = 0;
			}
		}
	}
	return !count || write_rows(fd, rows, count, bytes, offset);
}

static ssize_t preview_send(int socket, const struct line_preview_frame *frame,
			    const int *fds, guint count)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(2 * sizeof(int))];
	} control = { 0 };
	struct iovec payload = { .iov_base = (void *)frame,
				 .iov_len = sizeof(*frame) };
	struct msghdr message = { .msg_iov = &payload,
				  .msg_iovlen = 1,
				  .msg_control = control.bytes,
				  .msg_controllen = sizeof(control.bytes) };
	struct cmsghdr *c = CMSG_FIRSTHDR(&message);

	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(count * sizeof(int));
	message.msg_controllen = CMSG_SPACE(count * sizeof(int));
	memcpy(CMSG_DATA(c), fds, count * sizeof(int));
	ssize_t n;

	do {
		n = sendmsg(socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	return n;
}

gboolean line_media_send_dmabuf(int socket,
				const struct line_preview_frame *report,
				const struct line_dmabuf_layout *layout,
				int dma_fd, int release_fd)
{
	struct line_preview_frame frame = *report;
	const int fds[] = { dma_fd, release_fd };

	frame.dmabuf = 1;
	frame.layout = *layout;
	return preview_send(socket, &frame, fds, 2) == (ssize_t)sizeof(frame);
}

gboolean line_media_send_frame(int socket,
			       const struct line_preview_frame *frame,
			       const struct line_camera_frame *image)
{
	struct pollfd pfd = { .fd = socket, .events = POLLOUT };
	int ready;

	/* The socket carries live previews. Drop early while the UI is behind. */
	do {
		ready = poll(&pfd, 1, 0);
	} while (ready < 0 && errno == EINTR);
	if (!ready)
		return socket >= 0;
	if (ready < 0 || !(pfd.revents & POLLOUT) ||
	    pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return FALSE;
	int fd = sealable_memfd("line-preview");

	if (fd < 0)
		return FALSE;
	gboolean sent = FALSE;

	if (write_i420(fd, image) && seal(fd)) {
		ssize_t n = preview_send(socket, frame, &fd, 1);
		/* A full preview queue can drop this picture. */
		sent = n == (ssize_t)sizeof(*frame) ||
		       (n < 0 && errno == EAGAIN);
	}
	close(fd);
	return sent;
}

static gboolean valid_layout(const struct line_preview_frame *frame)
{
	const struct line_dmabuf_layout *d = &frame->layout;

	return line_preview_size_valid(frame->width, frame->height,
				       frame->source == LINE_PREVIEW_SCREEN) &&
	       d->width >= frame->width && d->width <= 8192 &&
	       d->height >= frame->height && d->height <= 8192 &&
	       d->crop_x <= d->width - frame->width &&
	       d->crop_y <= d->height - frame->height &&
	       !((d->crop_x | d->crop_y) & 1) && d->full_range <= 1 &&
	       d->stride[0] >= d->width &&
	       d->stride[1] >= ((d->width + 1u) & ~1u);
}

int line_media_recv_preview(int socket, struct line_preview_frame *frame,
			    int *release_fd)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(2 * sizeof(int))];
	} control = { 0 };
	struct iovec payload = { .iov_base = frame, .iov_len = sizeof(*frame) };
	struct msghdr message = { .msg_iov = &payload,
				  .msg_iovlen = 1,
				  .msg_control = control.bytes,
				  .msg_controllen = sizeof(control.bytes) };
	ssize_t n;

	*release_fd = -1;
	do {
		n = recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
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
			if (count == 0)
				fd = received;
			else if (count == 1)
				*release_fd = received;
			else
				close(received);
			count++;
		}
	}
	if (n != (ssize_t)sizeof(*frame) || frame->dmabuf > 1 ||
	    count != 1u + frame->dmabuf ||
	    (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
	    frame->source >= LINE_PREVIEW_COUNT ||
	    frame->chat[sizeof(frame->chat) - 1]) {
		if (fd >= 0)
			close(fd);
		if (*release_fd >= 0)
			close(*release_fd);
		*release_fd = -1;
		return -1;
	}
	if (frame->dmabuf) {
		int type, family;
		socklen_t len = sizeof(int);
		struct dma_buf_export_sync_file fence = {
			.flags = DMA_BUF_SYNC_READ, .fd = -1
		};
		gboolean local = !getsockopt(*release_fd, SOL_SOCKET, SO_TYPE,
					     &type, &len) &&
				 type == SOCK_STREAM &&
				 !getsockopt(*release_fd, SOL_SOCKET, SO_DOMAIN,
					     &family, &len) &&
				 family == AF_UNIX;

		if (!local || !valid_layout(frame) ||
		    ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &fence)) {
			if (local)
				(void)send(*release_fd, "R", 1,
					   MSG_NOSIGNAL | MSG_DONTWAIT);
			close(fd);
			close(*release_fd);
			*release_fd = -1;
			return -1;
		}
		close(fence.fd);
	}
	return fd;
}

int line_media_recv_frame(int socket, struct line_preview_frame *frame)
{
	int release_fd;
	int fd = line_media_recv_preview(socket, frame, &release_fd);

	if (release_fd >= 0) {
		(void)send(release_fd, "R", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
		close(release_fd);
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
	int fd = sealable_memfd("line-image");

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
	J *result = obj();

	json_object_object_add(result, "lease", json_object_new_string(lease));
	return result;
}

int line_media_lease_open(GHashTable *leases, const char *lease)
{
	gpointer value;
	char path[64];

	if (!lease ||
	    !g_hash_table_lookup_extended(leases, lease, NULL, &value))
		return -1;
	/* A new read-only open file, with its own offset, like open(2) had. */
	g_snprintf(path, sizeof(path), "/proc/self/fd/%d",
		   GPOINTER_TO_INT(value));
	return open(path, O_RDONLY | O_CLOEXEC);
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

gboolean line_wire_send(int socket, const char *text, gsize length, int fd)
{
	struct iovec parts[] = { { (void *)text, length }, { "\n", 1 } };
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = { 0 };
	struct msghdr message = { 0 };
	int first = 0;

	if (fd >= 0) {
		struct cmsghdr *cm = &control.align;

		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control.bytes);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
	}
	while (first < (int)G_N_ELEMENTS(parts)) {
		ssize_t sent;

		message.msg_iov = parts + first;
		message.msg_iovlen = G_N_ELEMENTS(parts) - first;
		sent = message.msg_control ?
			       sendmsg(socket, &message, MSG_NOSIGNAL) :
			       writev(socket, parts + first,
				      G_N_ELEMENTS(parts) - first);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && errno == ENOTSOCK && message.msg_control) {
			/* Not the UI's socket: the line still goes out. */
			message.msg_control = NULL;
			message.msg_controllen = 0;
			continue;
		}
		if (sent <= 0)
			return FALSE;
		/* The descriptor went with the first bytes. */
		message.msg_control = NULL;
		message.msg_controllen = 0;
		while (first < (int)G_N_ELEMENTS(parts) &&
		       (size_t)sent >= parts[first].iov_len) {
			sent -= parts[first].iov_len;
			first++;
		}
		if (first < (int)G_N_ELEMENTS(parts)) {
			parts[first].iov_base =
				(char *)parts[first].iov_base + sent;
			parts[first].iov_len -= sent;
		}
	}
	return TRUE;
}

static int staged_upload_fd = -1;

void line_media_stage_fd(int fd)
{
	if (staged_upload_fd >= 0)
		close(staged_upload_fd);
	staged_upload_fd = fd;
}

int line_media_take_staged_fd(void)
{
	int fd = staged_upload_fd;

	staged_upload_fd = -1;
	return fd;
}

void line_media_clear_staged_fd(void)
{
	if (staged_upload_fd >= 0) {
		close(staged_upload_fd);
		staged_upload_fd = -1;
	}
}

void line_wire_reader_init(struct line_wire_reader *r)
{
	r->input = g_string_new(NULL);
	r->pending_fd = -1;
}

void line_wire_reader_clear(struct line_wire_reader *r)
{
	if (r->input)
		g_string_free(r->input, TRUE);
	r->input = NULL;
	if (r->pending_fd >= 0)
		close(r->pending_fd);
	r->pending_fd = -1;
}

/* Keeps at most one passed descriptor and closes the rest. */
static int passed_fd(struct msghdr *message, ssize_t got)
{
	int fd = -1;

	for (struct cmsghdr *cm = CMSG_FIRSTHDR(message); cm;
	     cm = CMSG_NXTHDR(message, cm)) {
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
			continue;
		gsize count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);

		for (gsize i = 0; i < count; i++) {
			int passed;

			memcpy(&passed, CMSG_DATA(cm) + i * sizeof(int),
			       sizeof(passed));
			if (fd < 0 && got > 0)
				fd = passed;
			else
				close(passed);
		}
	}
	if (message->msg_flags & MSG_CTRUNC && fd >= 0) {
		close(fd);
		fd = -1;
	}
	return fd;
}

ssize_t line_wire_read(struct line_wire_reader *r, int socket, char *buf,
		       gsize size,
		       void (*line)(char *text, int fd, gpointer data),
		       gpointer data)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(4 * sizeof(int))];
	} control;
	struct iovec iov = { buf, size };
	struct msghdr message = { .msg_iov = &iov,
				  .msg_iovlen = 1,
				  .msg_control = control.bytes,
				  .msg_controllen = sizeof(control.bytes) };
	ssize_t got = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);

	if (got < 0 && errno == ENOTSOCK) {
		got = read(socket, buf, size);
		message.msg_controllen = 0;
		message.msg_flags = 0;
	}
	if (got <= 0)
		return got;
	int fd = message.msg_controllen ? passed_fd(&message, got) : -1;
	ssize_t fd_line = -1;

	if (fd >= 0) {
		/* The last line that starts here: skip its own newline. */
		ssize_t i = got - 1;

		if (buf[i] == '\n')
			i--;
		while (i >= 0 && buf[i] != '\n')
			i--;
		fd_line = i + 1;
		/* A lease must start its own line. */
		if (!fd_line && r->input->len) {
			close(fd);
			fd = -1;
		}
	}
	gsize start = 0;

	for (gsize i = 0; i < (gsize)got; i++) {
		if (buf[i] != '\n')
			continue;
		int line_fd = -1;

		if (fd >= 0 && (ssize_t)start == fd_line) {
			line_fd = fd;
			fd = -1;
		} else if (!start && r->pending_fd >= 0) {
			line_fd = r->pending_fd;
			r->pending_fd = -1;
		}
		g_string_append_len(r->input, buf + start, (gssize)(i - start));
		line(g_string_free(r->input, FALSE), line_fd, data);
		r->input = g_string_new(NULL);
		start = i + 1;
	}
	if (fd >= 0) {
		/* The lease's line continues in the next read. */
		if (r->pending_fd >= 0)
			close(r->pending_fd);
		r->pending_fd = fd;
	}
	g_string_append_len(r->input, buf + start, got - (ssize_t)start);
	return got;
}
