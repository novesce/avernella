/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * The network worker serializes all public calls. The media thread receives
 * only call-local information and never shares or retains the account token.
 */
J *line_voice_start(Line *s, const char *chat, GError **error);
gboolean line_video_enabled(void);
J *line_call_start(Line *s, const char *chat, gboolean video, GError **error);
J *line_call_camera(gboolean enabled, GError **error);
void line_call_preview(const char *chat, gboolean visible);
/* Starts or stops sharing this desktop through the screen cast portal. */
J *line_call_share(gboolean enabled, GError **error);
J *line_voice_hangup(GError **error);
J *line_voice_mute(gboolean muted, GError **error);
void line_voice_close(void);
void line_call_share_socket(int fd);

/* Only feed operations from the authenticated account's Talk sync stream. */
void line_voice_operation(Line *s, J *op);
void line_voice_tick(void);
J *line_voice_answer(const char *caller, GError **error);
J *line_voice_decline(const char *caller, GError **error);
/* Takes the latest pending remote frame as tightly packed I420. */
GBytes *line_call_take_frame(const char *chat, int *width, int *height,
			     int *rotation, gboolean *camera_enabled);
/* Takes the latest frame of the peer's screen share as packed I420. */
GBytes *line_call_take_share_frame(const char *chat, int *width, int *height,
				   int *rotation, gboolean *sharing);
/* Takes the latest local camera frame while the camera is on. */
GBytes *line_call_take_self_frame(const char *chat, int *width, int *height);
