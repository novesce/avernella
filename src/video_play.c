// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_play.h"
#include "media.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

struct line_video_playback {
	GSubprocess *process;
	GCancellable *cancel;
	GThread *monitor;
	LineVideoDone done;
	gpointer data;
};

static gpointer monitor_playback(gpointer data)
{
	LineVideoPlayback *play = data;
	GInputStream *stderr_pipe = g_subprocess_get_stderr_pipe(play->process);
	gboolean logged_error = FALSE;
	guint8 discard[1024];
	gssize n;

	/*
	 * FFplay can return success even after a demux or decode error. Only
	 * retain the fact that it wrote an error, never filenames or metadata.
	 */
	while ((n = g_input_stream_read(stderr_pipe, discard, sizeof(discard),
					play->cancel, NULL)) > 0)
		logged_error = TRUE;
	gboolean ended = g_subprocess_wait(play->process, NULL, NULL);

	if (!g_cancellable_is_cancelled(play->cancel) && play->done) {
		const char *error =
			ended && n == 0 && !logged_error &&
					g_subprocess_get_successful(
						play->process) ?
				NULL :
				"FFplay could not play this attachment.";

		play->done(error, play->data);
	}
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

LineVideoPlayback *line_video_play(int fd, gboolean use_vulkan,
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
	g_autoptr(GSubprocessLauncher) launcher =
		g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
					  G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv(
		launcher, "SDL_RENDER_DRIVER",
		use_vulkan ? "vulkan,software" : "software", TRUE);

	/*
	 * An inherited fd keeps playback seekable, even after the worker
	 * releases its memfd. Restrict probing to ordinary media containers.
	 * The MOV demuxer has its external track references disabled by default.
	 */
	g_subprocess_launcher_take_fd(launcher, fd, 3);
	GSubprocess *process = g_subprocess_launcher_spawn(
		launcher, error, ffplay, "-hide_banner", "-loglevel", "error",
		"-nostats", "-autoexit", "-sn", "-threads", "2",
		"-window_title", "LINE media player", "-protocol_whitelist",
		"file", "-format_whitelist",
		"mov,matroska,webm,avi,mpeg,mpegts,mpegvideo,flv,ogg,mp3,wav,aac",
		"-i", "/proc/self/fd/3", NULL);
	if (!process)
		return NULL;
	LineVideoPlayback *play = g_new0(LineVideoPlayback, 1);

	play->process = process;
	play->cancel = g_cancellable_new();
	play->done = done;
	play->data = data;
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
	g_subprocess_force_exit(play->process);
	if (play->monitor)
		g_thread_join(play->monitor);
	else
		g_subprocess_wait(play->process, NULL, NULL);
	g_object_unref(play->cancel);
	g_object_unref(play->process);
	g_free(play);
}
