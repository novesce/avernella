/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/*
 * Pasting pictures and files from the desktop clipboard. The window asks
 * SDL3 (through sdl2-compat) for the offered MIME types; these helpers
 * choose one and stage it for the ordinary photo or file preview.
 */

/* The preferred picture type on offer and its file extension, or NULL. */
const char *line_paste_image_type(const char *const *mimes, gsize count,
				  const char **extension);

/*
 * The first local file named in a text/uri-list (as file managers copy
 * files), if it is a regular file; caller frees.
 */
char *line_paste_file(const char *uri_list, gsize length);

/*
 * Writes clipboard picture bytes to a new 0600 file in a private 0700
 * directory under XDG_RUNTIME_DIR (or the temporary directory). The bytes
 * must begin like the named type. Returns the path; caller unlinks it.
 */
char *line_paste_save(const void *data, gsize length, const char *mime,
		      GError **error);
