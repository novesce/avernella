/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

enum {
	/* LINE's own clients upload square profile pictures of this size. */
	LINE_PROFILE_SIDE = 640,
	LINE_PROFILE_FILE_LIMIT = 20 * 1024 * 1024
};

/*
 * Decodes a JPEG, PNG or WebP picture, crops the centered square and
 * scales it to at most LINE_PROFILE_SIDE, then encodes a baseline JPEG.
 * Transparent pixels are composited onto white.
 */
GBytes *line_profile_jpeg(GBytes *image, GError **error);

/* Reads a local picture safely and encodes a centered square baseline JPEG. */
GBytes *line_profile_image_prepare(const char *path, GError **error);

/* Replaces the account's profile picture with the file at 'path'. */
J *line_own_picture_update(Line *s, const char *path, GError **error);
