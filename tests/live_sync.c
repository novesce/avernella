// SPDX-License-Identifier: GPL-3.0-or-later
#include "live_sync.h"
#include <poll.h>

#define TEST_TIMEOUT (5 * G_USEC_PER_SEC)

static GAsyncQueue *requests, *replies, *finished;

J *line_call(Line *s, const char *path, const char *method, J *args,
	     const char *access, int poll_seconds, GError **error)
{
	(void)path, (void)method, (void)args, (void)poll_seconds;
	static gint64 revision;

	g_async_queue_push(requests, g_strdup(access));
	g_autofree char *rotation =
		g_async_queue_timeout_pop(replies, TEST_TIMEOUT);

	g_assert_nonnull(rotation);
	if (!strcmp(rotation, "offline")) {
		g_set_error_literal(error, LINE_ERROR, LINE_RETRY, "Offline");
		return NULL;
	}
	if (!strcmp(rotation, "idle")) {
		J *response = obj();

		json_object_object_add(response, "1", obj());
		return response;
	}
	if (*rotation) {
		g_free(s->access);
		s->access = g_steal_pointer(&rotation);
		s->token_changed = TRUE;
	}
	J *response = obj(), *page = obj(), *op = obj();
	J *ops = json_object_new_array();

	json_object_object_add(op, "1", json_object_new_int64(++revision));
	json_object_array_add(ops, op);
	json_object_object_add(page, "1", ops);
	json_object_object_add(response, "1", page);
	return response;
}

int line_transport_retry_after(void)
{
	return 0;
}

gint64 line_retry_delay(guint failures, gint64 first, gint64 cap)
{
	(void)failures, (void)cap;
	return first;
}

void line_transport_release(void)
{
	g_async_queue_push(finished, GINT_TO_POINTER(1));
}

static void expect_request(const char *access)
{
	g_autofree char *token =
		g_async_queue_timeout_pop(requests, TEST_TIMEOUT);

	g_assert_cmpstr(token, ==, access);
}

static void expect_rotation(struct line_live_sync *sync, const char *from,
			    const char *to)
{
	struct pollfd fd = { .fd = line_live_sync_fd(sync), .events = POLLIN };
	struct line_live_batch batch;

	g_assert_cmpint(poll(&fd, 1, TEST_TIMEOUT / 1000), ==, 1);
	line_live_sync_take(sync, &batch);
	g_assert_cmpstr(batch.access_from, ==, from);
	g_assert_cmpstr(batch.access, ==, to);
	g_free(batch.access_from);
	g_free(batch.access);
	json_object_put(batch.ops);
}

static struct line_live_sync *start_sync(const char *access)
{
	g_autoptr(GError) error = NULL;
	struct line_talk_sync cursor = { 0 };

	requests = g_async_queue_new();
	replies = g_async_queue_new();
	finished = g_async_queue_new();
	struct line_live_sync *sync =
		line_live_sync_start(&cursor, access, &error);

	g_assert_no_error(error);
	g_assert_nonnull(sync);
	return sync;
}

static void stop_sync(struct line_live_sync *sync)
{
	line_live_sync_stop(sync);
	g_async_queue_push(replies, g_strdup(""));
	g_assert_nonnull(g_async_queue_timeout_pop(finished, TEST_TIMEOUT));
	g_async_queue_unref(finished);
	g_async_queue_unref(replies);
	g_async_queue_unref(requests);
}

static void stale_rotation(void)
{
	struct line_live_sync *sync = start_sync("original");

	expect_request("original");
	line_live_sync_set_access(sync, "renewed");
	g_async_queue_push(replies, g_strdup("stale-rotation"));
	expect_rotation(sync, NULL, NULL);
	expect_request("renewed");
	g_async_queue_push(replies, g_strdup("current-rotation"));
	expect_rotation(sync, "renewed", "current-rotation");
	expect_request("current-rotation");
	stop_sync(sync);
}

static void reconnect_without_new_messages(void)
{
	struct line_live_sync *sync = start_sync("token");
	struct line_live_batch batch;
	struct pollfd fd = { .fd = line_live_sync_fd(sync), .events = POLLIN };

	expect_request("token");
	g_async_queue_push(replies, g_strdup("offline"));
	g_assert_cmpint(poll(&fd, 1, TEST_TIMEOUT / 1000), ==, 1);
	line_live_sync_take(sync, &batch);
	g_assert_cmpint(batch.recovered_since, ==, 0);
	g_assert_null(batch.ops);

	expect_request("token");
	g_async_queue_push(replies, g_strdup("idle"));
	g_assert_cmpint(poll(&fd, 1, TEST_TIMEOUT / 1000), ==, 1);
	line_live_sync_take(sync, &batch);
	g_assert_cmpint(batch.recovered_since, >, 0);
	g_assert_cmpuint(json_object_array_length(batch.ops), ==, 0);
	json_object_put(batch.ops);

	line_live_sync_take(sync, &batch);
	g_assert_cmpint(batch.recovered_since, ==, 0);
	expect_request("token");
	stop_sync(sync);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/live-sync/ignores-stale-token-rotation",
			stale_rotation);
	g_test_add_func("/live-sync/recovers-without-new-messages",
			reconnect_without_new_messages);
	return g_test_run();
}
