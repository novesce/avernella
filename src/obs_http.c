// SPDX-License-Identifier: GPL-3.0-or-later
#include "obs_http.h"
#include "media.h"
#include <curl/curl.h>
#include <string.h>

static gboolean segment(const char *s)
{
	if (!s || !*s || strlen(s) > 512)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && *s != '-' && *s != '_')
			return FALSE;
	return TRUE;
}

static gboolean digits(const char *s)
{
	if (!s || !*s || strlen(s) > 24)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isdigit(*s))
			return FALSE;
	return TRUE;
}

static void invalid(GError **e)
{
	g_set_error_literal(e, LINE_ERROR, 1, "Invalid LINE media reference.");
}

char *line_obs_token(Line *s, GError **error)
{
	gint64 now = g_get_monotonic_time();

	if (s && s->obs_token && now < s->obs_until)
		return g_strdup(s->obs_token);
	g_autoptr(J) args = obj();
	json_object_object_add(args, "2:i32", json_object_new_int(2));
	g_autoptr(J) result =
		line_rpc(s, "/S4", "acquireEncryptedAccessToken", args, error);
	if (!result)
		return NULL;
	if (json_object_is_type(result, json_type_string)) {
		const char *raw = json_object_get_string(result);
		const char *end = raw + json_object_get_string_len(result);
		const char *sep = memchr(raw, 0x1e, end - raw);

		if (sep && sep > raw && sep - raw <= 12) {
			for (const char *p = raw; p < sep; p++)
				if (!g_ascii_isdigit(*p))
					goto bad;
			g_autofree char *seconds = g_strndup(raw, sep - raw);
			gint64 ttl = g_ascii_strtoll(seconds, NULL, 10);
			const char *token = sep + 1;

			sep = memchr(token, 0x1e, end - token);
			if (sep)
				end = sep;
			if (end <= token || end - token > 16384)
				goto bad;
			for (const char *p = token; p < end; p++)
				if (*p < 33 || *p > 126)
					goto bad;
			char *fresh = g_strndup(token, end - token);

			if (s) {
				g_free(s->obs_token);
				s->obs_token = g_strdup(fresh);
				s->obs_until = now + CLAMP(ttl - CLAMP(ttl / 20,
								       30, 600),
							   0, 86400) *
							     G_USEC_PER_SEC;
			}
			return fresh;
		}
	}
bad:
	g_set_error_literal(
		error, LINE_ERROR, 1,
		"LINE returned an invalid attachment access token.");
	return NULL;
}

struct obs_response_body {
	GByteArray *bytes;
	gsize limit;
	char *oid;
};

static size_t receive(char *data, size_t size, size_t count, void *user)
{
	struct obs_response_body *body = user;

	if (size && count > (body->limit - body->bytes->len) / size)
		return 0;
	g_byte_array_append(body->bytes, (const guint8 *)data, size * count);
	return size * count;
}

static size_t header(char *data, size_t size, size_t count, void *user)
{
	struct obs_response_body *body = user;
	size_t n = size * count;

	if (n >= 5 && !g_ascii_strncasecmp(data, "HTTP/", 5))
		g_clear_pointer(&body->oid, g_free);
	if (n >= 10 && n <= 540 &&
	    !g_ascii_strncasecmp(data, "x-obs-oid:", 10)) {
		g_autofree char *value = g_strndup(data + 10, n - 10);

		g_strstrip(value);
		if (!segment(value) || body->oid)
			return 0;
		body->oid = g_steal_pointer(&value);
	}
	return n;
}

static void free_http_client(gpointer easy)
{
	curl_easy_cleanup(easy);
}

static GPrivate http_client = G_PRIVATE_INIT(free_http_client);

void line_obs_http_close(void)
{
	g_private_replace(&http_client, NULL);
}

static GBytes *transfer(const char *url, const char *token,
			const char *metadata, GBytes *upload, char **oid,
			gsize limit, long timeout, GError **error)
{
	CURL *c = g_private_get(&http_client);

	if (!c) {
		c = curl_easy_init();
		if (c)
			g_private_set(&http_client, c);
	}
	struct curl_slist *headers = NULL;
	g_autofree char *auth =
		token ? g_strconcat("x-line-access: ", token, NULL) : NULL;
	const char *const list[] = {
		auth, metadata,
		token ? "x-line-application: DESKTOPWIN\t9.7.0.3556\tWINDOWS\t10.0.0-NT-x64" :
			NULL,
		upload ? "Content-Type: application/octet-stream" : NULL,
		upload ? "Expect:" : NULL
	};
	struct obs_response_body body = { .bytes = g_byte_array_new(),
					  .limit = limit };
	CURLcode rc = CURLE_FAILED_INIT;
	long status = 0;

	if (!c)
		goto done;
	for (gsize i = 0; i < G_N_ELEMENTS(list); i++)
		if (list[i]) {
			struct curl_slist *next =
				curl_slist_append(headers, list[i]);
			if (!next) {
				rc = CURLE_OUT_OF_MEMORY;
				goto done;
			}
			headers = next;
		}
#define OPT(opt, val)                               \
	do {                                        \
		rc = curl_easy_setopt(c, opt, val); \
		if (rc)                             \
			goto done;                  \
	} while (0)
	if (!line_curl_tune(c, TRUE))
		goto done;
	OPT(CURLOPT_URL, url);
	OPT(CURLOPT_PROTOCOLS_STR, "https");
	OPT(CURLOPT_FOLLOWLOCATION, 0L);
	OPT(CURLOPT_NOSIGNAL, 1L);
	OPT(CURLOPT_CONNECTTIMEOUT, MIN(8L, timeout));
	OPT(CURLOPT_TIMEOUT, timeout);
	OPT(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
	OPT(CURLOPT_HTTPHEADER, headers);
	OPT(CURLOPT_USERAGENT, "Line/9.7.0.3556");
	OPT(CURLOPT_WRITEFUNCTION, receive);
	OPT(CURLOPT_WRITEDATA, &body);
	OPT(CURLOPT_HEADERFUNCTION, header);
	OPT(CURLOPT_HEADERDATA, &body);
	if (upload) {
		gsize n;
		const void *ptr = g_bytes_get_data(upload, &n);

		OPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)n);
		OPT(CURLOPT_POSTFIELDS, ptr);
	}
#undef OPT
	rc = curl_easy_perform(c);
	if (!rc)
		rc = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
done:
	if (c)
		curl_easy_reset(c);
	curl_slist_free_all(headers);
	if (rc)
		g_set_error(error, LINE_ERROR, 1,
			    "LINE media request failed: %s.",
			    curl_easy_strerror(rc));
	else if (token && status == 410)
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This attachment has expired on LINE.");
	else if (status < 200 || status > 299)
		g_set_error(error, LINE_ERROR,
			    status == 404	   ? LINE_ASSET_NOT_FOUND :
			    token && status == 401 ? LINE_OBS_UNAUTHORIZED :
						     1,
			    "LINE media service returned HTTP %ld.", status);
	else if (oid && !body.oid)
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE did not acknowledge the attachment upload.");
	else {
		if (oid)
			*oid = g_steal_pointer(&body.oid);
		g_free(body.oid);
		return g_byte_array_free_to_bytes(body.bytes);
	}
	g_free(body.oid);
	g_byte_array_unref(body.bytes);
	return NULL;
}

char *line_obs_upload(const char *token, const char *sid, const char *oid,
		      const char *name, GBytes *encrypted, GError **e)
{
	if (!token || !segment(sid) || !segment(oid) || !name || !encrypted ||
	    !g_bytes_get_size(encrypted)) {
		invalid(e);
		return NULL;
	}
	gsize n = g_bytes_get_size(encrypted);
	g_autofree char *range = g_strdup_printf("bytes 0-%zu/%zu", n - 1, n);

	g_autoptr(J) params = obj();
	json_object_object_add(params, "type", json_object_new_string("FILE"));
	json_object_object_add(params, "ver", json_object_new_string("2.0"));
	json_object_object_add(params, "name", json_object_new_string(name));
	json_object_object_add(params, "range", json_object_new_string(range));
	const char *json =
		json_object_to_json_string_ext(params, JSON_C_TO_STRING_PLAIN);
	g_autofree char *b64 =
		g_base64_encode((const guint8 *)json, strlen(json));
	g_autofree char *hdr = g_strconcat("x-obs-params: ", b64, NULL);
	g_autofree char *url = g_strdup_printf(
		"https://obs.line-apps.com/r/talk/%s/%s", sid, oid);
	char *result = NULL;

	g_autoptr(GBytes) data =
		transfer(url, token, hdr, encrypted, &result, 65536, 90L, e);
	return result;
}

/*
 * Profile pictures live at talk/p/<mid>. LINE's clients post the JPEG
 * with "type":"image" and full quality; OBS then updates the profile and
 * answers without an object ID.
 */
gboolean line_obs_upload_profile(const char *token, const char *mid,
				 GBytes *jpeg, GError **e)
{
	if (!token || !mid || *mid != 'u' || !segment(mid) || !jpeg ||
	    !g_bytes_get_size(jpeg)) {
		invalid(e);
		return FALSE;
	}
	g_autoptr(J) params = obj();
	json_object_object_add(params, "name",
			       json_object_new_string("profile.jpg"));
	json_object_object_add(params, "quality",
			       json_object_new_string("100"));
	json_object_object_add(params, "type", json_object_new_string("image"));
	json_object_object_add(params, "ver", json_object_new_string("2.0"));
	const char *json =
		json_object_to_json_string_ext(params, JSON_C_TO_STRING_PLAIN);
	g_autofree char *b64 =
		g_base64_encode((const guint8 *)json, strlen(json));
	g_autofree char *hdr = g_strconcat("x-obs-params: ", b64, NULL);
	g_autofree char *url =
		g_strconcat("https://obs.line-apps.com/r/talk/p/", mid, NULL);
	g_autoptr(GBytes) reply =
		transfer(url, token, hdr, jpeg, NULL, 65536, 60L, e);
	return reply != NULL;
}

static char *download_header(const char *id)
{
	guint8 thrift[48] = { 11, 0, 4, 0, 0, 0, 0 };
	gsize n = strlen(id);

	thrift[6] = n;
	memcpy(thrift + 7, id, n);
	static const guint8 tail[] = { 15, 0, 27, 12, 0, 0, 0, 0, 0 };

	memcpy(thrift + 7 + n, tail, sizeof(tail));
	g_autofree char *message =
		g_base64_encode(thrift, 7 + n + sizeof(tail));
	g_autofree char *json =
		g_strdup_printf("{\"message\":\"%s\"}", message);
	g_autofree char *b64 =
		g_base64_encode((const guint8 *)json, strlen(json));
	return g_strconcat("x-talk-meta: ", b64, NULL);
}

GBytes *line_obs_download(const char *token, const char *sid, const char *oid,
			  const char *id, GError **e)
{
	if (!token || !segment(sid) || !segment(oid) || !digits(id)) {
		invalid(e);
		return NULL;
	}
	g_autofree char *hdr = download_header(id);
	g_autofree char *url = g_strdup_printf(
		"https://obs.line-apps.com/r/talk/%s/%s", sid, oid);
	return transfer(url, token, hdr, NULL, NULL, LINE_MEDIA_LIMIT + 65536,
			90L, e);
}

GBytes *line_obs_download_preview(const char *token, const char *sid,
				  const char *oid, const char *id, GError **e)
{
	if (!token || !segment(oid) || !digits(id) ||
	    (g_strcmp0(sid, "m") && g_strcmp0(sid, "emv")) ||
	    (!g_strcmp0(sid, "m") && g_strcmp0(oid, id))) {
		invalid(e);
		return NULL;
	}
	gboolean sealed = !strcmp(sid, "emv");
	const gsize limit = LINE_MEDIA_PREVIEW_LIMIT + (sealed ? 32 : 0);
	g_autofree char *hdr = sealed ? download_header(id) : NULL;
	g_autofree char *base = g_strdup_printf(
		"https://obs.line-apps.com/r/talk/%s/%s", sid, oid);
	g_autofree char *url =
		g_strconcat(base, sealed ? "/ud-preview" : "/preview", NULL);
	g_autoptr(GError) error = NULL;
	GBytes *data = transfer(url, token, hdr, NULL, NULL, limit, 6L, &error);

	if (!data && sealed &&
	    g_error_matches(error, LINE_ERROR, LINE_ASSET_NOT_FOUND)) {
		g_clear_error(&error);
		g_free(url);
		url = g_strconcat(base, "__ud-preview", NULL);
		data = transfer(url, token, hdr, NULL, NULL, limit, 6L, &error);
	}
	if (!data && error)
		g_propagate_error(e, g_steal_pointer(&error));
	return data;
}

GBytes *line_avatar_download(const char *path, GError **e)
{
	if (!path || path[0] != '/' || strlen(path) > 1024 ||
	    strstr(path, "..")) {
		invalid(e);
		return NULL;
	}
	for (const char *p = path + 1; *p; p++)
		if (!g_ascii_isalnum(*p) && !strchr("/_-.", *p)) {
			invalid(e);
			return NULL;
		}
	g_autofree char *url =
		g_strconcat("https://profile.line-scdn.net", path, NULL);
	return transfer(url, NULL, NULL, NULL, NULL, 2 * 1024 * 1024, 10L, e);
}

GBytes *line_public_asset(const char *url, gsize limit, GError **error)
{
	static const char origin[] = "https://stickershop.line-scdn.net/";

	if (!url || !g_str_has_prefix(url, origin) || !limit ||
	    limit > 16 * 1024 * 1024) {
		invalid(error);
		return NULL;
	}
	return transfer(url, NULL, NULL, NULL, NULL, limit, 10L, error);
}
