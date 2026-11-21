// SPDX-License-Identifier: GPL-3.0-or-later
#include "clipboard_paste.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	/* The photo preview's own decode limit. */
	PASTE_LIMIT = 32 * 1024 * 1024,
	URI_LIST_LIMIT = 64 * 1024
};

static const struct {
	const char *mime, *extension;
} pictures[] = {
	/* Lossless first: screenshots and copied images are usually PNG. */
	{ "image/png", ".png" },
	{ "image/webp", ".webp" },
	{ "image/jpeg", ".jpg" },
	{ "image/jpg", ".jpg" },
};

const char *line_paste_image_type(const char *const *mimes, gsize count,
				  const char **extension)
{
	for (guint p = 0; p < G_N_ELEMENTS(pictures); p++)
		for (gsize i = 0; mimes && i < count; i++)
			if (mimes[i] &&
			    !g_ascii_strcasecmp(mimes[i], pictures[p].mime)) {
				if (extension)
					*extension = pictures[p].extension;
				return pictures[p].mime;
			}
	return NULL;
}

char *line_paste_file(const char *uri_list, gsize length)
{
	if (!uri_list || !length || length > URI_LIST_LIMIT)
		return NULL;
	g_autofree char *text = g_strndup(uri_list, length);

	g_auto(GStrv) uris = g_uri_list_extract_uris(text);

	for (guint i = 0; uris && uris[i]; i++) {
		g_autofree char *path =
			g_filename_from_uri(uris[i], NULL, NULL);
		struct stat st;

		if (path && !stat(path, &st) && S_ISREG(st.st_mode))
			return g_steal_pointer(&path);
	}
	return NULL;
}

static gboolean looks_like(const guint8 *d, gsize n, const char *mime)
{
	if (!strcmp(mime, "image/png"))
		return n >= 8 && !memcmp(d, "\211PNG\r\n\032\n", 8);
	if (!strcmp(mime, "image/webp"))
		return n >= 12 && !memcmp(d, "RIFF", 4) &&
		       !memcmp(d + 8, "WEBP", 4);
	return n >= 3 && !memcmp(d, "\xff\xd8\xff", 3);
}

static char *private_dir(GError **error)
{
	const char *runtime = g_getenv("XDG_RUNTIME_DIR");
	g_autofree char *dir = g_build_filename(
		runtime && g_path_is_absolute(runtime) ? runtime :
							 g_get_tmp_dir(),
		"avernella", NULL);
	struct stat st;

	if (g_mkdir(dir, 0700) && errno != EEXIST) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			    "Could not stage the pasted image: %s.",
			    g_strerror(errno));
		return NULL;
	}
	/* Never write into a directory another user could read. */
	if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
	    (st.st_mode & 077)) {
		g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_PERM,
				    "The paste folder is not private.");
		return NULL;
	}
	return g_steal_pointer(&dir);
}

char *line_paste_save(const void *data, gsize length, const char *mime,
		      GError **error)
{
	const char *extension = NULL;
	const char *const one[] = { mime };

	if (!mime || !line_paste_image_type(one, 1, &extension) || !data ||
	    !length || length > PASTE_LIMIT ||
	    !looks_like(data, length, mime)) {
		g_set_error_literal(
			error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			"The clipboard picture is not a JPEG, PNG or WebP of up to 32 MiB.");
		return NULL;
	}
	g_autofree char *dir = private_dir(error);

	if (!dir)
		return NULL;
	g_autofree char *name =
		g_strconcat("Pasted image XXXXXX", extension, NULL);
	char *path = g_build_filename(dir, name, NULL);
	int fd = g_mkstemp_full(path, O_RDWR | O_CLOEXEC, 0600);

	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			    "Could not stage the pasted image: %s.",
			    g_strerror(errno));
		g_free(path);
		return NULL;
	}
	const guint8 *at = data;
	gsize left = length;

	while (left) {
		ssize_t n = write(fd, at, left);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		at += n;
		left -= n;
	}
	if (left || close(fd)) {
		if (left)
			close(fd);
		g_unlink(path);
		g_free(path);
		g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_IO,
				    "Could not stage the pasted image.");
		return NULL;
	}
	return path;
}
