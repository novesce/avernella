// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_play.h"
#include "media.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/mman.h>
#include <unistd.h>

struct playback_result {
	GMutex lock;
	GCond cond;
	char *error;
	gboolean done;
};

static void finished(const char *error, gpointer data)
{
	struct playback_result *result = data;

	g_mutex_lock(&result->lock);
	result->error = g_strdup(error);
	result->done = TRUE;
	g_cond_signal(&result->cond);
	g_mutex_unlock(&result->lock);
}

static void await_exit(struct playback_result *result)
{
	gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;

	g_mutex_lock(&result->lock);
	while (!result->done)
		g_assert_true(g_cond_wait_until(&result->cond, &result->lock,
						deadline));
	g_mutex_unlock(&result->lock);
}

static void clear_result(struct playback_result *result)
{
	g_free(result->error);
	g_cond_clear(&result->cond);
	g_mutex_clear(&result->lock);
}

static int clip_fd(const char *bytes, gsize size)
{
	int fd =
		memfd_create("line-test-clip", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	g_assert_cmpint(fd, >=, 0);
	for (gsize left = size; left;) {
		ssize_t n = write(fd, bytes + size - left, left);

		if (n < 0 && errno == EINTR)
			continue;
		g_assert_cmpint(n, >, 0);
		left -= n;
	}
	g_assert_cmpint(fcntl(fd, F_ADD_SEALS,
			      F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
				      F_SEAL_WRITE),
			==, 0);
	g_autofree char *path = g_strdup_printf("/proc/self/fd/%d", fd);
	int read_only = open(path, O_RDONLY | O_CLOEXEC);

	g_assert_cmpint(read_only, >=, 0);
	close(fd);
	return read_only;
}

static void play_mp4(void)
{
	g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree char *ffplay = g_find_program_in_path("ffplay");

	if (!ffmpeg || !ffplay) {
		g_test_skip("Synthetic playback requires FFmpeg and FFplay");
		return;
	}
	g_autoptr(GError) error = NULL;
	g_autofree char *dir =
		g_dir_make_tmp("line-playback-test.XXXXXX", &error);

	g_assert_no_error(error);
	g_autofree char *path = g_build_filename(dir, "fixture.mp4", NULL);
	char *const argv[] = { ffmpeg,	    "-hide_banner",
			       "-loglevel", "error",
			       "-f",	    "lavfi",
			       "-i",	    "color=blue:s=320x180:r=24",
			       "-f",	    "lavfi",
			       "-i",	    "sine=f=440:r=48000",
			       "-t",	    "1",
			       "-c:v",	    "libx264",
			       "-pix_fmt",  "yuv420p",
			       "-c:a",	    "aac",
			       path,	    NULL };
	int status;

	g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
				   NULL, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
	g_autofree char *bytes = NULL;
	gsize size;

	g_assert_true(g_file_get_contents(path, &bytes, &size, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
	struct playback_result result = { 0 };
	LineVideoPlayback *play = line_video_play(clip_fd(bytes, size), TRUE,
						  finished, &result, &error);

	g_assert_no_error(error);
	g_assert_nonnull(play);
	await_exit(&result);
	g_assert_null(result.error);
	line_video_play_free(play);
	clear_result(&result);

	struct playback_result stopped = { 0 };

	play = line_video_play(clip_fd(bytes, size), FALSE, finished, &stopped,
			       &error);
	g_assert_no_error(error);
	g_assert_nonnull(play);
	line_video_play_free(play);
	g_assert_false(stopped.done);
	clear_result(&stopped);
}

static void invalid_clip(void)
{
	g_autofree char *ffplay = g_find_program_in_path("ffplay");

	if (!ffplay) {
		g_test_skip("Synthetic playback requires FFplay");
		return;
	}
	const char *const malformed[] = {
		"Not a video",
		"#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nno-such-file.ts\n#EXT-X-ENDLIST\n"
	};

	for (guint i = 0; i < G_N_ELEMENTS(malformed); i++) {
		struct playback_result result = { 0 };

		g_autoptr(GError) error = NULL;
		int fd = clip_fd(malformed[i], strlen(malformed[i]));
		LineVideoPlayback *play =
			line_video_play(fd, FALSE, finished, &result, &error);

		g_assert_no_error(error);
		g_assert_nonnull(play);
		await_exit(&result);
		g_assert_nonnull(result.error);
		line_video_play_free(play);
		clear_result(&result);
	}
}

static void invalid_descriptor(void)
{
	g_autoptr(GError) error = NULL;
	int fd = clip_fd("", 0);

	g_assert_null(line_video_play(fd, FALSE, NULL, NULL, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
	g_assert_cmpint(errno, ==, EBADF);
	g_clear_error(&error);
	fd = memfd_create("line-oversize-test", MFD_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(ftruncate(fd, (off_t)LINE_MEDIA_LIMIT + 1), ==, 0);
	g_autofree char *path = g_strdup_printf("/proc/self/fd/%d", fd);
	int read_only = open(path, O_RDONLY | O_CLOEXEC);

	g_assert_cmpint(read_only, >=, 0);
	close(fd);
	g_assert_null(line_video_play(read_only, FALSE, NULL, NULL, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void missing_player(void)
{
	if (g_test_subprocess()) {
		g_setenv("PATH", "/dev/null", TRUE);
		g_autoptr(GError) error = NULL;
		int fd = clip_fd("x", 1);

		g_assert_null(line_video_play(fd, FALSE, NULL, NULL, &error));
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
		g_assert_nonnull(strstr(error->message, "FFplay"));
		g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
		g_assert_cmpint(errno, ==, EBADF);
		return;
	}
	g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC,
			       G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
}

int main(int argc, char **argv)
{
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/video-play/seekable-private-mp4-and-cancellation",
			play_mp4);
	g_test_add_func("/video-play/invalid-media-and-playlist", invalid_clip);
	g_test_add_func("/video-play/invalid-descriptor", invalid_descriptor);
	g_test_add_func("/video-play/ffplay-missing", missing_player);
	return g_test_run();
}
