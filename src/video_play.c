// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_play.h"
#include "media.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum playback_mode {
	PLAY_HW_DECODE,
	PLAY_VULKAN,
	PLAY_SDL,
};

struct line_video_playback {
	GSubprocess *process;
	GCancellable *cancel;
	GThread *monitor;
	GMutex lock;
	char *ffplay, *vk_params;
	const char *hwaccel;
	int fd;
	enum playback_mode mode;
	LineVideoDone done;
	gpointer data;
};

static char *vulkan_options(const char *device)
{
	GString *options = g_string_new("create_by_placebo=1:device=");

	/* FFplay parses device names as an FFmpeg dictionary value. */
	for (; *device; device++) {
		if (strchr("\\':", *device))
			g_string_append_c(options, '\\');
		g_string_append_c(options, *device);
	}
	return g_string_free(options, FALSE);
}

static GSubprocess *spawn_player(LineVideoPlayback *play, GError **error)
{
	g_autoptr(GSubprocessLauncher) launcher =
		g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
					  G_SUBPROCESS_FLAGS_STDERR_PIPE);
	const char *const options[] = {
		"-hide_banner",
		"-loglevel",
		play->mode == PLAY_SDL ? "level+error" : "level+verbose",
		"-stats",
		"-autoexit",
		"-sn",
		"-threads",
		"2",
		"-window_title",
		"Avernella media player",
		"-protocol_whitelist",
		"file",
		"-format_whitelist",
		"mov,matroska,webm,avi,mpeg,mpegts,mpegvideo,flv,ogg,mp3,wav,aac"
	};
	const char *args[G_N_ELEMENTS(options) + 10] = { play->ffplay };
	guint i = 1 + G_N_ELEMENTS(options);
	int fd = fcntl(play->fd, F_DUPFD_CLOEXEC, 3);

	if (fd < 0) {
		g_set_error_literal(
			error, G_IO_ERROR, g_io_error_from_errno(errno),
			"Could not open the attachment for playback.");
		return NULL;
	}
	memcpy(args + 1, options, sizeof(options));
	if (play->mode != PLAY_SDL) {
		args[i++] = "-enable_vulkan";
		args[i++] = "-vulkan_params";
		args[i++] = play->vk_params;
		if (play->mode == PLAY_HW_DECODE) {
			args[i++] = "-hwaccel";
			args[i++] = play->hwaccel;
		}
	}
	args[i++] = "-i";
	args[i++] = "/proc/self/fd/3";
	args[i] = NULL;
	g_subprocess_launcher_setenv(
		launcher, "SDL_RENDER_DRIVER",
		play->mode == PLAY_SDL ? "software" : "vulkan", TRUE);
	g_subprocess_launcher_setenv(launcher, "AV_LOG_FORCE_NOCOLOR", "1",
				     TRUE);
	/*
	 * Reopening the inherited descriptor gives each attempt a seekable
	 * private input starting at offset zero.
	 */
	g_subprocess_launcher_take_fd(launcher, fd, 3);
	return g_subprocess_launcher_spawnv(launcher, args, error);
}

static enum playback_mode fallback_mode(const char *line,
					enum playback_mode mode)
{
	static const char *const renderer_errors[] = {
		"Failed to create window", "Failed to create vulkan renderer",
		"Vulkan renderer is not available", "Unrecognized option",
		"Option not found"
	};
	static const char *const decoder_errors[] = {
		"Failed setup for format",
		"No support for codec",
		"Device creation failed",
		"Failed to create a hardware device",
		"Hardware does not support",
		"Failed to initialise VAAPI",
		"Failed to initialize VAAPI",
		"Cannot load libcuda",
		"No device available for decoder"
	};

	for (guint i = 0; i < G_N_ELEMENTS(renderer_errors); i++)
		if (strstr(line, renderer_errors[i]))
			return PLAY_SDL;
	if (mode == PLAY_HW_DECODE)
		for (guint i = 0; i < G_N_ELEMENTS(decoder_errors); i++)
			if (strstr(line, decoder_errors[i]))
				return PLAY_VULKAN;
	return mode;
}

struct player_status {
	enum playback_mode mode, next;
	gboolean played, decoded, error, fatal, late_error;
};

/* FFplay emits progress on stderr even at error log level. A finite master
 * clock means playback began. A decoder may log its failed hardware setup
 * and then play the same clip successfully with its own software fallback.
 */
static void player_line(struct player_status *status, const char *line)
{
	char *end;

	while (g_ascii_isspace(*line))
		line++;
	/* FFplay 7 prefixes progress with [info] at verbose log level. */
	if (g_str_has_prefix(line, "[info]")) {
		line += sizeof("[info]") - 1;
		while (g_ascii_isspace(*line))
			line++;
	}
	if (g_ascii_isdigit(*line) || *line == '-') {
		g_ascii_strtod(line, &end);
		if (end != line && strstr(end, " fd=") && strstr(end, " aq=") &&
		    (strstr(end, "M-V:") || strstr(end, "A-V:") ||
		     (strstr(end, "M-A:") && status->mode == status->next))) {
			status->played = TRUE;
			return;
		}
	}
	if (*line != '[')
		return;
	/* FFplay builds its video filter input on the first decoded frame,
	 * including a one-frame clip that ends before the clock is printed.
	 */
	if (g_str_has_prefix(line, "[ffplay_buffer @ ") &&
	    strstr(line, "] [verbose] w:") && strstr(line, " pixfmt:"))
		status->decoded = TRUE;
	const char *level = strchr(line, ']');

	if (level && g_str_has_prefix(level, "] ["))
		line = level + 2;
	gboolean fatal = g_str_has_prefix(line, "[fatal]") ||
			 g_str_has_prefix(line, "[panic]");
	if (!fatal && !g_str_has_prefix(line, "[error]"))
		return;
	status->error = TRUE;
	status->fatal |= fatal;
	status->late_error |= status->played;
	enum playback_mode next = fallback_mode(line, status->mode);

	if (next > status->next)
		status->next = next;
}

static gpointer monitor_playback(gpointer data)
{
	LineVideoPlayback *play = data;
	gboolean success = FALSE;

	for (;;) {
		GInputStream *pipe =
			g_subprocess_get_stderr_pipe(play->process);
		struct player_status status = { .mode = play->mode,
						.next = play->mode };
		char chunk[1024], line[512];
		gsize used = 0;
		gssize n;

		/* Drain without retaining file metadata, even for a long line. */
		while ((n = g_input_stream_read(pipe, chunk, sizeof(chunk),
						play->cancel, NULL)) > 0) {
			for (gssize i = 0; i < n; i++) {
				if (chunk[i] == '\r' || chunk[i] == '\n') {
					line[used] = 0;
					player_line(&status, line);
					used = 0;
				} else if (used < sizeof(line) - 1) {
					line[used++] = chunk[i];
				}
			}
		}
		if (used) {
			line[used] = 0;
			player_line(&status, line);
		}
		gboolean ended = g_subprocess_wait(play->process, NULL, NULL);

		if (!ended || n < 0 || g_cancellable_is_cancelled(play->cancel))
			break;
		gboolean clean = g_subprocess_get_successful(play->process);
		/* A terminal format/renderer error can produce exit status zero.
		 * A clean exit with only a recoverable decoder warning may also
		 * be a completed one-frame clip, before the first stats tick.
		 */
		success = clean && !status.late_error &&
			  (status.played || !status.error ||
			   (status.decoded && !status.fatal &&
			    status.next == PLAY_VULKAN &&
			    play->mode == PLAY_HW_DECODE));
		if (success || status.played || play->mode == PLAY_SDL)
			break;
		enum playback_mode next = status.next;

		if (next == play->mode) {
			if (clean && !status.fatal)
				break;
			/* A driver can exit or crash before emitting a diagnostic. */
			next = play->mode + 1;
		}
		g_mutex_lock(&play->lock);
		if (!g_cancellable_is_cancelled(play->cancel)) {
			play->mode = next;
			GSubprocess *retry = spawn_player(play, NULL);

			if (retry) {
				g_object_unref(play->process);
				play->process = retry;
				g_mutex_unlock(&play->lock);
				continue;
			}
		}
		g_mutex_unlock(&play->lock);
		break;
	}
	if (!g_cancellable_is_cancelled(play->cancel) && play->done)
		play->done(success ? NULL :
				     "FFplay could not play this attachment.",
			   play->data);
	return NULL;
}

static gboolean playable_fd(int fd)
{
	struct stat st;
	int flags = fcntl(fd, F_GETFL);

	return flags >= 0 && !(flags & O_PATH) &&
	       (flags & O_ACCMODE) == O_RDONLY && !fstat(fd, &st) &&
	       S_ISREG(st.st_mode) && st.st_size > 0 &&
	       st.st_size <= LINE_MEDIA_LIMIT;
}

LineVideoPlayback *line_video_play(int fd, const struct line_video_gpu *gpu,
				   LineVideoDone done, gpointer data,
				   GError **error)
{
	if (!playable_fd(fd)) {
		if (fd >= 0)
			close(fd);
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Choose a readable attachment up to 64 MiB.");
		return NULL;
	}
	g_autofree char *ffplay = g_find_program_in_path("ffplay");

	if (!ffplay) {
		close(fd);
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"Install FFplay to play video and audio attachments.");
		return NULL;
	}
	LineVideoPlayback *play = g_new0(LineVideoPlayback, 1);

	play->fd = fd;
	play->ffplay = g_steal_pointer(&ffplay);
	play->mode = PLAY_SDL;
	if (gpu && gpu->name && *gpu->name) {
		play->vk_params = vulkan_options(gpu->name);
		switch (gpu->vendor) {
		case 0x10de:
			play->hwaccel = "cuda";
			break;
		case 0x1002:
		case 0x8086:
			play->hwaccel = "vulkan";
			break;
		}
		play->mode = play->hwaccel ? PLAY_HW_DECODE : PLAY_VULKAN;
	}
	play->cancel = g_cancellable_new();
	play->done = done;
	play->data = data;
	play->process = spawn_player(play, error);
	if (!play->process) {
		line_video_play_free(play);
		return NULL;
	}
	play->monitor =
		g_thread_try_new("line-player", monitor_playback, play, error);
	if (!play->monitor) {
		line_video_play_free(play);
		return NULL;
	}
	return play;
}

void line_video_play_free(LineVideoPlayback *play)
{
	if (!play)
		return;
	g_cancellable_cancel(play->cancel);
	g_mutex_lock(&play->lock);
	if (play->process)
		g_subprocess_force_exit(play->process);
	g_mutex_unlock(&play->lock);
	if (play->monitor)
		g_thread_join(play->monitor);
	else if (play->process)
		g_subprocess_wait(play->process, NULL, NULL);
	g_object_unref(play->cancel);
	g_clear_object(&play->process);
	g_mutex_clear(&play->lock);
	close(play->fd);
	g_free(play->vk_params);
	g_free(play->ffplay);
	g_free(play);
}
