// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_theme.h"
#include <errno.h>
#include <string.h>

static gboolean line_layout;
static int sidebar_width;
enum ui_palette ui_palette;

const struct ui_palette_colors ui_palettes[UI_PALETTE_COUNT] = {
	[UI_PALETTE_PINK] = { 0xff5ba9, 0xff8fc2, 0x210d1a, 0x492338,
			      0x783951 },
	[UI_PALETTE_BLUE] = { 0x79aeff, 0xa4c8ff, 0x101d37, 0x25365a,
			      0x36588a },
	[UI_PALETTE_MINT] = { 0x55dbad, 0x8aebc8, 0x08271e, 0x23463c,
			      0x336753 },
	[UI_PALETTE_AMBER] = { 0xf4b962, 0xffd299, 0x301b09, 0x483720,
			       0x745535 },
};

static const char *const palette_names[] = { "pink", "blue", "mint", "amber" };

static char *theme_path(const char *name)
{
	return g_build_filename(g_get_user_config_dir(), "avernella", name,
				NULL);
}

void ui_theme_init(void)
{
	g_autofree char *path = theme_path("layout");
	g_autofree char *accent = theme_path("accent");
	g_autofree char *sidebar = theme_path("sidebar-width");
	g_autofree char *name = NULL;
	gsize len = 0;

	/* LINE is the default; only an explicit choice keeps Avernella. */
	line_layout = !(g_file_get_contents(path, &name, &len, NULL) &&
			len == sizeof("avernella\n") - 1 &&
			!memcmp(name, "avernella\n", len));
	sidebar_width = 0;
	g_clear_pointer(&name, g_free);
	if (g_file_get_contents(sidebar, &name, &len, NULL) && len &&
	    len <= sizeof("480\n") - 1 && !memchr(name, 0, len)) {
		guint64 width;

		if (g_ascii_string_to_unsigned(g_strchomp(name), 10,
					       UI_SIDEBAR_MIN, UI_SIDEBAR_MAX,
					       &width, NULL))
			sidebar_width = width;
	}
	ui_palette = UI_PALETTE_PINK;
	g_clear_pointer(&name, g_free);
	if (!g_file_get_contents(accent, &name, &len, NULL))
		return;
	for (guint i = 0; i < G_N_ELEMENTS(palette_names); i++)
		if (len == strlen(palette_names[i]) &&
		    !strcmp(name, palette_names[i]))
			ui_palette = i;
}

gboolean ui_theme_is_line(void)
{
	return line_layout;
}

static gboolean save(const char *name, const char *value, GError **error)
{
	g_autofree char *path = theme_path(name);
	g_autofree char *dir = g_path_get_dirname(path);

	if (g_mkdir_with_parents(dir, 0700) < 0) {
		int err = errno;

		g_set_error(
			error, G_FILE_ERROR, g_file_error_from_errno(err),
			"Cannot create the appearance settings directory: %s",
			g_strerror(err));
		return FALSE;
	}
	return g_file_set_contents_full(
		path, value, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

gboolean ui_theme_toggle(GError **error)
{
	if (!save("layout", line_layout ? "avernella\n" : "line\n", error))
		return FALSE;
	line_layout = !line_layout;
	return TRUE;
}

gboolean ui_theme_set_palette(enum ui_palette palette, GError **error)
{
	g_return_val_if_fail((guint)palette < UI_PALETTE_COUNT, FALSE);
	if (!save("accent", palette_names[palette], error))
		return FALSE;
	ui_palette = palette;
	return TRUE;
}

int ui_theme_sidebar_width(void)
{
	return sidebar_width;
}

gboolean ui_theme_save_sidebar_width(int width, GError **error)
{
	char value[16];

	g_return_val_if_fail(width >= UI_SIDEBAR_MIN && width <= UI_SIDEBAR_MAX,
			     FALSE);
	if (width == sidebar_width)
		return TRUE;
	g_snprintf(value, sizeof(value), "%d\n", width);
	if (!save("sidebar-width", value, error))
		return FALSE;
	sidebar_width = width;
	return TRUE;
}
