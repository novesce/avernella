// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_record.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct video_recording {
	GPid pid;
	int stderr_fd;
	char *path;
	LineRecordDone done;
	gpointer data;
	gboolean cancelled;
	gint64 stop_time;
};

static GMutex lock;
static GCond idle;
static struct video_recording *active;
static unsigned int jobs;

enum {
	CLIP_MIB = 64,
	MAX_DIAGNOSTIC = 2048
};

void line_record_discard(char *path)
{
	if (!path)
		return;
	g_autofree char *dir = g_path_get_dirname(path);

	g_unlink(path);
	g_rmdir(dir);
	g_free(path);
}

static void request_stop(gboolean cancel)
{
	g_mutex_lock(&lock);
	if (active) {
		active->cancelled |= cancel;
		if (!active->stop_time) {
			active->stop_time = g_get_monotonic_time();
			kill(active->pid, SIGINT);
		}
	}
	g_mutex_unlock(&lock);
}

void line_record_stop(void)
{
	request_stop(FALSE);
}

void line_record_cancel(void)
{
	request_stop(TRUE);
}

/*
 * FFmpeg can return 255 after flushing a valid MP4 in response to SIGINT.
 * Accept a finalized container with a track and nonempty media payload.
 */
static gboolean finalized(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return FALSE;
	struct stat st;
	gboolean ok = !fstat(fd, &st) && st.st_size > 0 &&
		      st.st_size <= (off_t)CLIP_MIB * 1024 * 1024;
	gboolean media = FALSE, moov = FALSE;
	off_t at = 0;

	while (ok && at < st.st_size) {
		guint8 box[8];

		if (pread(fd, box, sizeof(box), at) != sizeof(box))
			break;
		guint32 size;

		memcpy(&size, box, sizeof(size));
		size = GUINT32_FROM_BE(size);
		/* Files up to our attachment limit use short box sizes. */
		if (size < 8 || (off_t)size > st.st_size - at)
			break;
		if (!memcmp(box + 4, "mdat", 4) && size > 8)
			media = TRUE;
		if (!memcmp(box + 4, "moov", 4) && size > 8)
			moov = TRUE;
		at += size;
	}
	close(fd);
	return ok && media && moov && at == st.st_size;
}

static void read_diagnostic(int fd, GString *log)
{
	char buffer[512];
	ssize_t n;

	while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
		if (log->len < MAX_DIAGNOSTIC)
			g_string_append_len(log, buffer,
					    MIN((gsize)n,
						MAX_DIAGNOSTIC - log->len));
	}
}

static gpointer monitor(gpointer data)
{
	struct video_recording *job = data;

	g_autoptr(GString) diagnostic = g_string_new(NULL);
	struct pollfd pfd = { .fd = job->stderr_fd, .events = POLLIN };
	int status = 0;
	gboolean reaped = FALSE, stopped;

	for (;;) {
		read_diagnostic(job->stderr_fd, diagnostic);
		g_mutex_lock(&lock);
		pid_t r = waitpid(job->pid, &status, WNOHANG);

		if (r == job->pid || (r < 0 && errno != EINTR)) {
			reaped = r == job->pid;
			stopped = job->stop_time != 0;
			active = NULL;
			g_mutex_unlock(&lock);
			break;
		}
		if (job->stop_time && g_get_monotonic_time() - job->stop_time >
					      4 * G_USEC_PER_SEC)
			kill(job->pid, SIGKILL);
		g_mutex_unlock(&lock);
		if (poll(&pfd, 1, 80) > 0 &&
		    (pfd.revents & (POLLHUP | POLLERR)))
			g_usleep(20000);
	}
	read_diagnostic(job->stderr_fd, diagnostic);
	close(job->stderr_fd);
	g_spawn_close_pid(job->pid);
	char *path = NULL, *error = NULL;

	if (!job->cancelled) {
		if (reaped && WIFEXITED(status) &&
		    (WEXITSTATUS(status) == 0 || stopped) &&
		    finalized(job->path))
			path = g_steal_pointer(&job->path);
		else {
			g_strchomp(diagnostic->str);
			g_autofree char *clean =
				g_utf8_make_valid(diagnostic->str, -1);
			error = *clean ?
					g_strdup_printf(
						"Could not record video. %s",
						clean) :
					g_strdup(
						"No video was recorded. Check the camera and microphone, then try again.");
		}
	}
	line_record_discard(job->path);
	if (!job->cancelled)
		job->done(path, error, job->data);
	g_free(job);
	g_mutex_lock(&lock);
	jobs--;
	g_cond_broadcast(&idle);
	g_mutex_unlock(&lock);
	return NULL;
}

static void die_with_parent(gpointer unused)
{
	(void)unused;
	prctl(PR_SET_PDEATHSIG, SIGTERM);
}

static gboolean start_recording(gboolean synthetic, LineRecordDone done,
				gpointer data, GError **error)
{
	g_return_val_if_fail(done, FALSE);
	g_autofree char *program = g_find_program_in_path("ffmpeg");

	if (!program) {
		g_set_error_literal(
			error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT,
			"Video recording needs FFmpeg with H.264, AAC, V4L2 and PulseAudio support.");
		return FALSE;
	}
	g_mutex_lock(&lock);
	if (active) {
		g_mutex_unlock(&lock);
		g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
				    "A recording is already active.");
		return FALSE;
	}
	g_autofree char *dir = g_dir_make_tmp("line-camera-XXXXXX", error);

	if (!dir) {
		g_mutex_unlock(&lock);
		return FALSE;
	}
	char *path = g_build_filename(dir, "video.mp4", NULL);
	struct video_recording *job = g_new0(struct video_recording, 1);

	job->path = path;
	job->done = done;
	job->data = data;
	const char *argv[80] = { program, "-hide_banner", "-loglevel",
				 "error", "-nostdin",	  "-y" };
	size_t arg = 6;

	if (synthetic) {
		const char *const inputs[] = {
			"-re",
			"-f",
			"lavfi",
			"-i",
			"testsrc2=size=160x120:rate=12",
			"-re",
			"-f",
			"lavfi",
			"-i",
			"sine=frequency=440:sample_rate=48000"
		};
		memcpy(argv + arg, inputs, sizeof(inputs));
		arg += G_N_ELEMENTS(inputs);
	} else {
		const char *const inputs[] = { "-thread_queue_size",
					       "16",
					       "-f",
					       "v4l2",
					       "-i",
					       "/dev/video0",
					       "-thread_queue_size",
					       "32",
					       "-f",
					       "pulse",
					       "-i",
					       "default" };
		memcpy(argv + arg, inputs, sizeof(inputs));
		arg += G_N_ELEMENTS(inputs);
	}
	const char *const output[] = {
		"-map",
		"0:v:0",
		"-map",
		"1:a:0",
		"-vf",
		"scale=w='min(1280,iw)':h='min(720,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2",
		"-r",
		"24",
		"-c:v",
		"libx264",
		"-threads:v",
		"2",
		"-preset",
		"veryfast",
		"-crf",
		"25",
		"-pix_fmt",
		"yuv420p",
		"-c:a",
		"aac",
		"-b:a",
		"96k",
		"-ac",
		"1",
		"-ar",
		"48000",
		"-fs",
		"66000000",
		"-movflags",
		"+faststart",
		"-f",
		"mp4",
		path,
		NULL
	};
	memcpy(argv + arg, output, sizeof(output));
	gboolean spawned = g_spawn_async_with_pipes(
		NULL, (char **)argv, NULL,
		G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
		die_with_parent, NULL, &job->pid, NULL, NULL, &job->stderr_fd,
		error);
	if (!spawned) {
		g_mutex_unlock(&lock);
		line_record_discard(path);
		g_free(job);
		return FALSE;
	}
	fcntl(job->stderr_fd, F_SETFL,
	      fcntl(job->stderr_fd, F_GETFL) | O_NONBLOCK);
	active = job;
	jobs++;
	g_thread_unref(g_thread_new("camera-recorder", monitor, job));
	g_mutex_unlock(&lock);
	return TRUE;
}

gboolean line_record_start(LineRecordDone done, gpointer data, GError **error)
{
	return start_recording(FALSE, done, data, error);
}

#ifdef LINE_RECORD_TEST
gboolean line_record_start_synthetic(LineRecordDone done, gpointer data,
				     GError **error)
{
	return start_recording(TRUE, done, data, error);
}
#endif

void line_record_shutdown(void)
{
	line_record_cancel();
	g_mutex_lock(&lock);
	while (jobs)
		g_cond_wait(&idle, &lock);
	g_mutex_unlock(&lock);
}
