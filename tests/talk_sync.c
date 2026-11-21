// SPDX-License-Identifier: GPL-3.0-or-later
#include "talk_sync.h"

struct received {
	int type[8];
	guint count;
};

static void operation(J *op, gpointer data)
{
	struct received *received = data;

	g_assert_cmpuint(received->count, <, G_N_ELEMENTS(received->type));
	received->type[received->count++] = json_object_get_int(field(op, "3"));
}

static gboolean apply(struct line_talk_sync *cursor, const char *json,
		      struct received *received, gboolean *reload)
{
	g_autoptr(J) response = json_tokener_parse(json);

	g_assert_nonnull(response);
	return line_talk_sync_apply(cursor, response, operation, received,
				    reload);
}

static void retry_incomplete(void)
{
	struct line_talk_sync cursor = { .revision = 100 };
	struct received received = { 0 };
	gboolean reload = FALSE;

	g_assert_false(apply(&cursor, "{\"1\":{\"1\":[],\"2\":true}}",
			     &received, &reload));
	g_assert_true(cursor.more);
	g_assert_cmpint(cursor.revision, ==, 100);
	g_assert_false(apply(&cursor, "{}", &received, &reload));
	g_assert_true(cursor.more);
	g_assert_false(apply(
		&cursor, "{\"1\":{\"1\":[{\"1\":103,\"3\":51},{\"3\":50}]}}",
		&received, &reload));
	g_assert_cmpint(cursor.revision, ==, 100);
	g_assert_cmpuint(received.count, ==, 0);

	g_autoptr(J) args = line_talk_sync_request(&cursor);
	J *request = field(args, "1:obj");

	g_assert_cmpint(json_object_get_int64(field(request, "1:i64")), ==,
			100);
	g_assert_true(apply(&cursor,
			    "{\"1\":{\"1\":[{\"1\":101,\"3\":50}],\"2\":true}}",
			    &received, &reload));
	g_assert_cmpint(cursor.revision, ==, 101);
	g_assert_false(reload);
	g_assert_false(apply(
		&cursor,
		"{\"1\":{\"1\":[{\"1\":103,\"3\":51},{\"1\":101,\"3\":50}],\"2\":false}}",
		&received, &reload));
	g_assert_cmpint(cursor.revision, ==, 103);
	g_assert_false(cursor.more);
	g_assert_cmpuint(received.count, ==, 2);
	g_assert_cmpint(received.type[0], ==, 50);
	g_assert_cmpint(received.type[1], ==, 51);
}

static void ordered_operations(void)
{
	struct line_talk_sync cursor = { .revision = 100 };
	struct received received = { 0 };
	gboolean reload = FALSE;

	g_assert_false(apply(
		&cursor,
		"{\"1\":{\"1\":[{\"1\":103,\"3\":51},{\"1\":102,\"3\":50}],"
		"\"3\":{\"2\":901},\"4\":{\"2\":902}}}",
		&received, &reload));
	g_assert_cmpuint(received.count, ==, 2);
	g_assert_cmpint(received.type[0], ==, 50);
	g_assert_cmpint(received.type[1], ==, 51);
	g_assert_cmpint(cursor.revision, ==, 103);
	g_assert_cmpint(cursor.global_revision, ==, 901);
	g_assert_cmpint(cursor.individual_revision, ==, 902);
	g_assert_false(apply(
		&cursor, "{\"1\":{\"3\":{\"2\":900},\"4\":{\"2\":\"904\"}}}",
		&received, &reload));
	g_assert_cmpint(cursor.global_revision, ==, 901);
	g_assert_cmpint(cursor.individual_revision, ==, 902);
	g_assert_cmpint(cursor.revision, ==, 103);
}

static void resync_pages(void)
{
	struct line_talk_sync cursor = { .revision = 100 };
	struct received received = { 0 };
	gboolean reload = FALSE;
	const char *partial = "{\"3\":{\"1\":{\"2\":9007199254740993}}}";

	g_assert_true(apply(&cursor, partial, &received, &reload));
	g_assert_true(reload);
	g_assert_true(cursor.more);
	g_assert_cmpint(cursor.revision, ==, 100);
	g_assert_false(apply(&cursor, partial, &received, &reload));
	g_assert_false(reload);
	g_assert_true(cursor.more);

	g_autoptr(J) args = line_talk_sync_request(&cursor);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) wire = line_encode_call("sync", args, &error);
	gsize len;

	g_assert_no_error(error);
	g_assert_nonnull(wire);
	const guint8 *bytes = g_bytes_get_data(wire, &len);

	g_assert_cmpuint(len, >, 8);
	g_autoptr(J) decoded = line_decode_struct(bytes + 8, len - 8, &error);
	J *ack = field(field(field(decoded, "1"), "6"), "2");

	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int64(ack), ==, 9007199254740993);
	g_assert_false(
		apply(&cursor, "{\"2\":{\"2\":97}}", &received, &reload));
	g_assert_cmpint(cursor.revision, ==, 100);
	g_assert_false(reload);
	g_assert_false(
		apply(&cursor, "{\"2\":{\"2\":105}}", &received, &reload));
	g_assert_cmpint(cursor.revision, ==, 105);
	g_assert_true(reload);
	g_assert_false(cursor.more);
	g_assert_cmpuint(received.count, ==, 0);

	line_talk_sync_clear(&cursor);
	g_assert_cmpint(cursor.revision, ==, -1);
	g_assert_null(cursor.partial);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/talk-sync/retry-incomplete", retry_incomplete);
	g_test_add_func("/talk-sync/ordered-operations", ordered_operations);
	g_test_add_func("/talk-sync/resync-pages", resync_pages);
	return g_test_run();
}
