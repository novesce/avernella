// SPDX-License-Identifier: GPL-3.0-or-later
#include "square.h"
#include <curl/curl.h>

#define SEARCH_PATH "/lus/v2/search"
#define SEARCH_RESPONSE_LIMIT (4 * 1024 * 1024)

struct search_response {
	GByteArray *data;
};

static gpointer search_error(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return NULL;
}

static void put_str(J *object, const char *key, const char *value)
{
	json_object_object_add(object, key, json_object_new_string(value));
}

static void put_num(J *object, const char *key, gint64 value)
{
	json_object_object_add(object, key, json_object_new_int64(value));
}

static size_t search_length(J *value)
{
	return json_object_is_type(value, json_type_array) ?
		       json_object_array_length(value) :
		       0;
}

static size_t receive_search(char *data, size_t size, size_t count, void *user)
{
	struct search_response *response = user;
	size_t n = size * count;

	if (size &&
	    count > (SEARCH_RESPONSE_LIMIT - response->data->len) / size)
		return 0;
	g_byte_array_append(response->data, (const guint8 *)data, n);
	return n;
}

static gboolean append_header(struct curl_slist **headers, const char *value)
{
	struct curl_slist *next = curl_slist_append(*headers, value);

	if (!next)
		return FALSE;
	*headers = next;
	return TRUE;
}

static gboolean printable_token(const char *token)
{
	if (!token || !*token || strlen(token) > 16384)
		return FALSE;
	for (const char *p = token; *p; p++)
		if ((guint8)*p < 33 || (guint8)*p > 126)
			return FALSE;
	return TRUE;
}

static char *search_url(const char *query, const char *continuation)
{
	if (continuation && *continuation) {
		const char *path = continuation;

		if (*path == '?')
			return g_strconcat(LINE_RPC_ORIGIN, SEARCH_PATH, path,
					   NULL);
		if (g_str_has_prefix(path, "https://line.me/"))
			path += strlen("https://line.me/");
		if (g_str_has_prefix(path, "/"))
			path++;
		if (!g_str_has_prefix(path, "lus/v2/search?") ||
		    strstr(path, "://"))
			return NULL;
		return g_strconcat(LINE_RPC_ORIGIN, "/", path, NULL);
	}
	g_autofree char *escaped = g_uri_escape_string(query, NULL, TRUE);

	return g_strdup_printf("%s%s?q=%s&sscode=os.tab.square",
			       LINE_RPC_ORIGIN, SEARCH_PATH, escaped);
}

static char *search_info(Line *s, const char *region_code)
{
	char locale[17];

	line_language_tag(locale);
	const char *region = "US";

	if (region_code && strlen(region_code) == 2) {
		region = region_code;
	} else {
		const char *locale_region = strrchr(locale, '_');

		if (locale_region && strlen(locale_region + 1) == 2)
			region = locale_region + 1;
	}
	J *info = obj();

	put_str(info, "mid", s->mid);
	put_str(info, "version", "26.19.0");
	put_str(info, "type", "ANDROID");
	put_str(info, "phase", "RELEASE");
	put_str(info, "locale", locale);
	put_str(info, "region", region);
	char *json = g_strdup(
		json_object_to_json_string_ext(info, JSON_C_TO_STRING_PLAIN));

	json_object_put(info);
	return json;
}

static char *search_channel_token(Line *s, GError **error)
{
	J *args = obj();

	json_object_object_add(args, "1:str",
			       json_object_new_string("1557852768"));
	g_autoptr(J) reply = line_rpc(
		s, "/CH4", "approveChannelAndIssueChannelToken", args, error);
	if (!reply)
		return NULL;
	const char *token = strfield(reply, "5");

	if (!printable_token(token))
		return search_error(
			error, "LINE did not return an OpenChat search token.");
	return g_strdup(token);
}

static GBytes *search_transfer(Line *s, const char *url, GError **error)
{
	if (!printable_token(s->access) || !line_square_id(s->mid, 'u'))
		return search_error(error, "The LINE session is unavailable.");
	g_autoptr(J) profile_args = obj();

	json_object_object_add(profile_args, "1:i32", json_object_new_int(8));
	g_autoptr(GError) profile_error = NULL;
	g_autoptr(J) profile =
		line_rpc(s, "/S4", "getProfile", profile_args, &profile_error);
	const char *region = strfield(profile, "12");
	g_autofree char *info = search_info(s, region);
	g_autoptr(GError) channel_error = NULL;
	g_autoptr(LineSecret) channel = search_channel_token(s, &channel_error);

	if (!channel)
		g_clear_error(&channel_error);
	if (line_cooldown_check(url, error))
		return NULL;
	g_autoptr(LineSecret) token =
		channel ? g_strconcat("x-line-channeltoken: ", channel, NULL) :
			  NULL;
	g_autofree char *info_header = g_strconcat("x-line-info: ", info, NULL);
	const char *const values[] = {
		token,		 info_header,
		LINE_APP_HEADER, "x-lhm: GET",
		"x-lpv: 1",	 "accept: application/json; charset=utf-8",
		"Expect:"
	};
	struct curl_slist *headers = NULL;
	CURL *curl = curl_easy_init();
	CURLcode rc = CURLE_FAILED_INIT;
	long status = 0;
	struct search_response response = { .data = g_byte_array_new() };

	if (!curl)
		goto out;
	for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
		if (values[i] && !append_header(&headers, values[i])) {
			rc = CURLE_OUT_OF_MEMORY;
			goto out;
		}
#define SET(opt, value)                                  \
	do {                                             \
		rc = curl_easy_setopt(curl, opt, value); \
		if (rc)                                  \
			goto out;                        \
	} while (0)
	if (!line_curl_tune(curl, TRUE))
		goto out;
	SET(CURLOPT_URL, url);
	SET(CURLOPT_HTTPHEADER, headers);
	SET(CURLOPT_HTTPGET, 1L);
	SET(CURLOPT_FOLLOWLOCATION, 0L);
	SET(CURLOPT_PROTOCOLS_STR, "https");
	SET(CURLOPT_CONNECTTIMEOUT, 10L);
	SET(CURLOPT_TIMEOUT, 30L);
	SET(CURLOPT_NOSIGNAL, 1L);
	SET(CURLOPT_ACCEPT_ENCODING, "gzip");
	SET(CURLOPT_USERAGENT, LINE_USER_AGENT);
	SET(CURLOPT_WRITEFUNCTION, receive_search);
	SET(CURLOPT_WRITEDATA, &response);
#undef SET
	rc = curl_easy_perform(curl);
	if (!rc)
		rc = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (!rc)
		line_cooldown_note(url, curl, status);
out:
	curl_easy_cleanup(curl);
	line_curl_headers_free(headers);
	if (!rc && status >= 200 && status < 300)
		return g_byte_array_free_to_bytes(response.data);
	g_byte_array_unref(response.data);
	if (rc)
		g_set_error(error, LINE_ERROR,
			    rc == CURLE_OPERATION_TIMEDOUT ? LINE_TIMEOUT : 1,
			    "LINE OpenChat search failed: %s.",
			    curl_easy_strerror(rc));
	else
		g_set_error(
			error, LINE_ERROR,
			status == 429 || status == 503 ? LINE_RATE_LIMITED :
			status == 401 || status == 403 ? LINE_ACCESS_DENIED :
							 1,
			"LINE OpenChat search returned HTTP %ld.", status);
	return NULL;
}

static J *parse_search_response(J *raw, GError **error)
{
	J *main = field(field(raw, "result"), "main");
	J *rows = json_object_new_array();
	const char *continuation = NULL;

	for (size_t i = 0; i < search_length(main); i++) {
		J *body = field(json_object_array_get_idx(main, i), "body");

		if (g_strcmp0(strfield(body, "collectionType"), "openchat"))
			continue;
		J *items = field(body, "itemList");

		for (size_t j = 0; j < search_length(items); j++) {
			J *item = json_object_array_get_idx(items, j);
			const char *title = strfield(item, "title");
			const char *link = strfield(item, "link");

			if (!title || !*title || !link)
				continue;
			g_autofree char *ticket =
				line_square_parse_ticket(link);
			g_autofree char *emid =
				ticket ? NULL : line_square_parse_emid(link);

			if (!ticket && !emid)
				continue;
			J *row = obj();
			const char *desc = strfield(item, "desc");
			const char *count = strfield(item, "memberCountText");
			const char *doc_id = strfield(item, "docId");

			if (line_square_id(doc_id, 's'))
				put_str(row, "squareMid", doc_id);
			put_str(row, "name", title);
			put_str(row, "description", desc ? desc : "");
			put_str(row, "invitationUrl", link);
			if (count && *count) {
				char *end = NULL;
				gint64 number =
					g_ascii_strtoll(count, &end, 10);

				put_str(row, "memberCountText", count);
				if (end && end != count && !*end && number >= 0)
					put_num(row, "memberCount", number);
			}
			json_object_array_add(rows, row);
		}
		continuation = strfield(body, "moreLink");
	}
	J *out = obj();

	json_object_object_add(out, "squares", rows);
	if (continuation && *continuation) {
		g_autofree char *url = search_url("", continuation);

		if (!url) {
			json_object_put(out);
			return search_error(
				error,
				"LINE returned an invalid OpenChat search page.");
		}
		put_str(out, "cursor", continuation);
	}
	return out;
}

J *line_square_search_service(Line *s, const char *query,
			      const char *continuation, GError **error)
{
	g_autofree char *url = search_url(query, continuation);

	if (!url)
		return search_error(
			error,
			"LINE returned an invalid OpenChat search page.");
	g_autoptr(GBytes) bytes = search_transfer(s, url, error);

	if (!bytes)
		return NULL;
	gsize n = 0;
	const char *data = g_bytes_get_data(bytes, &n);
	struct json_tokener *parser = json_tokener_new_ex(32);

	if (!parser)
		return search_error(error,
				    "Could not parse OpenChat search results.");
	json_tokener_set_flags(parser, JSON_TOKENER_STRICT |
					       JSON_TOKENER_VALIDATE_UTF8);
	J *raw = json_tokener_parse_ex(parser, data, (int)n);
	gboolean ok = json_tokener_get_error(parser) == json_tokener_success &&
		      json_tokener_get_parse_end(parser) == n;

	json_tokener_free(parser);
	if (!ok || !json_object_is_type(raw, json_type_object)) {
		json_object_put(raw);
		return search_error(
			error,
			"LINE returned unreadable OpenChat search results.");
	}
	J *out = parse_search_response(raw, error);

	json_object_put(raw);
	return out;
}
