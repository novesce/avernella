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
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

enum record_encoder {
	ENCODER_CPU,
	ENCODER_PROBE,
	ENCODER_NVENC
};

struct video_recording {
	GPid pid;
	int stderr_fd;
	char *path, *program;
	char **gpu_env;
	enum record_encoder encoder;
	gboolean synthetic;
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
			if (active->pid)
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

static gboolean wait_encoder(struct video_recording *job, int *status,
			     GString *diagnostic)
{
	struct pollfd pfd = { .fd = job->stderr_fd, .events = POLLIN };
	gint64 started = g_get_monotonic_time();
	gboolean reaped;

	for (;;) {
		read_diagnostic(job->stderr_fd, diagnostic);
		g_mutex_lock(&lock);
		pid_t r = waitpid(job->pid, status, WNOHANG);

		if (r == job->pid || (r < 0 && errno != EINTR)) {
			reaped = r == job->pid;
			g_spawn_close_pid(job->pid);
			job->pid = 0;
			g_mutex_unlock(&lock);
			break;
		}
		gint64 now = g_get_monotonic_time();

		if ((job->stop_time &&
		     now - job->stop_time > 4 * G_USEC_PER_SEC) ||
		    (job->encoder == ENCODER_PROBE &&
		     now - started > 5 * G_USEC_PER_SEC))
			kill(job->pid, SIGKILL);
		g_mutex_unlock(&lock);
		if (poll(&pfd, 1, 80) > 0 &&
		    (pfd.revents & (POLLHUP | POLLERR)))
			g_usleep(20000);
	}
	read_diagnostic(job->stderr_fd, diagnostic);
	close(job->stderr_fd);
	return reaped;
}

static gboolean spawn_encoder(struct video_recording *job,
			      enum record_encoder encoder, GError **error);

static gboolean nvenc_open_failed(const char *path, const char *diagnostic)
{
	struct stat st;

	if (!strstr(diagnostic, "h264_nvenc"))
		return FALSE;
	if (g_stat(path, &st))
		return errno == ENOENT;
	/* FFmpeg may already have written the MP4 file and empty media headers. */
	return st.st_size <= 48;
}

static gpointer monitor(gpointer data)
{
	struct video_recording *job = data;

	g_autoptr(GString) diagnostic = g_string_new(NULL);
	int status = 0;
	gboolean success, stopped;
	char *path = NULL, *error = NULL;

	for (;;) {
		success = wait_encoder(job, &status, diagnostic) &&
			  WIFEXITED(status) && !WEXITSTATUS(status);
		g_mutex_lock(&lock);
		stopped = job->stop_time != 0;
		enum record_encoder next = ENCODER_CPU;
		gboolean starting = job->encoder == ENCODER_PROBE;

		if (starting && success)
			next = ENCODER_NVENC;
		if (!stopped &&
		    (starting ||
		     (!success && job->encoder == ENCODER_NVENC &&
		      nvenc_open_failed(job->path, diagnostic->str)))) {
			g_autoptr(GError) issue = NULL;

			g_string_truncate(diagnostic, 0);
			if (spawn_encoder(job, next, &issue)) {
				g_mutex_unlock(&lock);
				continue;
			}
			g_string_assign(diagnostic, issue->message);
			success = FALSE;
		}
		active = NULL;
		g_mutex_unlock(&lock);
		break;
	}
	if (!job->cancelled) {
		if (job->encoder != ENCODER_PROBE &&
		    (success || stopped || job->encoder == ENCODER_NVENC) &&
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
	g_strfreev(job->gpu_env);
	g_free(job->program);
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

static gboolean select_nvenc(guint vendor, int render_minor, char ***gpu_env)
{
	const char *node = g_getenv("AVERNELLA_RENDER_NODE");
	struct stat st;

	if (!g_strcmp0(g_getenv("LINE_DISABLE_GPU"), "1"))
		return FALSE;
	if (node && *node) {
		if (stat(node, &st) || !S_ISCHR(st.st_mode) ||
		    major(st.st_rdev) != 226)
			return FALSE;
		render_minor = minor(st.st_rdev);
	} else if (vendor && vendor != 0x10de) {
		return FALSE;
	}
	if (render_minor < 0)
		return vendor == 0x10de ||
		       !access("/dev/nvidiactl", R_OK | W_OK);
	g_autofree char *device =
		g_strdup_printf("/sys/dev/char/226:%d/device", render_minor);
	g_autofree char *target = g_file_read_link(device, NULL);

	if (!target)
		return FALSE;
	g_autofree char *pci = g_path_get_basename(target);
	g_autofree char *info = g_build_filename("/proc/driver/nvidia/gpus",
						 pci, "information", NULL);
	g_autofree char *text = NULL;

	if (!g_file_get_contents(info, &text, NULL, NULL))
		return FALSE;
	char *label = strstr(text, "GPU UUID:");

	if (!label)
		return FALSE;
	char *uuid = label + strlen("GPU UUID:");

	uuid[strcspn(uuid, "\r\n")] = 0;
	g_strstrip(uuid);
	if (!g_str_has_prefix(uuid, "GPU-") ||
	    !g_uuid_string_is_valid(uuid + 4))
		return FALSE;
	/* Narrow an existing visibility mask only when it names this device. */
	const char *mask = g_getenv("CUDA_VISIBLE_DEVICES");

	if (mask) {
		g_auto(GStrv) devices = g_strsplit(mask, ",", -1);

		if (!g_strv_contains((const char *const *)devices, uuid)) {
			if (strcmp(mask, "0"))
				return FALSE;
			g_autoptr(GDir) gpus =
				g_dir_open("/proc/driver/nvidia/gpus", 0, NULL);

			if (!gpus || !g_dir_read_name(gpus) ||
			    g_dir_read_name(gpus))
				return FALSE;
		}
	}
	*gpu_env = g_environ_setenv(g_get_environ(), "CUDA_VISIBLE_DEVICES",
				    uuid, TRUE);
	return TRUE;
}

static gboolean spawn_encoder(struct video_recording *job,
			      enum record_encoder encoder, GError **error)
{
	const char *argv[80] = { job->program, "-hide_banner", "-loglevel",
				 "error",      "-nostdin",     "-y" };
	size_t arg = 6;

	if (encoder == ENCODER_PROBE) {
		const char *const inputs[] = {
			"-f",	     "lavfi",
			"-i",	     "color=c=black:s=320x240:r=24",
			"-frames:v", "1"
		};

		memcpy(argv + arg, inputs, sizeof(inputs));
		arg += G_N_ELEMENTS(inputs);
	} else if (job->synthetic) {
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
	const char *const mapping[] = {
		"-map",
		"0:v:0",
		"-map",
		"1:a:0",
		"-vf",
		"scale=w='min(1280,iw)':h='min(720,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2",
		"-r",
		"24",
	};
	const char *const cpu[] = { "-c:v",    "libx264",  "-threads:v", "2",
				    "-preset", "veryfast", "-crf",	 "25" };
	const char *const nvenc[] = { "-c:v",	 "h264_nvenc", "-gpu", "0",
				      "-preset", "p4",	       "-rc",  "vbr",
				      "-cq",	 "25",	       "-b:v", "0" };
	if (encoder != ENCODER_PROBE) {
		memcpy(argv + arg, mapping, sizeof(mapping));
		arg += G_N_ELEMENTS(mapping);
	}
	if (encoder == ENCODER_CPU) {
		memcpy(argv + arg, cpu, sizeof(cpu));
		arg += G_N_ELEMENTS(cpu);
	} else {
		memcpy(argv + arg, nvenc, sizeof(nvenc));
		arg += G_N_ELEMENTS(nvenc);
	}
	const char *const output[] = { "-pix_fmt",  "yuv420p",	  "-c:a",
				       "aac",	    "-b:a",	  "96k",
				       "-ac",	    "1",	  "-ar",
				       "48000",	    "-fs",	  "66000000",
				       "-movflags", "+faststart", "-f",
				       "mp4",	    job->path,	  NULL };
	if (encoder == ENCODER_PROBE) {
		const char *const sink[] = { "-pix_fmt", "yuv420p", "-f",
					     "null",	 "-",	    NULL };

		memcpy(argv + arg, sink, sizeof(sink));
	} else {
		memcpy(argv + arg, output, sizeof(output));
	}
	if (!g_spawn_async_with_pipes(
		    NULL, (char **)argv,
		    encoder == ENCODER_CPU ? NULL : job->gpu_env,
		    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
		    die_with_parent, NULL, &job->pid, NULL, NULL,
		    &job->stderr_fd, error))
		return FALSE;
	fcntl(job->stderr_fd, F_SETFL,
	      fcntl(job->stderr_fd, F_GETFL) | O_NONBLOCK);
	job->encoder = encoder;
	return TRUE;
}

static gboolean start_recording(gboolean synthetic, guint vendor,
				int render_minor, LineRecordDone done,
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
	struct video_recording *job = g_new0(struct video_recording, 1);

	job->path = g_build_filename(dir, "video.mp4", NULL);
	job->program = g_steal_pointer(&program);
	job->synthetic = synthetic;
	job->done = done;
	job->data = data;
	enum record_encoder stage =
		select_nvenc(vendor, render_minor, &job->gpu_env) ?
			ENCODER_PROBE :
			ENCODER_CPU;

	if (!spawn_encoder(job, stage, error)) {
		g_mutex_unlock(&lock);
		line_record_discard(job->path);
		g_free(job->program);
		g_strfreev(job->gpu_env);
		g_free(job);
		return FALSE;
	}
	active = job;
	jobs++;
	g_thread_unref(g_thread_new("camera-recorder", monitor, job));
	g_mutex_unlock(&lock);
	return TRUE;
}

gboolean line_record_start(guint vendor, int render_minor, LineRecordDone done,
			   gpointer data, GError **error)
{
	return start_recording(FALSE, vendor, render_minor, done, data, error);
}

#ifdef LINE_RECORD_TEST
gboolean line_record_start_synthetic(guint vendor, LineRecordDone done,
				     gpointer data, GError **error)
{
	return start_recording(TRUE, vendor, -1, done, data, error);
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
