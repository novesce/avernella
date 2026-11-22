// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Simulate HTTPS transfers, exercising the real Compact encoder/decoder and
 * refresh state machine without opening a network connection.
 */
#include "line.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <string.h>

static curl_write_callback receive_body;
static void *receive_data;
static int refresh_code, requests;
static gint64 refresh_duration, refresh_issued;
static long http_code = 200;
static curl_off_t retry_after;
static gboolean reject_replay;
static GPtrArray *sent_headers;
/* When nonnegative, every reply is this TalkException code. */
static int forced_code = -1;

CURLcode __wrap_curl_easy_setopt(CURL *easy, CURLoption option, ...)
{
	(void)easy;
	va_list values;

	va_start(values, option);
	if (option == CURLOPT_WRITEFUNCTION)
		receive_body = va_arg(values, curl_write_callback);
	if (option == CURLOPT_WRITEDATA)
		receive_data = va_arg(values, void *);
	if (option == CURLOPT_HTTPHEADER) {
		if (!sent_headers)
			sent_headers = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_set_size(sent_headers, 0);
		for (struct curl_slist *h = va_arg(values, struct curl_slist *);
		     h; h = h->next)
			g_ptr_array_add(sent_headers, g_strdup(h->data));
	}
	va_end(values);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_getinfo(CURL *easy, CURLINFO info, ...)
{
	(void)easy;
	va_list values;

	va_start(values, info);
	if (info == CURLINFO_RETRY_AFTER)
		*va_arg(values, curl_off_t *) = retry_after;
	else {
		g_assert_cmpint(info, ==, CURLINFO_RESPONSE_CODE);
		*va_arg(values, long *) = http_code;
	}
	va_end(values);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_perform(CURL *easy)
{
	(void)easy;
	requests++;
	gboolean refresh = requests == 2 && forced_code < 0;

	if (refresh && refresh_code < 0)
		return CURLE_OPERATION_TIMEDOUT;
	gint code = forced_code >= 0		   ? forced_code :
		    refresh			   ? refresh_code :
		    requests == 1 || reject_replay ? 119 :
						     0;
	g_autoptr(J) fields = obj();
	J *value = obj();

	json_object_object_add(fields, code ? "1:obj" : "0:obj", value);
	if (code)
		json_object_object_add(value, "1:i32",
				       json_object_new_int(code));
	else if (refresh) {
		/* RefreshAccessTokenResponse: 2 duration, 4 issue time. */
		json_object_object_add(value, "1:str",
				       json_object_new_string("renewed"));
		json_object_object_add(value, "2:i64",
				       json_object_new_int64(refresh_duration));
		json_object_object_add(value, "4:i64",
				       json_object_new_int64(refresh_issued));
		json_object_object_add(value, "5:str",
				       json_object_new_string("next-refresh"));
	}
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) frame = line_encode_call(
		refresh ? "refresh" : "getProfile", fields, &error);
	g_assert_no_error(error);
	gsize size;
	const guint8 *bytes = g_bytes_get_data(frame, &size);

	g_autofree guint8 *reply = g_memdup2(bytes, size);

	reply[1] = 0x41; /* REPLY */
	g_assert_cmpuint(receive_body((char *)reply, 1, size, receive_data), ==,
			 size);
	return CURLE_OK;
}

static void expect(int code, gboolean with_token, gboolean expires_again,
		   gboolean success, int expected_error, int expected_calls)
{
	Line s = { .access = g_strdup("old"),
		   .refresh = with_token ? g_strdup("refresh") : NULL };
	g_autoptr(GError) error = NULL;
	requests = 0;
	refresh_code = code;
	reject_replay = expires_again;
	g_autoptr(J) response = line_rpc(&s, "/S4", "getProfile", NULL, &error);
	g_assert_cmpint(response != NULL, ==, success);
	if (success)
		g_assert_no_error(error);
	else
		g_assert_error(error, LINE_ERROR, expected_error);
	g_assert_cmpint(requests, ==, expected_calls);
	g_free(s.access);
	g_free(s.refresh);
}

static void refresh_errors(void)
{
	expect(1000, TRUE, FALSE, FALSE, LINE_SESSION_EXPIRED,
	       2); /* invalid refresh */
	expect(1001, TRUE, FALSE, FALSE, 1, 2); /* retry later */
	expect(-1, TRUE, FALSE, FALSE, LINE_TIMEOUT, 2); /* offline */
	expect(0, FALSE, FALSE, FALSE, LINE_SESSION_EXPIRED,
	       1); /* no refresh token */
	expect(0, TRUE, TRUE, FALSE, LINE_SESSION_EXPIRED,
	       3); /* refreshed token refused */
	expect(0, TRUE, FALSE, TRUE, 0, 3);
	line_transport_release();
}

static void scheduled_refresh(void)
{
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	Line s = { .access = g_strdup("old"), .refresh = g_strdup("refresh") };

	g_autoptr(GError) error = NULL;

	/* The first request is the refresh itself. */
	requests = 1;
	refresh_code = 0;
	refresh_duration = 7 * 86400;
	refresh_issued = now - 60;
	g_assert_true(line_refresh_session(&s, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(s.access, ==, "renewed");
	g_assert_cmpstr(s.refresh, ==, "next-refresh");
	g_assert_cmpint(s.refresh_at, ==, refresh_issued + refresh_duration);
	g_assert_true(s.token_changed);

	/* Without a usable hint, renew again within a day. */
	requests = 1;
	refresh_duration = 0;
	g_assert_true(line_refresh_session(&s, &error));
	g_assert_cmpint(s.refresh_at, >=, now + 86400);
	g_assert_cmpint(s.refresh_at, <=, now + 86400 + 5);

	/* Pairing's TokenV3IssueResult uses the same schedule. */
	g_autoptr(J) duration = json_object_new_int64(3600);
	line_schedule_refresh(&s, duration, NULL);
	g_assert_cmpint(s.refresh_at, >=, now + 3600);
	g_assert_cmpint(s.refresh_at, <=, now + 3605);
	line_schedule_refresh(&s, NULL, NULL);
	g_assert_cmpint(s.refresh_at, ==, 0);
	g_free(s.access);
	g_free(s.refresh);
	refresh_duration = refresh_issued = 0;
	line_transport_release();
}

/*
 * A refresh failure that is not LINE's explicit rejection must never look
 * like a TalkException code, which the worker would treat as a sign-out.
 */
static void refresh_failure_keeps_session(void)
{
	static const int codes[] = { 1, 7, 8, 14, 17, 1001, -1 };

	for (guint i = 0; i < G_N_ELEMENTS(codes); ++i) {
		Line s = { .access = g_strdup("old"),
			   .refresh = g_strdup("refresh") };
		g_autoptr(GError) error = NULL;

		requests = 0;
		refresh_code = codes[i];
		reject_replay = FALSE;
		g_assert_null(line_rpc(&s, "/S4", "getProfile", NULL, &error));
		g_assert_nonnull(error);
		g_assert_false(g_error_matches(error, LINE_ERROR,
					       LINE_SESSION_EXPIRED));
		g_assert_cmpint(s.service_code, ==, -1);
		/* The saved tokens are untouched for the next attempt. */
		g_assert_cmpstr(s.access, ==, "old");
		g_assert_cmpstr(s.refresh, ==, "refresh");
		g_free(s.access);
		g_free(s.refresh);
	}
	line_transport_release();
}

static void renewal_schedule(void)
{
	Line s = { .refresh = (char *)"refresh" };

	/* An older saved session without a schedule renews at once. */
	g_assert_true(line_refresh_due(&s, 1000));
	s.refresh_at = 2000;
	g_assert_false(line_refresh_due(&s, 1999));
	g_assert_true(line_refresh_due(&s, 2000));
	g_assert_true(line_refresh_due(&s, 900000));
	s.refresh = (char *)"";
	g_assert_false(line_refresh_due(&s, 900000));
	s.refresh = NULL;
	g_assert_false(line_refresh_due(&s, 900000));

	/* Backoff doubles up to its cap and stays there indefinitely. */
	g_assert_cmpint(line_retry_delay(0, 120, 3600), ==, 120);
	g_assert_cmpint(line_retry_delay(1, 120, 3600), ==, 120);
	g_assert_cmpint(line_retry_delay(2, 120, 3600), ==, 240);
	g_assert_cmpint(line_retry_delay(5, 120, 3600), ==, 1920);
	g_assert_cmpint(line_retry_delay(6, 120, 3600), ==, 3600);
	g_assert_cmpint(line_retry_delay(G_MAXUINT, 120, 3600), ==, 3600);
}

static void poll_timeout(void)
{
	Line s = { 0 };

	g_autoptr(GError) error = NULL;
	http_code = 410;
	requests = 0;
	refresh_code = 0;
	g_assert_null(line_call(&s, "/acct/lp/lgn/sq/v1", "checkQrCodeVerified",
				NULL, "fixture", 30, &error));
	g_assert_error(error, LINE_ERROR, LINE_TIMEOUT);
	g_clear_error(&error);
	g_assert_null(line_auth_wait(&s, "fixture", &error));
	g_assert_error(error, LINE_ERROR, LINE_TIMEOUT);
	g_clear_error(&error);
	g_assert_null(
		line_call(&s, "/SYNC4", "sync", NULL, "fixture", 50, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_null(
		line_call(&s, "/S4", "getProfile", NULL, "fixture", 0, &error));
	g_assert_error(error, LINE_ERROR, 1);
	http_code = 200;
	line_transport_release();
}

static void rate_limit(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, 0);
		g_test_trap_assert_passed();
		return;
	}
	Line client = { 0 }, poll = { 0 };
	g_autoptr(GError) error = NULL;

	http_code = 429;
	retry_after = 90;
	requests = 0;
	g_assert_null(line_rpc(&client, "/S4", "getProfile", NULL, &error));
	g_assert_error(error, LINE_ERROR, LINE_RATE_LIMITED);
	g_assert_cmpint(line_transport_retry_after(), >=, 89);
	g_clear_error(&error);

	http_code = 200;
	g_assert_null(line_call(&poll, "/SYNC4", "sync", NULL, "fixture", 50,
				&error));
	g_assert_error(error, LINE_ERROR, LINE_RATE_LIMITED);
	g_assert_cmpint(requests, ==, 1);
	line_transport_release();
}

static gboolean sent(const char *header)
{
	for (guint i = 0; sent_headers && i < sent_headers->len; i++)
		if (!strcmp(g_ptr_array_index(sent_headers, i), header))
			return TRUE;
	return FALSE;
}

/* One identity on every request, with the desktop's own language tag. */
static void identity_headers(void)
{
	Line client = { .access = (char *)"fixture" };
	g_autoptr(GError) error = NULL;
	const char *const cases[][2] = { { "th_TH.UTF-8", "x-lal: th_TH" },
					 { "ja_JP", "x-lal: ja_JP" },
					 { "C", "x-lal: en_US" } };

	http_code = 500;
	for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_setenv("LANGUAGE", cases[i][0], TRUE);
		g_assert_null(line_call(&client, "/S4", "getProfile", NULL,
					client.access, 0, &error));
		g_clear_error(&error);
		g_assert_true(sent(LINE_APP_HEADER));
		g_assert_true(sent(cases[i][1]));
	}
	g_unsetenv("LANGUAGE");
	http_code = 200;
	line_transport_release();
}

/* HTTP 401 and 403 are LINE refusing the credentials, not a network fault. */
static void access_denied(void)
{
	Line client = { .access = (char *)"fixture" };
	g_autoptr(GError) error = NULL;

	http_code = 403;
	requests = 0;
	g_assert_null(line_rpc(&client, "/S4", "getProfile", NULL, &error));
	g_assert_error(error, LINE_ERROR, LINE_ACCESS_DENIED);
	g_assert_cmpint(client.http_status, ==, 403);
	g_assert_cmpint(requests, ==, 1);
	http_code = 200;
	line_transport_release();
}

/*
 * After NOT_AUTHORIZED_DEVICE nothing more goes out with that token, as in
 * LINE's own client; other tokens (a pairing session) are unaffected.
 */
static void denial_latch(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, 0);
		g_test_trap_assert_passed();
		return;
	}
	Line client = { .access = (char *)"fixture" };
	g_autoptr(GError) error = NULL;

	forced_code = 8;
	requests = 0;
	g_assert_null(line_rpc(&client, "/S4", "getProfile", NULL, &error));
	g_assert_true(client.denied);
	g_assert_cmpint(requests, ==, 1);
	g_clear_error(&error);
	g_assert_null(line_rpc(&client, "/S4", "getProfile", NULL, &error));
	g_assert_error(error, LINE_ERROR, LINE_SESSION_EXPIRED);
	g_assert_cmpint(requests, ==, 1);
	g_clear_error(&error);
	g_assert_null(line_call(&client, "/acct/lp/lgn/sq/v1",
				"checkQrCodeVerified", NULL, "pairing", 1,
				&error));
	g_assert_cmpint(requests, ==, 2);
	line_transport_release();
}

/* EXCESSIVE_ACCESS pauses every RPC to legy, but not other hosts. */
static void busy_cooldown(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, 0);
		g_test_trap_assert_passed();
		return;
	}
	Line client = { .access = (char *)"fixture" };
	g_autoptr(GError) error = NULL;

	forced_code = 4;
	requests = 0;
	g_assert_null(line_rpc(&client, "/S4", "getProfile", NULL, &error));
	g_assert_false(client.denied);
	g_assert_cmpint(line_transport_retry_after(), >=, 29);
	g_clear_error(&error);
	g_assert_null(line_rpc(&client, "/S4", "getProfile", NULL, &error));
	g_assert_error(error, LINE_ERROR, LINE_RATE_LIMITED);
	g_assert_cmpint(requests, ==, 1);
	g_assert_cmpint(line_cooldown_seconds("https://obs.line-apps.com/r"),
			==, 0);
	/* A cooldown is per host and ignores the path and letter case. */
	line_cooldown_set("https://OBS.line-apps.com/r/talk/m/1", 60);
	g_assert_cmpint(line_cooldown_seconds("https://obs.line-apps.com/x"),
			>=, 59);
	line_transport_release();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/transport/refresh-classification", refresh_errors);
	g_test_add_func("/transport/scheduled-refresh", scheduled_refresh);
	g_test_add_func("/transport/refresh-failure-keeps-session",
			refresh_failure_keeps_session);
	g_test_add_func("/transport/renewal-schedule", renewal_schedule);
	g_test_add_func("/transport/qr-poll-timeout", poll_timeout);
	g_test_add_func("/transport/identity-headers", identity_headers);
	g_test_add_func("/transport/access-denied", access_denied);
	g_test_add_func("/transport/denial-latch", denial_latch);
	g_test_add_func("/transport/busy-cooldown", busy_cooldown);
	g_test_add_func("/transport/retry-after", rate_limit);
	return g_test_run();
}
