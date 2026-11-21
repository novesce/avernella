// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_record.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

struct recording_result {
	GMutex lock;
	GCond cond;
	char *path, *error;
	gboolean called;
};

static void done(char *path, char *error, gpointer data)
{
	struct recording_result *r = data;

	g_mutex_lock(&r->lock);
	r->path = path;
	r->error = error;
	r->called = TRUE;
	g_cond_broadcast(&r->cond);
	g_mutex_unlock(&r->lock);
}

static void record_clip(void)
{
	g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree char *ffprobe = g_find_program_in_path("ffprobe");

	if (!ffmpeg || !ffprobe) {
		g_test_skip("Synthetic video test requires FFmpeg and ffprobe");
		return;
	}
	struct recording_result result = { 0 };

	g_autoptr(GError) error = NULL;
	g_assert_true(line_record_start_synthetic(done, &result, &error));
	g_assert_no_error(error);
	g_assert_false(line_record_start_synthetic(done, &result, &error));
	g_assert_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED);
	g_clear_error(&error);
	g_usleep(2 * G_USEC_PER_SEC);
	line_record_stop();
	g_mutex_lock(&result.lock);
	gint64 limit = g_get_monotonic_time() + 12 * G_USEC_PER_SEC;

	while (!result.called)
		g_assert_true(
			g_cond_wait_until(&result.cond, &result.lock, limit));
	g_mutex_unlock(&result.lock);
	line_record_shutdown();
	g_assert_cmpstr(result.error, ==, NULL);
	g_assert_nonnull(result.path);
	g_autofree char *dir = g_path_get_dirname(result.path);
	struct stat st;

	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	char *const argv[] = { ffprobe,
			       "-v",
			       "error",
			       "-show_entries",
			       "stream=codec_name",
			       "-of",
			       "csv=p=0",
			       result.path,
			       NULL };
	g_autofree char *out = NULL;
	int status;

	g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
				   &out, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(out, "h264"));
	g_assert_nonnull(strstr(out, "aac"));
	line_record_discard(result.path);
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	g_cond_clear(&result.cond);
	g_mutex_clear(&result.lock);
}

static void cancel_clip(void)
{
	g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");

	if (!ffmpeg) {
		g_test_skip("Synthetic video test requires FFmpeg");
		return;
	}
	struct recording_result result = { 0 };

	g_autoptr(GError) error = NULL;
	g_assert_true(line_record_start_synthetic(done, &result, &error));
	g_assert_no_error(error);
	line_record_cancel();
	line_record_shutdown();
	g_assert_false(result.called);
	g_mutex_clear(&result.lock);
	g_cond_clear(&result.cond);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/video-record/synthetic-camera-microphone",
			record_clip);
	g_test_add_func("/video-record/cancel", cancel_clip);
	return g_test_run();
}
