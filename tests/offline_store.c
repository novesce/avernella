// SPDX-License-Identifier: GPL-3.0-or-later
#include "offline_store.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <unistd.h>

static const guint8 key[LINE_STATE_KEY_SIZE] = { 1 };
static const char account[] = "u00000000000000000000000000000001";
static const char other[] = "u00000000000000000000000000000002";
static const char chat[] = "c00000000000000000000000000000003";

struct fixture {
	char *path;
	int dir;
	LineOffline *store;
};

static void setup(struct fixture *fixture, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;

	(void)unused;
	fixture->path = g_dir_make_tmp("avernella-offline-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->path);
	fixture->dir = open(fixture->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(fixture->dir, >=, 0);
	fixture->store = line_offline_open(fixture->dir, account, key);
	g_assert_nonnull(fixture->store);
}

static void teardown(struct fixture *fixture, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;

	(void)unused;
	line_offline_close(fixture->store);
	g_assert_true(line_offline_clear_all(fixture->dir, &error));
	g_assert_no_error(error);
	g_assert_cmpint(unlinkat(fixture->dir, "offline", AT_REMOVEDIR), ==, 0);
	close(fixture->dir);
	g_assert_cmpint(g_rmdir(fixture->path), ==, 0);
	g_free(fixture->path);
}

static char *slot_path(const char *home, const char *mid, const char *name)
{
	g_autofree char *digest =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, mid, -1);

	return g_build_filename(home, "offline", digest, name, NULL);
}

static void journal(struct fixture *fixture, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(J) initial = line_offline_load_outbox(fixture->store, &error);
	g_autoptr(J) rows =
		json_tokener_parse("[{\"text\":\"fixture-private-draft\"}]");

	(void)unused;
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(initial), ==, 0);
	g_assert_true(line_offline_save_outbox(fixture->store, rows, &error));
	g_assert_no_error(error);
	g_autofree char *path = slot_path(fixture->path, account, "outbox");
	g_autofree char *bytes = NULL;
	gsize size = 0;
	struct stat st;

	g_assert_true(g_file_get_contents(path, &bytes, &size, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_true(line_state_is_encrypted(bytes, size));
	g_assert_null(memmem(bytes, size, "fixture-private-draft", 21));
	g_clear_pointer(&fixture->store, line_offline_close);

	guint8 wrong[LINE_STATE_KEY_SIZE] = { 2 };

	g_autoptr(LineOffline) bad =
		line_offline_open(fixture->dir, account, wrong);

	g_assert_nonnull(bad);
	g_assert_null(line_offline_load_outbox(bad, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(line_offline_save_outbox(bad, initial, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_autoptr(LineOffline) second =
		line_offline_open(fixture->dir, other, key);
	g_autoptr(J) empty = line_offline_load_outbox(second, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(empty), ==, 0);
	g_autofree char *other_path = slot_path(fixture->path, other, "outbox");

	g_assert_true(g_file_set_contents_full(other_path, bytes, size,
					       G_FILE_SET_CONTENTS_NONE, 0600,
					       &error));
	g_assert_no_error(error);
	g_assert_null(line_offline_load_outbox(second, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	fixture->store = line_offline_open(fixture->dir, account, key);
	g_autoptr(J) loaded = line_offline_load_outbox(fixture->store, &error);

	g_assert_no_error(error);
	g_assert_true(json_object_equal(rows, loaded));
	g_assert_true(line_offline_clear_all(fixture->dir, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(other_path, G_FILE_TEST_EXISTS));
}

static void add_message(J *rows, J *raw, guint n)
{
	g_autofree char *id = g_strdup_printf("%u", n);
	J *row = obj();
	J *message = obj();
	const char chunk[] = { '\0', '\xff', '\r', '\n' };

	json_object_object_add(row, "id", json_object_new_string(id));
	json_object_object_add(row, "time", json_object_new_int64(n));
	json_object_object_add(row, "type", json_object_new_string("text"));
	json_object_object_add(
		row, "text", json_object_new_string("fixture-private-message"));
	json_object_object_add(message, "4", json_object_new_string(id));
	json_object_object_add(message, "5", json_object_new_int64(n));
	json_object_object_add(message, "6", json_object_new_int64(n));
	json_object_object_add(message, "20",
			       json_object_new_string_len(chunk,
							  sizeof(chunk)));
	json_object_object_add(raw, id, message);
	json_object_array_add(rows, row);
}

static void cached_history(struct fixture *fixture, gconstpointer unused)
{
	g_autoptr(J) incoming = json_object_new_array();
	g_autoptr(J) raw = obj();
	g_autoptr(J) head = obj();
	g_autoptr(J) chats = json_object_new_array();
	g_autoptr(J) profile = json_tokener_parse("{\"name\":\"Fixture\"}");

	(void)unused;
	for (guint i = 1; i <= 70; i++)
		add_message(incoming, raw, i);
	g_assert_true(line_offline_save_profile(fixture->store, profile));
	json_object_object_add(head, "id", json_object_new_string(chat));
	json_object_object_add(head, "lastId", json_object_new_string("70"));
	json_object_object_add(
		head, "preview",
		json_object_new_string("fixture-private-message"));
	json_object_array_add(chats, json_object_get(head));
	g_assert_true(line_offline_save_chats(fixture->store, chats));
	g_assert_true(
		line_offline_save_history(fixture->store, chat, incoming, raw));
	/* A later fetch overlaps the previous page. Neither page duplicates. */
	g_autoptr(J) overlap = json_object_new_array();
	g_autoptr(J) overlap_index = obj();

	for (guint i = 62; i <= 72; i++)
		add_message(overlap, overlap_index, i);
	g_assert_true(line_offline_save_history(fixture->store, chat, overlap,
						overlap_index));
	g_clear_pointer(&fixture->store, line_offline_close);
	fixture->store = line_offline_open(fixture->dir, account, key);
	g_autoptr(J) restored_profile =
		line_offline_load_profile(fixture->store);
	g_autoptr(J) index = NULL;
	gboolean more = FALSE;

	g_autoptr(J) latest = line_offline_load_history(fixture->store, chat,
							NULL, &index, &more);

	g_assert_true(json_object_equal(profile, restored_profile));
	g_assert_cmpuint(json_object_array_length(latest), ==, 50);
	g_assert_cmpstr(strfield(json_object_array_get_idx(latest, 0), "id"),
			==, "23");
	g_assert_true(more);
	J *chunk = field(field(index, "70"), "20");

	g_assert_cmpuint(json_object_get_string_len(chunk), ==, 4);
	g_assert_cmpmem(json_object_get_string(chunk), 4, "\0\xff\r\n", 4);

	g_autoptr(J) older_index = NULL;
	g_autoptr(J) older = line_offline_load_history(
		fixture->store, chat, "23", &older_index, &more);

	g_assert_false(more);
	g_assert_cmpuint(json_object_array_length(older), ==, 22);
	g_assert_nonnull(field(older_index, "1"));
	g_assert_null(line_offline_load_history(fixture->store, chat, "300",
						NULL, NULL));

	line_offline_unsend(fixture->store, chat, "70", 70);
	g_assert_true(line_offline_save_history(fixture->store, chat, overlap,
						overlap_index));
	g_autoptr(J) clean_index = NULL;
	g_autoptr(J) clean = line_offline_load_history(
		fixture->store, chat, NULL, &clean_index, NULL);
	J *tombstone = json_object_array_get_idx(clean, 47);

	g_assert_cmpstr(strfield(tombstone, "id"), ==, "70");
	g_assert_cmpstr(strfield(tombstone, "type"), ==, "unsent");
	g_assert_nonnull(field(field(clean_index, "70"), "6"));
	g_assert_null(field(field(clean_index, "70"), "20"));
	g_autoptr(J) clean_chats = line_offline_load_chats(fixture->store);
	J *clean_head = json_object_array_get_idx(clean_chats, 0);

	g_assert_null(field(clean_head, "preview"));
	g_assert_true(json_object_get_boolean(field(clean_head, "lastUnsent")));
	line_offline_drop_history(fixture->store, chat);
	g_assert_null(line_offline_load_history(fixture->store, chat, NULL,
						NULL, NULL));
}

static void archive_limit(struct fixture *fixture, gconstpointer unused)
{
	g_autoptr(J) incoming = json_object_new_array();
	g_autoptr(J) raw = obj();

	(void)unused;
	for (guint i = 1; i <= 300; i++)
		add_message(incoming, raw, i);
	g_assert_true(
		line_offline_save_history(fixture->store, chat, incoming, raw));
	/* Only the newest 256 remain, starting at message 45. */
	g_assert_null(line_offline_load_history(fixture->store, chat, "44",
						NULL, NULL));
	g_autoptr(J) first = line_offline_load_history(fixture->store, chat,
						       "46", NULL, NULL);

	g_assert_cmpuint(json_object_array_length(first), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(first, 0), "id"), ==,
			"45");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add("/offline/journal", struct fixture, NULL, setup, journal,
		   teardown);
	g_test_add("/offline/history", struct fixture, NULL, setup,
		   cached_history, teardown);
	g_test_add("/offline/archive-limit", struct fixture, NULL, setup,
		   archive_limit, teardown);
	return g_test_run();
}
