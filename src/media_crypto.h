/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "media.h"

enum line_media_object {
	LINE_MEDIA_BODY,
	LINE_MEDIA_VIDEO_BODY,
	LINE_MEDIA_VIDEO_POSTER
};

/*
 * Consume inputs so a uniquely owned media buffer can be processed in place.
 * Videos emit a separate sequence of 128 KiB chunk hashes for OBS.
 * original_size is the advertised full attachment length, or 0 if absent.
 */
GBytes *line_media_encrypt(GBytes *plain, const guint8 material[32],
			   GBytes **chunk_hashes, GError **error);
GBytes *line_media_decrypt(GBytes *cipher, const guint8 material[32],
			   enum line_media_object object, guint64 original_size,
			   GError **error);
