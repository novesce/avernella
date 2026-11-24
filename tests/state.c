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

static void incomplete_session(void)
{
	g_autofree char *staged =
		g_build_filename(sandbox, LINE_STATE_DIR, "session.new", NULL);
	g_autofree char *session =
		g_build_filename(sandbox, LINE_STATE_DIR, "session.json", NULL);
	g_autoptr(GError) error = NULL;
	Line saved = { .access = g_strdup("committed"), .keys = obj() };
	Line restored = { 0 };
	int dir = line_store_open(&restored, &error);

	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	/* A failure to publish a staged file must remove its contents. */
	g_assert_cmpint(g_mkdir(session, 0700), ==, 0);
	g_assert_false(line_store_save(dir, &saved, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_false(g_file_test(staged, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_rmdir(session), ==, 0);

	g_assert_true(line_store_save(dir, &saved, &error));
	g_assert_true(g_file_set_contents_full(staged, "interrupted save", -1,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, &error));
	/* Only the owner, never a competing window, may discard staging. */
	g_assert_cmpint(line_store_open(&restored, &error), ==, -1);
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_true(g_file_test(staged, G_FILE_TEST_IS_REGULAR));
	close(dir);

	dir = line_store_open(&restored, &error);
	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	g_assert_cmpstr(restored.access, ==, "committed");
	g_assert_false(g_file_test(staged, G_FILE_TEST_EXISTS));
	/* An unremovable staging entry cannot prevent committed-token removal. */
	g_assert_cmpint(g_mkdir(staged, 0700), ==, 0);
	g_assert_false(line_store_forget(dir, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_false(g_file_test(session, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_rmdir(staged), ==, 0);
	close(dir);
	line_session_clear(&saved);
	line_session_clear(&restored);
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

static void current_directory_wins_over(const char *name)
{
	g_autofree char *legacy = g_build_filename(sandbox, name, NULL);
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

static void current_directory_wins(void)
{
	current_directory_wins_over("kitatide");
	current_directory_wins_over("line-linux");
}

/* A private earlier-client directory with a session and chat preferences. */
static void write_legacy(const char *name, const char *session)
{
	g_autofree char *dir = g_build_filename(sandbox, name, NULL);
	g_autofree char *file = g_build_filename(dir, "session.json", NULL);
	g_autofree char *lists =
		g_build_filename(dir, "lists-fixture.json", NULL);
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents_full(file, session, -1,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, &error));
	g_assert_true(g_file_set_contents_full(
		lists, "{}", -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error));
}

static void remove_legacy(const char *name)
{
	g_autofree char *dir = g_build_filename(sandbox, name, NULL);
	g_autofree char *file = g_build_filename(dir, "session.json", NULL);
	g_autofree char *lists =
		g_build_filename(dir, "lists-fixture.json", NULL);

	g_remove(file);
	g_remove(lists);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* Opens the store, checks the restored session, then cleans up. */
static void expect_restored(const char *access, gint64 seq)
{
	g_autofree char *lists = g_build_filename(sandbox, "avernella",
						  "lists-fixture.json", NULL);
	g_autoptr(GError) error = NULL;
	Line restored = { 0 };

	g_assert_true(line_store_has_session());
	int dir = line_store_open(&restored, &error);

	g_assert_no_error(error);
	g_assert_cmpint(dir, >=, 0);
	g_assert_cmpstr(restored.access, ==, access);
	g_assert_cmpint(restored.seq, ==, seq);
	/* Chat preferences move with the session. */
	g_assert_true(g_file_test(lists, G_FILE_TEST_IS_REGULAR));
	g_assert_true(line_store_forget(dir, &error));
	g_assert_cmpint(g_remove(lists), ==, 0);
	close(dir);
	line_session_clear(&restored);
}

/* Without an Avernella directory, line-linux's data moves in. */
static void line_linux_migration(void)
{
	g_autofree char *current = g_build_filename(sandbox, "avernella", NULL);
	g_autofree char *legacy = g_build_filename(sandbox, "line-linux", NULL);

	g_assert_cmpint(g_rmdir(current), ==, 0);
	write_legacy("line-linux",
		     "{\"access\":\"from-line-linux\",\"keys\":{},\"seq\":7}");
	expect_restored("from-line-linux", 7);
	g_assert_false(g_file_test(legacy, G_FILE_TEST_EXISTS));
}

/* kitatide is newer than line-linux, so it wins; line-linux stays put. */
static void newest_legacy_first(void)
{
	g_autofree char *current = g_build_filename(sandbox, "avernella", NULL);
	g_autofree char *older =
		g_build_filename(sandbox, "line-linux", "session.json", NULL);

	g_assert_cmpint(g_rmdir(current), ==, 0);
	write_legacy("kitatide",
		     "{\"access\":\"from-kitatide\",\"keys\":{},\"seq\":3}");
	write_legacy("line-linux",
		     "{\"access\":\"from-line-linux\",\"keys\":{},\"seq\":7}");
	expect_restored("from-kitatide", 3);
	g_assert_true(g_file_test(older, G_FILE_TEST_IS_REGULAR));
	remove_legacy("line-linux");
}

/* A session this version cannot read is left alone, not moved in. */
static void unreadable_legacy_skipped(void)
{
	g_autofree char *current = g_build_filename(sandbox, "avernella", NULL);
	g_autofree char *broken =
		g_build_filename(sandbox, "kitatide", "session.json", NULL);

	g_assert_cmpint(g_rmdir(current), ==, 0);
	write_legacy("kitatide", "{not json");
	write_legacy("line-linux",
		     "{\"access\":\"from-line-linux\",\"keys\":{},\"seq\":7}");
	expect_restored("from-line-linux", 7);
	g_assert_true(g_file_test(broken, G_FILE_TEST_IS_REGULAR));
	remove_legacy("kitatide");
}

/* Signing out removes a session file this version cannot read. */
static void unreadable_session_discarded(void)
{
	g_autofree char *file =
		g_build_filename(sandbox, "avernella", "session.json", NULL);
	g_autoptr(GError) error = NULL;
	Line broken = { 0 };

	g_assert_true(g_file_set_contents_full(file, "{not json", -1,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, &error));
	g_assert_true(line_store_has_session());
	g_assert_cmpint(line_store_open(&broken, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(line_store_discard(&error));
	g_assert_no_error(error);
	g_assert_false(line_store_has_session());
	/* Nothing to remove is not an error either. */
	g_assert_true(line_store_discard(&error));
	g_assert_no_error(error);
	line_session_clear(&broken);
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
	g_test_add_func("/state/incomplete-session", incomplete_session);
	g_test_add_func("/state/legacy-migration", legacy_migration);
	g_test_add_func("/state/current-directory-wins",
			current_directory_wins);
	g_test_add_func("/state/line-linux-migration", line_linux_migration);
	g_test_add_func("/state/newest-legacy-first", newest_legacy_first);
	g_test_add_func("/state/unreadable-legacy-skipped",
			unreadable_legacy_skipped);
	g_test_add_func("/state/unreadable-session-discarded",
			unreadable_session_discarded);
	g_test_add_func("/state/oversized-cache", large_cache);
	int result = g_test_run();
	g_autofree char *sub = g_build_filename(sandbox, "avernella", NULL);

	g_rmdir(sub);
	g_rmdir(sandbox);
	g_free(sandbox);
	return result;
}
