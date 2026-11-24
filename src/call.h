/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * The network worker serializes all public calls. The media thread receives
 * only call-local information and never shares or retains the account token.
 */
J *line_voice_start(Line *s, const char *chat, GError **error);
gboolean line_video_enabled(void);
gboolean line_group_voice_enabled(void);
gboolean line_group_video_enabled(void);
/* Worker-thread view of the active group call, if any. */
const char *line_call_group_chat(gboolean connected);
J *line_call_start(Line *s, const char *chat, gboolean video, GError **error);
J *line_call_camera(gboolean enabled, GError **error);
J *line_call_video_focus(const char *chat, const char *id, GError **error);
void line_call_preview(const char *chat, gboolean visible);
/* Starts or stops sharing this desktop through the screen cast portal. */
J *line_call_share(gboolean enabled, GError **error);
J *line_voice_hangup(GError **error);
J *line_voice_mute(gboolean muted, GError **error);
void line_voice_close(void);
void line_call_frame_socket(int fd);

/* Only feed operations from the authenticated account's Talk sync stream. */
void line_voice_operation(Line *s, J *op);
/* Expire a due invitation; return its monotonic deadline, or zero. */
gint64 line_voice_tick(void);
J *line_voice_answer(const char *caller, GError **error);
J *line_voice_decline(const char *caller, GError **error);
/* Takes the latest pending remote frame as tightly packed I420. */
struct line_preview_frame;
GBytes *line_call_take_frame(const char *chat, struct line_preview_frame *info);
/* Takes the latest frame of the peer's screen share as packed I420. */
GBytes *line_call_take_share_frame(const char *chat, int *width, int *height,
				   int *rotation, gboolean *sharing);
/* Takes the latest local camera frame while the camera is on. */
GBytes *line_call_take_self_frame(const char *chat, int *width, int *height);
