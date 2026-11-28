// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_io.h"
#include "camera.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void anonymous_image(void)
{
	g_autoptr(GHashTable) leases = line_leases_new();
	g_autoptr(GBytes) image = g_bytes_new_static("fixture", 7);
	g_autoptr(GError) e = NULL;
	g_autoptr(J) result = line_media_lease(leases, image, &e);
	g_assert_no_error(e);
	/* No path into the worker's /proc: the UI gets a descriptor. */
	g_assert_null(strfield(result, "path"));
	const char *lease = strfield(result, "lease");
	int fd = line_media_lease_open(leases, lease);

	g_assert_cmpint(fd, >=, 0);
	int seals = fcntl(fd, F_GET_SEALS);

	g_assert_cmpint(seals & F_SEAL_WRITE, ==, F_SEAL_WRITE);
	g_assert_cmpint(write(fd, "x", 1), ==, -1);
	g_assert_cmpint(errno, ==, EBADF);
	char bytes[7];

	/* A fresh open file: it reads from the start. */
	g_assert_cmpint(read(fd, bytes, 7), ==, 7);
	g_assert_cmpmem(bytes, 7, "fixture", 7);
	g_assert_cmpint(line_media_lease_open(leases, "unknown"), ==, -1);
	g_assert_true(g_hash_table_remove(leases, lease));
	g_assert_cmpint(line_media_lease_open(leases, lease), ==, -1);
	/* The UI's descriptor outlives the worker's release. */
	g_assert_cmpint(pread(fd, bytes, 7, 0), ==, 7);
	close(fd);
}

/* A worker that is not dumpable can still reopen its own leases. */
static void lease_not_dumpable(void)
{
	if (g_test_subprocess()) {
		g_autoptr(GHashTable) leases = line_leases_new();
		g_autoptr(GBytes) image = g_bytes_new_static("pixels", 6);
		g_autoptr(J) result = line_media_lease(leases, image, NULL);

		g_assert_cmpint(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0), ==, 0);
		int fd = line_media_lease_open(leases,
					       strfield(result, "lease"));
		char bytes[6];

		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(read(fd, bytes, 6), ==, 6);
		g_assert_cmpmem(bytes, 6, "pixels", 6);
		close(fd);
		return;
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
}

static void video_xdg_lease(gconstpointer data)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	g_autoptr(GError) error = NULL;
	g_autofree char *runtime =
		g_dir_make_tmp("avernella-lease-XXXXXX", &error);
	g_autoptr(GHashTable) leases = line_leases_new();
	g_autoptr(GBytes) bytes = g_bytes_new_static("video", 5);
	struct stat st;

	g_assert_no_error(error);
	if (GPOINTER_TO_INT(data)) {
		g_assert_true(g_setenv("XDG_RUNTIME_DIR", "invalid", TRUE));
		g_assert_true(g_setenv("XDG_CACHE_HOME", runtime, TRUE));
	} else {
		g_assert_true(g_setenv("XDG_RUNTIME_DIR", runtime, TRUE));
	}
	g_autoptr(J) result = line_media_video_lease(leases, bytes, &error);

	g_assert_no_error(error);
	g_assert_null(strfield(result, "path"));
	g_assert_cmpint(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0), ==, 0);
	const char *lease = strfield(result, "lease");
	int fd = line_media_lease_open(leases, lease);
	char proc[64], target[1024], payload[5];

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(fstat(fd, &st), ==, 0);
	g_assert_cmpint(st.st_nlink, ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0400);
	g_assert_cmpint(fcntl(fd, F_GETFL) & O_ACCMODE, ==, O_RDONLY);
	g_snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(proc, target, sizeof(target) - 1);

	g_assert_cmpint(n, >, 0);
	target[n] = '\0';
	g_autofree char *dir = g_build_filename(runtime, "avernella", NULL);
	g_autofree char *resolved = realpath(dir, NULL);

	g_assert_nonnull(resolved);
	g_assert_true(g_str_has_prefix(target, resolved));
	g_assert_true(g_hash_table_remove(leases, lease));
	g_assert_cmpint(read(fd, payload, sizeof(payload)), ==,
			sizeof(payload));
	g_assert_cmpmem(payload, sizeof(payload), "video", 5);
	close(fd);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
	g_assert_cmpint(g_rmdir(runtime), ==, 0);
}

struct wire_line {
	char *text;
	int fd;
};

static void collect(char *text, int fd, gpointer data)
{
	struct wire_line line = { text, fd };

	g_array_append_val(data, line);
}

static int memfd_with(const char *text)
{
	int fd = memfd_create("wire", MFD_CLOEXEC);

	g_assert_cmpint(write(fd, text, strlen(text)), ==,
			(ssize_t)strlen(text));
	return fd;
}

static void expect_fd(int fd, const char *text)
{
	char bytes[64] = { 0 };

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(pread(fd, bytes, sizeof(bytes) - 1, 0), ==,
			(ssize_t)strlen(text));
	g_assert_cmpstr(bytes, ==, text);
	close(fd);
}

/* Lines in order; each lease descriptor stays with its own line. */
static void wire_leases(gconstpointer data)
{
	gsize buffer = GPOINTER_TO_SIZE(data);
	static const char *const lines[] = {
		"{\"id\":1,\"result\":true}",
		"{\"id\":2,\"result\":{\"lease\":\"a\"}}",
		"{\"event\":\"changed\"}",
		"{\"id\":3,\"result\":{\"lease\":\"b\"}}",
		"{\"id\":4,\"result\":{\"lease\":\"c\"}}",
		"{\"id\":5,\"result\":false}",
	};
	static const char *const contents[] = { NULL,	  "first", NULL,
						"second", "third", NULL };
	int pair[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
				   pair),
			==, 0);
	for (gsize i = 0; i < G_N_ELEMENTS(lines); i++) {
		int fd = contents[i] ? memfd_with(contents[i]) : -1;

		g_assert_true(line_wire_send(pair[0], lines[i],
					     strlen(lines[i]), fd));
		if (fd >= 0)
			close(fd);
	}
	close(pair[0]);
	g_autoptr(GArray) got =
		g_array_new(FALSE, FALSE, sizeof(struct wire_line));
	g_autofree char *buf = g_malloc(buffer);
	struct line_wire_reader reader;

	line_wire_reader_init(&reader);
	while (line_wire_read(&reader, pair[1], buf, buffer, collect, got) > 0)
		;
	line_wire_reader_clear(&reader);
	close(pair[1]);
	g_assert_cmpuint(got->len, ==, G_N_ELEMENTS(lines));
	for (guint i = 0; i < got->len; i++) {
		struct wire_line *line =
			&g_array_index(got, struct wire_line, i);

		g_assert_cmpstr(line->text, ==, lines[i]);
		if (contents[i])
			expect_fd(line->fd, contents[i]);
		else
			g_assert_cmpint(line->fd, ==, -1);
		g_free(line->text);
	}
}

/* Standard output that is not a socket still gets the line. */
static void wire_not_socket(void)
{
	int pipefd[2], fd = memfd_with("x");
	char bytes[16] = { 0 };

	g_assert_cmpint(pipe2(pipefd, O_CLOEXEC), ==, 0);
	g_assert_true(line_wire_send(pipefd[1], "{}", 2, fd));
	g_assert_cmpint(read(pipefd[0], bytes, sizeof(bytes)), ==, 3);
	g_assert_cmpstr(bytes, ==, "{}\n");
	close(fd);
	close(pipefd[0]);
	close(pipefd[1]);
}

static void preview_descriptor(gconstpointer data)
{
	int pair[2];
	const guint8 y[] = { 1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0 };
	const guint8 u[] = { 10, 11, 0, 12, 13, 0 };
	const guint8 v[] = { 14, 15, 0, 16, 17, 0 };
	const guint8 expected[] = { 1,	2,  3,	4,  5,	6,  7,	8, 9,
				    10, 11, 12, 13, 14, 15, 16, 17 };
	const struct line_camera_frame pixels = { .width = 3,
						  .height = 3,
						  .plane = { y, u, v },
						  .stride = { 4, 3, 3 } };
	const struct line_preview_frame sent = {
		.source = GPOINTER_TO_UINT(data),
		.camera_enabled = TRUE,
		.width = 3,
		.height = 3,
		.rotation = 90,
		.decoded_at = 12,
		.chat = "u11111111111111111111111111111111"
	};
	struct line_preview_frame got;
	guint8 packed[sizeof(expected)];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
				   pair),
			==, 0);
	g_assert_true(line_media_send_frame(pair[0], &sent, &pixels));
	int fd = line_media_recv_frame(pair[1], &got);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpstr(got.chat, ==, sent.chat);
	g_assert_cmpuint(got.source, ==, sent.source);
	g_assert_true(got.camera_enabled);
	g_assert_cmpuint(got.width, ==, sent.width);
	g_assert_cmpuint(got.height, ==, sent.height);
	g_assert_cmpint(got.decoded_at, ==, sent.decoded_at);
	g_assert_cmpuint(got.rotation, ==, sent.rotation);
	g_assert_true(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	g_assert_true(fcntl(fd, F_GET_SEALS) & F_SEAL_WRITE);
	g_assert_cmpint(read(fd, packed, sizeof(packed)), ==, sizeof(packed));
	g_assert_cmpmem(packed, sizeof(packed), expected, sizeof(expected));
	g_assert_cmpint(line_media_recv_frame(pair[1], &got), ==, -1);
	close(fd);
	close(pair[0]);
	close(pair[1]);
}

/* Cross several writev batches with padding, reversed rows and packed chroma. */
static void preview_rows(void)
{
	enum {
		W = 64,
		H = 385,
		STRIDE = W + 16,
		CH = (H + 1) / 2
	};
	guint8 *y = g_malloc(STRIDE * H);
	guint8 *uv = g_malloc(W * CH);
	g_autofree guint8 *expected = g_malloc(line_camera_i420_size(W, H));
	g_autofree guint8 *received = g_malloc(line_camera_i420_size(W, H));
	struct line_camera_frame image = {
		.width = W,
		.height = H,
		.plane = { y + STRIDE * (H - 1), uv, uv + W / 2 * CH },
		.stride = { -STRIDE, W / 2, W / 2 },
	};
	struct line_preview_frame msg = {
		.source = LINE_PREVIEW_REMOTE,
		.width = W,
		.height = H,
	};
	int pair[2];

	for (guint i = 0; i < STRIDE * H; i++)
		y[i] = i % 193;
	for (guint i = 0; i < W * CH; i++)
		uv[i] = i % 211;
	line_camera_pack_i420(expected, &image);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
				   pair),
			==, 0);
	g_assert_true(line_media_send_frame(pair[0], &msg, &image));
	int fd = line_media_recv_frame(pair[1], &msg);
	size_t size = line_camera_i420_size(W, H);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(read(fd, received, size), ==, size);
	g_assert_cmpmem(received, size, expected, size);
	close(fd);
	msg.source = LINE_PREVIEW_COUNT;
	g_assert_true(line_media_send_frame(pair[0], &msg, &image));
	g_assert_cmpint(line_media_recv_frame(pair[1], &msg), ==, -1);
	close(pair[0]);
	close(pair[1]);
	g_free(uv);
	g_free(y);
}

static void preview_backpressure(void)
{
	const guint8 pixels[6] = { 0 };
	guint8 padding[2048] = { 0 };
	struct line_camera_frame image;
	struct line_preview_frame msg = { .width = 2, .height = 2 };
	int pair[2], fd;
	guint queued = 0;
	struct iovec iov = { .iov_base = padding, .iov_len = sizeof(padding) };
	struct msghdr dummy = { .msg_iov = &iov, .msg_iovlen = 1 };

	line_camera_i420_frame(&image, pixels, 2, 2);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
				   pair),
			==, 0);
	while (sendmsg(pair[0], &dummy, MSG_DONTWAIT) >= 0)
		queued++;
	g_assert_cmpint(errno, ==, EAGAIN);
	g_assert_cmpuint(queued, >, 0);
	g_assert_true(line_media_send_frame(pair[0], &msg, &image));
	for (guint i = 0; i < queued; i++)
		g_assert_cmpint(recvmsg(pair[1], &dummy, MSG_DONTWAIT), ==,
				sizeof(padding));
	g_assert_cmpint(line_media_recv_frame(pair[1], &msg), ==, -1);
	g_assert_true(line_media_send_frame(pair[0], &msg, &image));
	fd = line_media_recv_frame(pair[1], &msg);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	close(pair[1]);
	g_assert_false(line_media_send_frame(pair[0], &msg, &image));
	close(pair[0]);
}

static void preview_dmabuf(gconstpointer data)
{
	gboolean real = GPOINTER_TO_INT(data);
	struct line_preview_frame frame = { .width = 16,
					    .height = 16,
					    .source = LINE_PREVIEW_REMOTE };
	struct line_dmabuf_layout layout = { .width = 16,
					     .height = 16,
					     .offset = { 0, 256 },
					     .stride = { 16, 16 } };
	int fd = memfd_create("avernella-test-plane", MFD_ALLOW_SEALING);
	int pair[2], lease[2], release_fd;
	char byte;

	g_assert_cmpint(fd, >=, 0);
	if (real) {
		int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);

		if (device < 0) {
			close(fd);
			g_test_skip(
				"No user-accessible DMA-BUF test allocator");
			return;
		}
		struct udmabuf_create create = { .memfd = fd,
						 .size = getpagesize() };
		g_assert_cmpint(ftruncate(fd, create.size), ==, 0);
		g_assert_cmpint(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK), ==, 0);
		int dma = ioctl(device, UDMABUF_CREATE, &create);

		close(device);
		close(fd);
		g_assert_cmpint(dma, >=, 0);
		fd = dma;
	}
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair), ==, 0);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, lease), ==, 0);
	g_assert_true(
		line_media_send_dmabuf(pair[0], &frame, &layout, fd, lease[1]));
	close(lease[1]);
	close(fd);
	fd = line_media_recv_preview(pair[1], &frame, &release_fd);
	if (real) {
		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(release_fd, >=, 0);
		g_assert_cmpint(recv(lease[0], &byte, 1, MSG_DONTWAIT), ==, -1);
		close(fd);
		close(release_fd);
	} else {
		g_assert_cmpint(fd, ==, -1);
		g_assert_cmpint(release_fd, ==, -1);
		g_assert_cmpint(read(lease[0], &byte, 1), ==, 1);
		g_assert_cmpint(byte, ==, 'R');
	}
	g_assert_cmpint(read(lease[0], &byte, 1), ==, 0);
	close(lease[0]);
	close(pair[0]);
	close(pair[1]);
}

static void save_file(void)
{
	g_autoptr(GError) e = NULL;
	g_autofree char *dir = g_dir_make_tmp("line-save-XXXXXX", &e);

	g_assert_no_error(e);
	g_autofree char *path = g_build_filename(dir, "photo", NULL),
			*alias = g_build_filename(dir, "alias", NULL);
	g_autoptr(GBytes) image = g_bytes_new_static("abc", 3),
			  replacement = g_bytes_new_static("def", 3);
	g_assert_true(line_save_media(path, image, FALSE, &e));
	g_assert_false(line_save_media(path, replacement, FALSE, &e));
	g_clear_error(&e);
	g_assert_cmpint(symlink(path, alias), ==, 0);
	g_assert_true(line_save_media(alias, replacement, TRUE, &e));
	g_autofree char *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, NULL, &e));
	g_assert_cmpstr(data, ==, "abc");
	g_assert_false(g_file_test(alias, G_FILE_TEST_IS_SYMLINK));
	g_remove(alias);
	g_remove(path);
	g_rmdir(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/media-io/sealed-lease", anonymous_image);
	g_test_add_func("/media-io/lease-not-dumpable", lease_not_dumpable);
	g_test_add_data_func("/media-io/video-runtime-lease", NULL,
			     video_xdg_lease);
	g_test_add_data_func("/media-io/video-cache-lease", GINT_TO_POINTER(1),
			     video_xdg_lease);
	g_test_add_data_func("/media-io/wire-leases", GSIZE_TO_POINTER(65536),
			     wire_leases);
	/* Every lease line split over several reads. */
	g_test_add_data_func("/media-io/wire-leases-split", GSIZE_TO_POINTER(7),
			     wire_leases);
	g_test_add_data_func("/media-io/wire-leases-byte", GSIZE_TO_POINTER(1),
			     wire_leases);
	g_test_add_func("/media-io/wire-not-socket", wire_not_socket);
	g_test_add_data_func("/media-io/direct-share",
			     GUINT_TO_POINTER(LINE_PREVIEW_SCREEN),
			     preview_descriptor);
	g_test_add_data_func("/media-io/direct-camera",
			     GUINT_TO_POINTER(LINE_PREVIEW_REMOTE),
			     preview_descriptor);
	g_test_add_data_func("/media-io/direct-self",
			     GUINT_TO_POINTER(LINE_PREVIEW_SELF),
			     preview_descriptor);
	g_test_add_func("/media-io/preview-rows", preview_rows);
	g_test_add_func("/media-io/preview-backpressure", preview_backpressure);
	g_test_add_data_func("/media-io/dma-descriptors", GINT_TO_POINTER(1),
			     preview_dmabuf);
	g_test_add_data_func("/media-io/dma-reject-memfd", NULL,
			     preview_dmabuf);
	g_test_add_func("/media-io/atomic-save", save_file);
	return g_test_run();
}
