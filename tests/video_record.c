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

static void check_clip(guint vendor, const char *encoder)
{
	g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree char *ffprobe = g_find_program_in_path("ffprobe");

	if (!ffmpeg || !ffprobe) {
		g_test_skip("Synthetic video test requires FFmpeg and ffprobe");
		return;
	}
	struct recording_result result = { 0 };

	g_autoptr(GError) error = NULL;
	g_assert_true(
		line_record_start_synthetic(vendor, done, &result, &error));
	g_assert_no_error(error);
	g_assert_false(
		line_record_start_synthetic(vendor, done, &result, &error));
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
			       "stream=codec_name:stream_tags=encoder",
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
	if (encoder)
		g_assert_nonnull(strstr(out, encoder));
	line_record_discard(result.path);
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	g_cond_clear(&result.cond);
	g_mutex_clear(&result.lock);
}

static void record_clip(void)
{
	check_clip(0, NULL);
}

/* The driver can disappear or refuse a session after the first probe. */
static void fallback_clip(gconstpointer data)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 15 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed();
		return;
	}
	g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree char *ffprobe = g_find_program_in_path("ffprobe");

	if (!ffmpeg || !ffprobe)
		return;
	g_autofree char *dir = g_dir_make_tmp("avernella-encoder-XXXXXX", NULL);
	g_autofree char *shim = g_build_filename(dir, "ffmpeg", NULL);
	g_autofree char *log = g_build_filename(dir, "attempts", NULL);
	g_autofree char *exe = g_shell_quote(ffmpeg);
	g_autofree char *attempts = g_shell_quote(log);
	g_autofree char *script = g_strdup_printf(
		"#!/bin/sh\n"
		"case \" $* \" in\n"
		"*' -f null '*) echo probe >> %s; exit %d;;\n"
		"*' h264_nvenc '*) echo hardware >> %s; echo 'h264_nvenc: initialization failure' >&2; exit 1;;\n"
		"esac\n"
		"echo software >> %s\nexec %s \"$@\"\n",
		attempts, !data, attempts, attempts, exe);
	g_autofree char *path = g_strconcat(dir, ":", g_getenv("PATH"), NULL);

	g_assert_true(g_file_set_contents(shim, script, -1, NULL));
	g_assert_cmpint(g_chmod(shim, 0700), ==, 0);
	g_unsetenv("AVERNELLA_RENDER_NODE");
	g_unsetenv("LINE_DISABLE_GPU");
	g_setenv("PATH", path, TRUE);
	check_clip(0x10de, "libx264");
	g_autofree char *calls = NULL;

	g_assert_true(g_file_get_contents(log, &calls, NULL, NULL));
	g_assert_cmpstr(calls, ==,
			data ? "probe\nhardware\nsoftware\n" :
			       "probe\nsoftware\n");
	g_unlink(shim);
	g_unlink(log);
	g_rmdir(dir);
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
	g_assert_true(line_record_start_synthetic(0, done, &result, &error));
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
	g_test_add_data_func("/video-record/unavailable-hardware", NULL,
			     fallback_clip);
	g_test_add_data_func("/video-record/encoder-open-failure",
			     GINT_TO_POINTER(1), fallback_clip);
	return g_test_run();
}
