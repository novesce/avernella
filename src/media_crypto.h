/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "media.h"

/*
 * Consume inputs so a uniquely owned media buffer can be processed in place.
 * Videos emit a separate sequence of 128 KiB chunk hashes for OBS.
 */
GBytes *line_media_encrypt(GBytes *plain, const guint8 material[32],
			   GBytes **chunk_hashes, GError **error);
GBytes *line_media_decrypt(GBytes *cipher, const guint8 material[32],
			   GError **error);
