// SPDX-License-Identifier: GPL-3.0-or-later
#include "clipboard_paste.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

static char *runtime;

static void choose_type(void)
{
	const char *ext = NULL;
	const char *const browser[] = { "text/html", "image/jpeg", "image/png",
					"text/plain" };
	const char *const text[] = { "text/plain", "UTF8_STRING" };
	const char *const upper[] = { "IMAGE/WEBP" };

	/* PNG wins over JPEG when both are offered. */
	g_assert_cmpstr(line_paste_image_type(browser, 4, &ext), ==,
			"image/png");
	g_assert_cmpstr(ext, ==, ".png");
	g_assert_null(line_paste_image_type(text, 2, &ext));
	g_assert_null(line_paste_image_type(NULL, 0, &ext));
	g_assert_cmpstr(line_paste_image_type(upper, 1, &ext), ==,
			"image/webp");
	g_assert_cmpstr(ext, ==, ".webp");
}

static void copied_files(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree char *file = g_build_filename(runtime, "a b.txt", NULL);

	g_assert_true(g_file_set_contents(file, "x", 1, &error));
	g_autofree char *uri = g_filename_to_uri(file, NULL, &error);
	g_autofree char *list = g_strdup_printf(
		"# copied\r\nfile:///nonexistent/x\r\nfile://%s\r\n%s\r\n",
		runtime, uri);
	g_autofree char *path = line_paste_file(list, strlen(list));

	/* The directory is skipped; the first regular file wins. */
	g_assert_cmpstr(path, ==, file);
	g_assert_null(line_paste_file("https://example.com/a.png\r\n", 27));
	g_assert_null(line_paste_file("", 0));
	g_unlink(file);
}

static void staged_picture(void)
{
	static const guint8 png[] = { 0x89, 'P',  'N', 'G', '\r', '\n',
				      0x1a, '\n', 0,   1,   2 };
	g_autoptr(GError) error = NULL;
	g_autofree char *path =
		line_paste_save(png, sizeof(png), "image/png", &error);
	struct stat st;

	g_assert_no_error(error);
	g_assert_true(g_str_has_suffix(path, ".png"));
	g_assert_true(g_str_has_prefix(path, runtime));
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(st.st_size, ==, sizeof(png));
	g_autofree char *dir = g_path_get_dirname(path);

	g_assert_cmpint(stat(dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	g_unlink(path);
	/* Bytes must match the claimed type. */
	g_assert_null(line_paste_save(png, sizeof(png), "image/jpeg", &error));
	g_clear_error(&error);
	g_assert_null(line_paste_save(png, sizeof(png), "image/gif", &error));
	g_clear_error(&error);
	g_assert_null(line_paste_save(png, 0, "image/png", &error));
	g_clear_error(&error);
	/* A shared paste folder is refused. */
	g_assert_cmpint(chmod(dir, 0755), ==, 0);
	g_assert_null(line_paste_save(png, sizeof(png), "image/png", &error));
	g_assert_nonnull(error);
	g_assert_cmpint(chmod(dir, 0700), ==, 0);
	g_rmdir(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	runtime = g_dir_make_tmp("line-paste-XXXXXX", NULL);
	g_assert_nonnull(runtime);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_test_add_func("/clipboard-paste/picture-type", choose_type);
	g_test_add_func("/clipboard-paste/copied-files", copied_files);
	g_test_add_func("/clipboard-paste/staged-picture", staged_picture);
	int rc = g_test_run();

	g_rmdir(runtime);
	g_free(runtime);
	return rc;
}
