/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

typedef struct line_video_playback LineVideoPlayback;
typedef void (*LineVideoDone)(const char *error, gpointer data);

/*
 * Takes ownership of fd, including on failure. The callback runs on the
 * monitor thread with an error valid for the duration of the call, or NULL
 * on normal playback completion. Vulkan playback can fall back to software.
 */
LineVideoPlayback *line_video_play(int fd, gboolean use_vulkan,
				   LineVideoDone done, gpointer data,
				   GError **error);

/* Cancels, reaps and joins. Must not be called from the callback. */
void line_video_play_free(LineVideoPlayback *playback);
