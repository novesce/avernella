/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

enum {
	LINE_MEDIA_IMAGE = 1,
	LINE_MEDIA_VIDEO = 2,
	LINE_MEDIA_AUDIO = 3,
	LINE_MEDIA_FILE = 14,
	LINE_MEDIA_LIMIT = 64 * 1024 * 1024,
	LINE_MEDIA_PREVIEW_LIMIT = 2 * 1024 * 1024
};

/*
 * Sends an encrypted attachment and returns the acknowledged message ID.
 * The caller reserves and persists the request sequence as for line_send.
 */
J *line_send_attachment(Line *s, const char *chat, const char *path,
			int content_type, GError **error);
J *line_send_square_attachment(Line *s, const char *chat,
			       const char *my_member_id, const char *path,
			       int content_type, gint32 seq, GError **error);
J *line_send_image(Line *s, const char *chat, const char *path, GError **error);
J *line_send_file(Line *s, const char *chat, const char *path, GError **error);
J *line_forward_attachment(Line *s, const char *chat, J *message,
			   GError **error);
J *line_forward_square_attachment(Line *s, const char *chat,
				  const char *my_member_id, J *message,
				  gint32 seq, GError **error);

/*
 * Input is the original numeric-field Thrift message. Encrypted objects return
 * bytes only after verification. Legacy unsealed objects use their message ID.
 */
GBytes *line_download_attachment(Line *s, J *message, GError **error);
GBytes *line_download_image(Line *s, J *message, GError **error);
GBytes *line_download_video_preview(Line *s, J *message, GError **error);
char *line_attachment_filename(Line *s, J *message, GError **error);
