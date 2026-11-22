// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_theme.h"
#include <errno.h>
#include <string.h>

static gboolean line_layout;

static char *theme_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "avernella", "layout",
				NULL);
}

void ui_theme_init(void)
{
	g_autofree char *path = theme_path();
	g_autofree char *name = NULL;
	gsize len = 0;

	/* LINE is the default; only an explicit choice keeps Avernella. */
	line_layout = !(g_file_get_contents(path, &name, &len, NULL) &&
			len == sizeof("avernella\n") - 1 &&
			!memcmp(name, "avernella\n", len));
}

gboolean ui_theme_is_line(void)
{
	return line_layout;
}

gboolean ui_theme_toggle(GError **error)
{
	g_autofree char *path = theme_path();
	g_autofree char *dir = g_path_get_dirname(path);

	if (g_mkdir_with_parents(dir, 0700) < 0) {
		int err = errno;

		g_set_error(
			error, G_FILE_ERROR, g_file_error_from_errno(err),
			"Cannot create the appearance settings directory: %s",
			g_strerror(err));
		return FALSE;
	}
	if (!g_file_set_contents_full(
		    path, line_layout ? "avernella\n" : "line\n", -1,
		    G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
		return FALSE;
	line_layout = !line_layout;
	return TRUE;
}
