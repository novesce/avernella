// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"

static void visible_only(void)
{
	Interface view = { .connected = TRUE,
			   .active = TRUE,
			   .stick_bottom = TRUE,
			   .history_offset = 80,
			   .max_history_offset = 80 };
	const Uint32 front = SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;

	g_assert_true(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
	g_assert_false(
		ui_read_visible(&view, front, TRUE, TRUE, TRUE)); /* overlay */
	g_assert_false(
		ui_read_visible(&view, front, FALSE, FALSE,
				TRUE)); /* loading/failed/stale history */
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE,
				       FALSE)); /* compact list or friends */
	g_assert_false(
		ui_read_visible(&view, SDL_WINDOW_SHOWN, FALSE, TRUE, TRUE));
	g_assert_false(ui_read_visible(&view, front | SDL_WINDOW_MINIMIZED,
				       FALSE, TRUE, TRUE));
	g_assert_false(ui_read_visible(&view, front | SDL_WINDOW_HIDDEN, FALSE,
				       TRUE, TRUE));
	view.active = FALSE;
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
	view.active = TRUE;
	view.older_page = TRUE;
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
	view.older_page = FALSE;
	view.older_pending = TRUE;
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
	view.older_pending = FALSE;
	view.stick_bottom = FALSE;
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
	view.stick_bottom = TRUE;
	view.history_offset = 79;
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
	view.history_offset = 80;
	view.logging_out = TRUE;
	g_assert_false(ui_read_visible(&view, front, FALSE, TRUE, TRUE));
}

static void watermarks(void)
{
	UiReadCursor c = { 0 };
	guint64 id = ui_read_id("9007199254740993"),
		next = ui_read_id("9007199254740994");
	g_assert_cmpuint(id, ==, G_GUINT64_CONSTANT(9007199254740993));
	g_assert_cmpuint(ui_read_id("-1"), ==, 0);
	g_assert_cmpuint(ui_read_id("1x"), ==, 0);
	g_assert_cmpuint(ui_read_id("9223372036854775808"), ==, 0);
	g_assert_true(ui_read_present(&c, id, id - 1));
	g_assert_false(ui_read_present(&c, id, id - 1));
	g_assert_false(ui_read_present(&c, next, id - 1));
	g_assert_false(
		ui_read_finish(&c, next, TRUE, next)); /* different request */
	g_assert_cmpuint(c.pending, ==, id);
	g_assert_false(ui_read_finish(&c, id, TRUE,
				      next)); /* success for older viewport */
	g_assert_cmpuint(c.confirmed, ==, id);
	g_assert_false(ui_read_present(&c, id, id - 1));
	g_assert_true(ui_read_present(&c, next, id - 1));
	g_assert_false(ui_read_finish(&c, next, FALSE,
				      next)); /* ambiguous network failure */
	g_assert_cmpuint(c.confirmed, ==, id);
	g_assert_false(ui_read_present(&c, next, id - 1));
	c.attempted = c.confirmed; /* explicit refresh retries */
	g_assert_true(ui_read_present(&c, next, id - 1));
	g_assert_true(ui_read_finish(&c, next, TRUE, next));
	g_assert_cmpuint(c.confirmed, ==, next);
	g_assert_false(ui_read_present(&c, id, id - 1));
}

static void stale_counts(void)
{
	const guint64 head = 100, next = 101;
	UiReadCursor cursor = { 0 };

	/* A summary can report a count for a head already seen on the phone. */
	g_assert_true(ui_read_covers(NULL, head, head));
	g_assert_false(ui_read_covers(NULL, 0, head));
	g_assert_false(ui_read_covers(NULL, next, head));
	g_assert_false(ui_read_present(&cursor, head, head));
	g_assert_false(ui_read_present(&cursor, head, 0)); /* stale refresh */

	/* Clear the local badge at presentation, even during an RPC outage. */
	g_assert_true(ui_read_present(&cursor, next, head));
	g_assert_true(ui_read_covers(&cursor, next, head));
	g_assert_cmpuint(cursor.confirmed, ==, head);
	g_assert_false(ui_read_finish(&cursor, next, FALSE, next));
	g_assert_true(ui_read_covers(&cursor, next, 0));
	g_assert_false(ui_read_covers(&cursor, next + 1, head));
	/* The last failed RPC retries once on reopening the conversation. */
	cursor.attempted = cursor.confirmed;
	g_assert_true(ui_read_present(&cursor, next, head));
	g_assert_false(ui_read_present(&cursor, next, head));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ui-read/visible-latest-focused", visible_only);
	g_test_add_func("/ui-read/inflight-and-stale-ack", watermarks);
	g_test_add_func("/ui-read/visible-badge-and-server-watermark",
			stale_counts);
	return g_test_run();
}
