// SPDX-License-Identifier: GPL-3.0-or-later
#include "state.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static char *sandbox;

static void persistence(void)
{
	g_autoptr(GError) error = NULL;
	Line a = { 0 }, b = { 0 };
	int dir = line_store_open(&a, &error);

	g_assert_cmpint(dir, >=, 0);
	g_assert_cmpint(line_store_open(&b, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);
	a.access = g_strdup("fixture");
	a.keys = obj();
	a.seq = 12;
	a.refresh_at = 1900000000;
	g_assert_true(line_store_save(dir, &a, &error));
	g_autofree char *path =
		g_build_filename(sandbox, "avernella", "session.json", NULL);
	struct stat st;

	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	close(dir);
	dir = line_store_open(&b, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(b.access, ==, "fixture");
	g_assert_cmpint(b.seq, ==, 12);
	/* The renewal schedule survives a restart. */
	g_assert_cmpint(b.refresh_at, ==, 1900000000);
	g_assert_true(line_store_forget(dir, &error));
	close(dir);
	line_session_clear(&a);
	line_session_clear(&b);
}

static void legacy_migration(void)
{
	g_autofree char *current = g_build_filename(sandbox, "avernella", NULL);
	g_autofree char *legacy = g_build_filename(sandbox, "kitatide", NULL);
	g_autofree char *settings =
		g_build_filename(current, "lists-fixture.json", NULL);
	g_autoptr(GError) error = NULL;
	Line old = { 0 }, restored = { 0 };
	int dir = line_store_open(&old, &error);

	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	old.access = g_strdup("legacy-session");
	old.keys = obj();
	old.seq = 42;
	json_object_object_add(old.keys, "own",
			       json_object_new_string("fixture-key"));
	g_assert_true(line_store_save(dir, &old, &error));
	g_assert_true(g_file_set_contents_full(settings, "{}", -1,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, &error));
	/* Simulate a previous version holding the lock on the old location. */
	g_assert_cmpint(g_rename(current, legacy), ==, 0);
	g_assert_true(line_store_has_session());
	g_assert_cmpint(line_store_open(&restored, &error), ==, -1);
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_false(g_file_test(current, G_FILE_TEST_EXISTS));
	close(dir);

	dir = line_store_open(&restored, &error);
	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	g_assert_cmpstr(restored.access, ==, "legacy-session");
	g_assert_cmpint(restored.seq, ==, 42);
	g_assert_cmpstr(strfield(restored.keys, "own"), ==, "fixture-key");
	g_assert_true(g_file_test(settings, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(legacy, G_FILE_TEST_EXISTS));
	g_assert_true(line_store_has_session());
	g_assert_true(line_store_forget(dir, &error));
	g_assert_false(line_store_has_session());
	g_assert_cmpint(g_remove(settings), ==, 0);
	close(dir);
	line_session_clear(&old);
	line_session_clear(&restored);
}

static void current_directory_wins(void)
{
	g_autofree char *legacy = g_build_filename(sandbox, "kitatide", NULL);
	g_autofree char *session =
		g_build_filename(legacy, "session.json", NULL);
	g_autoptr(GError) error = NULL;
	Line current = { 0 };
	int dir = line_store_open(&current, &error);

	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	close(dir);
	g_assert_cmpint(g_mkdir(legacy, 0700), ==, 0);
	g_assert_true(g_file_set_contents_full(
		session, "{\"access\":\"old\",\"keys\":{},\"seq\":0}", -1,
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error));
	/* An existing signed-out directory must not restore an older session. */
	g_assert_false(line_store_has_session());
	dir = line_store_open(&current, &error);
	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	g_assert_null(current.access);
	g_assert_cmpint(g_remove(session), ==, 0);
	g_assert_cmpint(g_rmdir(legacy), ==, 0);
	close(dir);
	line_session_clear(&current);
}

static void large_cache(void)
{
	Line a = { 0 }, restored = { 0 };

	g_autoptr(GError) error = NULL;
	int dir = line_store_open(&a, &error);

	g_assert_no_error(error);
	a.access = g_strdup("current");
	a.keys = obj();
	json_object_object_add(a.keys, "own",
			       json_object_new_string("approved-identity"));
	g_autofree char *data = g_malloc(8 * 1024 * 1024);

	memset(data, 'x', 8 * 1024 * 1024);
	g_autoptr(J) huge = json_object_new_string_len(data, 8 * 1024 * 1024);
	J *peers = obj();

	json_object_object_add(a.keys, "peers", peers);
	json_object_object_add(peers, "cached", json_object_get(huge));
	g_assert_true(line_store_save(dir, &a, &error));
	g_assert_no_error(error);
	/* Essential data exceeding the same limit must not replace a good file. */
	json_object_object_add(a.keys, "own", json_object_get(huge));
	g_assert_false(line_store_save(dir, &a, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	close(dir);
	dir = line_store_open(&restored, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(restored.access, ==, "current");
	g_assert_cmpstr(strfield(restored.keys, "own"), ==,
			"approved-identity");
	g_assert_null(field(field(restored.keys, "peers"), "cached"));
	g_assert_true(line_store_forget(dir, &error));
	close(dir);
	line_session_clear(&a);
	line_session_clear(&restored);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	sandbox = g_dir_make_tmp("line-state-XXXXXX", NULL);
	g_assert_nonnull(sandbox);
	g_setenv("XDG_STATE_HOME", sandbox, TRUE);
	g_test_add_func("/state/private-atomic-roundtrip", persistence);
	g_test_add_func("/state/legacy-migration", legacy_migration);
	g_test_add_func("/state/current-directory-wins",
			current_directory_wins);
	g_test_add_func("/state/oversized-cache", large_cache);
	int result = g_test_run();
	g_autofree char *sub = g_build_filename(sandbox, "avernella", NULL);

	g_rmdir(sub);
	g_rmdir(sandbox);
	g_free(sandbox);
	return result;
}
