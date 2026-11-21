// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_io.h"
#include "camera.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void anonymous_image(void)
{
	g_autoptr(GHashTable) leases = line_leases_new();
	g_autoptr(GBytes) image = g_bytes_new_static("fixture", 7);
	g_autoptr(GError) e = NULL;
	g_autoptr(J) result = line_media_lease(leases, image, &e);
	g_assert_no_error(e);
	const char *path = strfield(result, "path");
	int fd = open(path, O_RDWR | O_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(write(fd, "x", 1), ==, -1);
	g_assert_cmpint(errno, ==, EPERM);
	char bytes[7];

	g_assert_cmpint(read(fd, bytes, 7), ==, 7);
	g_assert_cmpmem(bytes, 7, "fixture", 7);
	g_assert_true(g_hash_table_remove(leases, strfield(result, "lease")));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	close(fd);
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
	g_test_add_func("/media-io/atomic-save", save_file);
	return g_test_run();
}
