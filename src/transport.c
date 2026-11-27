// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include "sandbox.h"
#include <curl/curl.h>
#include <string.h>

enum {
	BODY_LIMIT = 16 * 1024 * 1024,
	TOKEN_LIMIT = 16384,
	/* After EXCESSIVE_ACCESS, CONGESTION_CONTROL or SERVER_BUSY. */
	BUSY_COOLDOWN_SECONDS = 30
};

static const char origin[] = LINE_RPC_ORIGIN;
static const char refresh_path[] = "/EXT/auth/tokenrefresh/v1";

/*
 * Each thread that talks to LINE keeps its own handle and connection pool
 * in GNU thread-local storage: the worker's RPCs never wait behind the
 * sync thread's long poll. Resetting options after each transfer removes
 * every borrowed pointer while retaining the connection.
 */
static __thread CURL *client;

int line_transport_retry_after(void)
{
	return line_cooldown_seconds(origin);
}

/* Talk and sync answers are TalkExceptions; other services number apart. */
static gboolean talk_path(const char *path)
{
	return !strcmp(path, "/S4") || !strcmp(path, "/SYNC4");
}

void line_transport_release(void)
{
	if (client)
		curl_easy_cleanup(client);
	client = NULL;
}

struct http_response {
	GByteArray *body;
	char *next_token;
	gboolean invalid_rotation;
};

static gboolean printable_token(const char *data, size_t n)
{
	if (!data || n == 0 || n > TOKEN_LIMIT)
		return FALSE;
	for (size_t i = 0; i < n; ++i)
		if ((guint8)data[i] < 33 || (guint8)data[i] > 126)
			return FALSE;
	return TRUE;
}

static size_t read_body(char *data, size_t size, size_t count, void *user)
{
	struct http_response *r = user;

	if (size && count > (BODY_LIMIT - r->body->len) / size)
		return 0;
	size_t n = size * count;

	g_byte_array_append(r->body, (const guint8 *)data, n);
	return n;
}

static size_t read_header(char *data, size_t size, size_t count, void *user)
{
	struct http_response *r = user;
	size_t n = size * count;
	static const char header[] = "x-line-next-access:";

	if (n >= 5 && !g_ascii_strncasecmp(data, "HTTP/", 5)) {
		line_secret_set(&r->next_token, NULL);
		r->invalid_rotation = FALSE;
	} else if (n >= sizeof(header) - 1 &&
		   !g_ascii_strncasecmp(data, header, sizeof(header) - 1)) {
		const char *begin = data + sizeof(header) - 1, *end = data + n;

		while (begin < end && (*begin == ' ' || *begin == '\t'))
			++begin;
		while (end > begin && (end[-1] == '\r' || end[-1] == '\n' ||
				       end[-1] == ' ' || end[-1] == '\t'))
			--end;
		if (r->next_token || !printable_token(begin, end - begin))
			r->invalid_rotation = TRUE;
		else
			r->next_token = g_strndup(begin, end - begin);
	}
	return n;
}

static gboolean append_header(struct curl_slist **headers, const char *value)
{
	struct curl_slist *list = curl_slist_append(*headers, value);

	if (!list)
		return FALSE;
	*headers = list;
	return TRUE;
}

static gboolean service_path(const char *path)
{
	if (!path || path[0] != '/')
		return FALSE;
	for (const char *p = path + 1; *p; ++p)
		if (!g_ascii_isalnum(*p) && *p != '/' && *p != '_' && *p != '-')
			return FALSE;
	return TRUE;
}

/* An encoded request only exchange() holds: wiped, then released. */
typedef GBytes RequestPacket;

static void request_packet_free(RequestPacket *packet)
{
	gsize n;
	const void *data = g_bytes_get_data(packet, &n);

	if (data)
		explicit_bzero((void *)data, n);
	g_bytes_unref(packet);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RequestPacket, request_packet_free)

enum protocol {
	COMPACT_RPC,
	BINARY_AUTH,
	JSON_APPROVAL,
};

static J *exchange(Line *s, const char *path, const char *method, J *args,
		   const char *access, int poll_seconds, enum protocol protocol,
		   GError **error)
{
	s->service_code = -1;
	s->http_status = 0;
	/*
	 * Once LINE has refused this session's credentials, nothing more goes
	 * out with them, as in LINE's own client, until the user pairs again.
	 */
	if (s->denied && access && !g_strcmp0(access, s->access)) {
		g_set_error_literal(error, LINE_ERROR, LINE_SESSION_EXPIRED,
				    "LINE ended this desktop session.");
		return NULL;
	}
	if ((!service_path(path) &&
	     (protocol != BINARY_AUTH ||
	      g_strcmp0(path, "/api/v3/TalkService.do"))) ||
	    poll_seconds < 0 || poll_seconds > 600 ||
	    (access && !printable_token(access, strlen(access)))) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid LINE request parameters.");
		return NULL;
	}
	if (line_cooldown_check(origin, error))
		return NULL;
	/* The encoded request can hold a refresh token: wiped when done. */
	g_autoptr(RequestPacket) packet = NULL;

	if (protocol != JSON_APPROVAL) {
		packet = protocol == BINARY_AUTH ?
				 line_encode_auth(method, args, error) :
				 line_encode_call(method, args, error);
		if (!packet)
			return NULL;
	}
	if (line_curl_global() && !client)
		client = curl_easy_init();
	if (!client) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Could not initialize HTTPS.");
		return NULL;
	}

	g_autofree char *url = g_strconcat(origin, path, NULL);
	g_autoptr(LineSecret) authorization =
		access ? g_strconcat("x-line-access: ", access, NULL) : NULL;
	g_autofree char *poll =
		poll_seconds && protocol == COMPACT_RPC ?
			g_strdup_printf("x-lst: %d", poll_seconds * 1000) :
			NULL;
	/* Replies can carry tokens, keys and messages: wiped when done. */
	g_autoptr(LineSecretBytes) data = g_byte_array_new();
	struct http_response response = { .body = data };
	struct curl_slist *headers = NULL;
	char tag[17];

	line_language_tag(tag);
	g_autofree char *language = g_strconcat("x-lal: ", tag, NULL);
	const char *const base_headers[] = { LINE_APP_HEADER, language,
					     "x-lpv: 1", "Expect:" };
	CURLcode result = CURLE_OUT_OF_MEMORY;
	long status = 0;
	gsize bytes = 0;
	const void *body = packet ? g_bytes_get_data(packet, &bytes) : NULL;

	for (size_t i = 0; i < G_N_ELEMENTS(base_headers); ++i)
		if (!append_header(&headers, base_headers[i]))
			goto cleanup;
	if (protocol == JSON_APPROVAL) {
		if (!append_header(&headers, "x-lhm: GET") ||
		    !append_header(&headers, "Accept: application/json"))
			goto cleanup;
	} else if (!append_header(&headers, "x-lhm: POST") ||
		   !append_header(&headers, "Accept: application/x-thrift") ||
		   !append_header(&headers,
				  "Content-Type: application/x-thrift")) {
		goto cleanup;
	}
	if ((authorization && !append_header(&headers, authorization)) ||
	    (poll && !append_header(&headers, poll)))
		goto cleanup;

#define SET(opt, val)                                        \
	do {                                                 \
		result = curl_easy_setopt(client, opt, val); \
		if (result != CURLE_OK)                      \
			goto cleanup;                        \
	} while (0)
	SET(CURLOPT_URL, url);
	SET(CURLOPT_NOSIGNAL, 1L);
	SET(CURLOPT_CONNECTTIMEOUT, 10L);
	SET(CURLOPT_TIMEOUT, poll_seconds ? poll_seconds + 5L : 30L);
	if (!strcmp(method, "getLastOpRevision"))
		SET(CURLOPT_TIMEOUT_MS, 1800L);
	SET(CURLOPT_PROTOCOLS_STR, "https");
	SET(CURLOPT_FOLLOWLOCATION, 0L);
	SET(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
	if (!line_curl_tune(client, TRUE)) {
		result = CURLE_FAILED_INIT;
		goto cleanup;
	}
	SET(CURLOPT_USERAGENT, LINE_USER_AGENT);
	SET(CURLOPT_ACCEPT_ENCODING, "gzip");
	SET(CURLOPT_HTTPHEADER, headers);
	if (packet) {
		SET(CURLOPT_POST, 1L);
		SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bytes);
		SET(CURLOPT_POSTFIELDS, body);
	}
	SET(CURLOPT_WRITEFUNCTION, read_body);
	SET(CURLOPT_WRITEDATA, &response);
	SET(CURLOPT_HEADERFUNCTION, read_header);
	SET(CURLOPT_HEADERDATA, &response);
#undef SET
	result = curl_easy_perform(client);
	if (result == CURLE_OK)
		result = curl_easy_getinfo(client, CURLINFO_RESPONSE_CODE,
					   &status);
	if (!result)
		line_cooldown_note(origin, client, status);

cleanup:
	curl_easy_reset(client);
	line_curl_headers_free(headers);
	g_autoptr(LineSecret) next = response.next_token;

	if (result != CURLE_OK) {
		g_set_error(error, LINE_ERROR,
			    result == CURLE_OPERATION_TIMEDOUT ? LINE_TIMEOUT :
								 1,
			    "LINE HTTPS request failed: %s.",
			    curl_easy_strerror(result));
		return NULL;
	}
	s->http_status = status;
	if (status < 200 || status >= 300) {
		/*
		 * LINE ends both approval polls and idle /SYNC4 long polls
		 * with HTTP 410. Non-polling endpoints keep their HTTP error.
		 */
		gboolean long_poll = protocol == JSON_APPROVAL ||
				     !strcmp(path, "/acct/lp/lgn/sq/v1") ||
				     !strcmp(path, "/SYNC4");
		gboolean timed_out =
			status == 408 || status == 504 ||
			(status == 410 && poll_seconds && long_poll);
		int code = timed_out ? LINE_TIMEOUT : 1;

		if (status == 429 || status == 503)
			code = LINE_RATE_LIMITED;
		else if (status == 401 || status == 403)
			code = LINE_ACCESS_DENIED;
		g_set_error(error, LINE_ERROR, code,
			    "LINE returned HTTP %ld for %s.", status, method);
		return NULL;
	}
	if (!data->len) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "LINE returned an empty reply.");
		return NULL;
	}
	if (protocol == JSON_APPROVAL) {
		json_tokener *parser = json_tokener_new();
		guint8 terminator = 0;

		g_byte_array_append(data, &terminator, 1);
		json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
		J *approval = json_tokener_parse_ex(
			parser, (const char *)data->data, data->len);
		gboolean valid =
			json_tokener_get_error(parser) ==
				json_tokener_success &&
			json_object_is_type(approval, json_type_object);

		json_tokener_free(parser);
		if (valid)
			return approval;
		json_object_put(approval);
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned an invalid approval response.");
		return NULL;
	}
	/* An over-read in the decoder faults instead of reading the heap. */
	struct line_guarded window;
	const guint8 *input = line_guard_copy(&window, data->data, data->len) ?
				      window.data :
				      data->data;
	J *reply = protocol == BINARY_AUTH ?
			   line_decode_auth(method, input, data->len,
					    &s->service_code, error) :
			   line_decode_reply(method, input, data->len,
					     &s->service_code, error);

	line_guard_free(&window);
	gboolean own = access && s->access && !g_strcmp0(access, s->access);

	if (!reply && talk_path(path) && line_busy_code(s->service_code))
		line_cooldown_set(origin, BUSY_COOLDOWN_SECONDS);
	if (!reply && own && talk_path(path) &&
	    line_auth_rejected(s->service_code))
		s->denied = TRUE;
	/*
	 * A pairing poll carries its own temporary token. Only an authenticated
	 * call with the current account token may rotate that account's token.
	 * A refresh uses the new token in its validated response body.
	 */
	if (strcmp(path, refresh_path) && (reply || s->service_code >= 0) &&
	    next && !response.invalid_rotation && access &&
	    !g_strcmp0(access, s->access) && strcmp(access, next)) {
		line_secret_set(&s->access, g_steal_pointer(&next));
		s->token_changed = TRUE;
	}
	return reply;
}

J *line_call(Line *s, const char *path, const char *method, J *args,
	     const char *access, int poll_seconds, GError **error)
{
	return exchange(s, path, method, args, access, poll_seconds,
			COMPACT_RPC, error);
}

J *line_auth_call(Line *s, const char *path, const char *method, J *args,
		  GError **error)
{
	return exchange(s, path, method, args, NULL, 0, BINARY_AUTH, error);
}

J *line_auth_wait(Line *s, const char *verifier, GError **error)
{
	return exchange(s, "/LF1", "phone verification", NULL, verifier, 180,
			JSON_APPROVAL, error);
}

void line_schedule_refresh(Line *s, J *duration, J *issued)
{
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 seconds = json_object_is_type(duration, json_type_int) ?
				 json_object_get_int64(duration) :
				 0;
	gint64 start = json_object_is_type(issued, json_type_int) ?
			       json_object_get_int64(issued) :
			       now;

	/* Ignore implausible clocks; LINE's own value is days to months. */
	if (start < now - 366 * 86400 || start > now + 86400)
		start = now;
	s->refresh_at = seconds >= 60 && seconds <= 10 * 366 * 86400 ?
				start + seconds :
				0;
	s->token_changed = TRUE;
}

gboolean line_refresh_due(const Line *s, gint64 now)
{
	/* An unknown schedule (an older saved session) renews right away. */
	return s->refresh && *s->refresh &&
	       (!s->refresh_at || now >= s->refresh_at);
}

gint64 line_retry_delay(guint failures, gint64 first, gint64 cap)
{
	gint64 delay = first;

	for (guint i = 1; i < failures && delay < cap; ++i)
		delay *= 2;
	return MIN(delay, cap);
}

static gboolean refresh_access(Line *s, GError **error)
{
	g_autoptr(J) args = obj();
	J *request = obj();

	J *sent = json_object_new_string(s->refresh);

	json_object_object_add(args, "1:obj", request);
	json_object_object_add(request, "1:str", sent);
	g_autoptr(J) reply = line_call(s, refresh_path, "refresh", args,
				       s->access, 0, error);

	line_secret_json_wipe(sent);
	if (!reply) {
		/*
		 * The refresh endpoint has its own error enum. 1000 is INVALID_REQUEST
		 * (unusable refresh token), while 1001 tells us to retry later.
		 */
		if (s->service_code == 1000) {
			g_clear_error(error);
			g_set_error_literal(
				error, LINE_ERROR, LINE_SESSION_EXPIRED,
				"LINE no longer accepts this session's refresh token.");
			s->denied = TRUE;
		}
		return FALSE;
	}
	J *access = field(reply, "1"), *refresh = field(reply, "5");

	if (!json_object_is_type(access, json_type_string) ||
	    !printable_token(json_object_get_string(access),
			     json_object_get_string_len(access)) ||
	    (refresh &&
	     (!json_object_is_type(refresh, json_type_string) ||
	      !printable_token(json_object_get_string(refresh),
			       json_object_get_string_len(refresh))))) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned an invalid token refresh response.");
		return FALSE;
	}
	line_secret_set(&s->access, g_strdup(json_object_get_string(access)));
	line_secret_json_wipe(access);
	if (refresh) {
		line_secret_set(&s->refresh,
				g_strdup(json_object_get_string(refresh)));
		line_secret_json_wipe(refresh);
	}
	line_schedule_refresh(s, field(reply, "2"), field(reply, "4"));
	/* Without LINE's hint, renew daily so the refresh token stays in use. */
	if (!s->refresh_at)
		s->refresh_at = g_get_real_time() / G_USEC_PER_SEC + 86400;
	return TRUE;
}

gboolean line_refresh_session(Line *s, GError **error)
{
	if (!s->refresh || !*s->refresh) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This session has no refresh token.");
		return FALSE;
	}
	return refresh_access(s, error);
}

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	g_autoptr(GError) first_error = NULL;
	J *reply = line_call(s, path, method, args, s->access, 0, &first_error);

	if (reply)
		return reply;
	if (s->service_code == 119 && strcmp(path, refresh_path)) {
		/*
		 * Retry only after an explicit token-expired rejection. Preserve the
		 * caller's request sequence and original encrypted chunks on a send.
		 */
		if (s->refresh && *s->refresh) {
			GError *refresh_error = NULL;

			if (!refresh_access(s, &refresh_error)) {
				/*
				 * Only refresh code 1000 ends the session. Other refresh
				 * codes come from a different enum than TalkException, so
				 * hide them from callers that classify Talk codes.
				 */
				if (!g_error_matches(refresh_error, LINE_ERROR,
						     LINE_SESSION_EXPIRED))
					s->service_code = -1;
				g_propagate_error(error, refresh_error);
				return NULL;
			}
			g_clear_error(&first_error);
			reply = line_call(s, path, method, args, s->access, 0,
					  &first_error);
			if (reply)
				return reply;
		}
		if (s->service_code == 119) {
			g_set_error_literal(
				error, LINE_ERROR, LINE_SESSION_EXPIRED,
				"LINE requires this computer to pair again.");
			s->denied = TRUE;
			return NULL;
		}
	}
	if (first_error)
		g_propagate_error(error, g_steal_pointer(&first_error));
	return NULL;
}
