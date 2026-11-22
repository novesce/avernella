// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_theme.h"
#include <glib/gstdio.h>

static void preference(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree char *contents = NULL;
	g_autofree char *dir =
		g_build_filename(g_get_user_config_dir(), "avernella", NULL);
	g_autofree char *path = g_build_filename(dir, "layout", NULL);

	ui_theme_init();
	g_assert_false(ui_theme_is_line());
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_true(ui_theme_toggle(&error));
	g_assert_no_error(error);
	g_assert_true(ui_theme_is_line());

	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(contents, ==, "line\n");
	ui_theme_init();
	g_assert_true(ui_theme_is_line());
	g_assert_true(ui_theme_toggle(&error));
	g_assert_no_error(error);
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(contents, ==, "avernella\n");
	ui_theme_init();
	g_assert_false(ui_theme_is_line());

	g_assert_true(g_file_set_contents(path, "unknown\n", -1, &error));
	g_assert_no_error(error);
	ui_theme_init();
	g_assert_false(ui_theme_is_line());

	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
	g_assert_false(ui_theme_toggle(&error));
	g_assert_nonnull(error);
	g_assert_false(ui_theme_is_line());
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui/theme/preference", preference);
	return g_test_run();
}
