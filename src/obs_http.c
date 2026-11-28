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
				line_secret_set(&s->obs_token, g_strdup(fresh));
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
	char *hash;
};

struct obs_video_request {
	const char *mid;
	char **redirect;
	gboolean metadata_url;
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

	if (n >= 5 && !g_ascii_strncasecmp(data, "HTTP/", 5)) {
		g_clear_pointer(&body->oid, g_free);
		g_clear_pointer(&body->hash, g_free);
	}
	if (n >= 10 && n <= 540 &&
	    !g_ascii_strncasecmp(data, "x-obs-oid:", 10)) {
		g_autofree char *value = g_strndup(data + 10, n - 10);

		g_strstrip(value);
		if (!segment(value) || body->oid)
			return 0;
		body->oid = g_steal_pointer(&value);
	}
	if (n >= 11 && n <= 540 &&
	    !g_ascii_strncasecmp(data, "x-obs-hash:", 11)) {
		g_autofree char *value = g_strndup(data + 11, n - 11);

		g_strstrip(value);
		if (!segment(value) || body->hash)
			return 0;
		body->hash = g_steal_pointer(&value);
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

static GBytes *transfer_full(const char *url, const char *token,
			     const char *metadata, const char *content_type,
			     GBytes *upload, char **oid, char **hash,
			     gsize limit, long timeout,
			     const struct obs_video_request *video,
			     GError **error)
{
	if (line_cooldown_check(url, error))
		return NULL;
	CURL *c = g_private_get(&http_client);

	if (!c) {
		c = curl_easy_init();
		if (c)
			g_private_set(&http_client, c);
	}
	g_autoptr(LineSecret) auth =
		token ? g_strconcat("x-line-access: ", token, NULL) : NULL;
	g_autoptr(LineSecret) mid =
		token && video && video->mid ?
			g_strconcat("x-line-mid: ", video->mid, NULL) :
			NULL;
	struct curl_slist *headers = NULL;
	const char *const list[] = {
		auth,
		metadata,
		mid,
		token ? LINE_APP_HEADER : NULL,
		upload ? (content_type ?
				  content_type :
				  "Content-Type: application/octet-stream") :
			 NULL,
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
	OPT(CURLOPT_USERAGENT, LINE_USER_AGENT);
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
	if (!rc) {
		line_cooldown_note(url, c, status);
		if (video && (status == 301 || status == 302 || status == 303 ||
			      status == 307 || status == 308)) {
			const char *location = NULL;

			rc = curl_easy_getinfo(c, CURLINFO_REDIRECT_URL,
					       &location);
			if (!rc && location)
				*video->redirect = g_strdup(location);
		}
	}
done:
	if (c)
		curl_easy_reset(c);
	line_curl_headers_free(headers);
	if (video && *video->redirect)
		goto failed;
	if (rc)
		g_set_error(error, LINE_ERROR, 1,
			    "LINE media request failed: %s.",
			    curl_easy_strerror(rc));
	else if (status == 410 && (token || (video && video->metadata_url)))
		g_set_error_literal(
			error, LINE_ERROR,
			video && video->metadata_url ? LINE_ASSET_NOT_FOUND : 1,
			"This attachment has expired on LINE.");
	else if (video && video->metadata_url && !token &&
		 (status == 401 || status == 403))
		g_set_error_literal(
			error, LINE_ERROR, LINE_ASSET_NOT_FOUND,
			"The public video link is no longer available.");
	else if (status < 200 || status > 299)
		g_set_error(error, LINE_ERROR,
			    status == 404	   ? LINE_ASSET_NOT_FOUND :
			    token && status == 401 ? LINE_OBS_UNAUTHORIZED :
			    status == 429 || status == 503 ? LINE_RATE_LIMITED :
							     1,
			    "LINE media service returned HTTP %ld.", status);
	else if (video && (status != 200 || !body.bytes->len))
		g_set_error_literal(
			error, LINE_ERROR, 1,
			status == 202 ?
				"LINE is still preparing this video." :
				"LINE returned no complete video data.");
	else if (oid && !body.oid)
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE did not acknowledge the attachment upload.");
	else {
		if (oid)
			*oid = g_steal_pointer(&body.oid);
		if (hash)
			*hash = g_steal_pointer(&body.hash);
		g_free(body.oid);
		g_free(body.hash);
		return g_byte_array_free_to_bytes(body.bytes);
	}
failed:
	g_free(body.oid);
	g_free(body.hash);
	g_byte_array_unref(body.bytes);
	return NULL;
}

static GBytes *transfer(const char *url, const char *token,
			const char *metadata, GBytes *upload, char **oid,
			gsize limit, long timeout, GError **error)
{
	return transfer_full(url, token, metadata, NULL, upload, oid, NULL,
			     limit, timeout, NULL, error);
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

gboolean line_obs_upload_group_profile(const char *token, const char *chat_mid,
				       GBytes *jpeg, GError **e)
{
	if (!token || !chat_mid || *chat_mid != 'c' || !segment(chat_mid) ||
	    !jpeg || !g_bytes_get_size(jpeg)) {
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
	g_autofree char *url = g_strconcat(
		"https://obs.line-apps.com/r/talk/g/", chat_mid, NULL);
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

GBytes *line_obs_download_original(const char *token, const char *sid,
				   const char *oid, const char *id, GError **e)
{
	if (!token || !segment(sid) || !segment(oid) || !digits(id)) {
		invalid(e);
		return NULL;
	}
	g_autofree char *hdr = download_header(id);
	g_autofree char *url = g_strdup_printf(
		"https://obs.line-apps.com/r/talk/%s/%s/original", sid, oid);
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

static gboolean obs_host(const char *host, const char *domain)
{
	const char *dot = strchr(host, '.');

	if (!dot || g_ascii_strcasecmp(dot + 1, domain))
		return FALSE;
	if (dot - host == 3 && !g_ascii_strncasecmp(host, "obs", 3))
		return TRUE;
	if (dot - host < 5 || dot - host > 63 ||
	    g_ascii_strncasecmp(host, "obs-", 4) || !g_ascii_isalnum(host[4]) ||
	    !g_ascii_isalnum(dot[-1]))
		return FALSE;
	for (const char *p = host + 5; p < dot; p++)
		if (!g_ascii_isalnum(*p) && *p != '-')
			return FALSE;
	return TRUE;
}

static GUri *video_endpoint(const char *url, gboolean *private)
{
	if (!url || strnlen(url, 4097) > 4096 ||
	    g_ascii_strncasecmp(url, "https://", 8))
		return NULL;
	const char *authority = url + 8;
	const char *end = authority + strcspn(authority, "/?#");

	for (const char *p = authority; p < end; p++)
		if (!g_ascii_isalnum(*p) && !strchr(".-:", *p))
			return NULL;
	for (const char *p = end; *p; p++)
		if (!g_ascii_isgraph(*p) || *p == '\\' || *p == '#')
			return NULL;
	g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);

	if (!uri || g_uri_get_userinfo(uri) ||
	    (g_uri_get_port(uri) != -1 && g_uri_get_port(uri) != 443) ||
	    g_uri_get_path(uri)[0] != '/')
		return NULL;
	const char *host = g_uri_get_host(uri);

	if (!host)
		return NULL;
	*private = obs_host(host, "line-apps.com");
	if (!*private && !obs_host(host, "line-scdn.net") &&
	    g_ascii_strcasecmp(host, "vos.line-scdn.net"))
		return NULL;
	return g_steal_pointer(&uri);
}

static gboolean video_scope(const char *path, const char *service,
			    const char *sid, const char *oid)
{
	g_autofree char *prefix = g_strdup_printf("/r/%s/%s/", service, sid);

	if (!g_str_has_prefix(path, prefix))
		return FALSE;
	const char *object = path + strlen(prefix);
	gsize size = strcspn(object, "/");

	if (size > 512)
		return FALSE;
	g_autofree char *name = g_strndup(object, size);
	const char *tail = object + size;

	if (!*tail && oid && strcmp(name, oid) &&
	    g_str_has_suffix(name, "__ud-preview"))
		name[size - strlen("__ud-preview")] = 0;
	if (*tail && strcmp(tail, "/original") && strcmp(tail, "/preview") &&
	    strcmp(tail, "/ud-preview"))
		return FALSE;
	return segment(name) && (!oid || !strcmp(name, oid));
}

static GBytes *video_get(const char *url, const char *token, const char *mid,
			 const char *header, gsize limit, gint64 deadline,
			 gboolean metadata_url, GError **error)
{
	g_autofree char *current = g_strdup(url);

	for (guint hop = 0; hop < 4; hop++) {
		gboolean private;

		g_autoptr(GUri) uri = video_endpoint(current, &private);

		if (!uri)
			break;
		const char *path = g_uri_get_path(uri);

		/* A public hop cannot send account headers on a later hop. */
		if (!private || (!g_str_has_prefix(path, "/r/talk/") &&
				 !g_str_has_prefix(path, "/r/g2/")))
			token = NULL;
		gint64 left = deadline - g_get_monotonic_time();

		if (left <= 0) {
			g_set_error_literal(error, LINE_ERROR, LINE_TIMEOUT,
					    "LINE video download timed out.");
			return NULL;
		}
		g_autofree char *redirect = NULL;
		const struct obs_video_request request = {
			.mid = mid,
			.redirect = &redirect,
			.metadata_url = metadata_url,
		};
		GBytes *bytes = transfer_full(
			current, token, token ? header : NULL, NULL, NULL, NULL,
			NULL, limit,
			(left + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, &request,
			error);

		if (!redirect)
			return bytes;
		g_free(current);
		current = g_steal_pointer(&redirect);
	}
	g_set_error_literal(error, LINE_ERROR, 1,
			    "LINE returned an unsupported video redirect.");
	return NULL;
}

static gboolean visible(const char *value, gsize limit)
{
	if (!value || !*value || strnlen(value, limit + 1) > limit)
		return FALSE;
	for (const char *p = value; *p; p++)
		if (!g_ascii_isgraph(*p))
			return FALSE;
	return TRUE;
}

static gboolean video_mid(const char *mid)
{
	if (!mid)
		return TRUE;
	if (strlen(mid) != 33 || mid[0] != 'u')
		return FALSE;
	return strspn(mid + 1, "0123456789abcdefABCDEF") == 32;
}

GBytes *line_obs_download_video(const char *token, const char *mid,
				const char *sid, const char *oid,
				const char *id, const char *pop,
				const char *url, gboolean square,
				gboolean preview, GError **error)
{
	if (!digits(id) || !video_mid(mid) ||
	    (token && !visible(token, 16384)) || (oid && !segment(oid)) ||
	    (g_strcmp0(sid, "m") && (square || g_strcmp0(sid, "emv"))) ||
	    (pop && !visible(pop, 1024))) {
		invalid(error);
		return NULL;
	}
	const char *service = square ? "g2" : "talk";
	gboolean sealed = !strcmp(sid, "emv");
	const char *fallback_oid = oid ? oid : (sealed ? NULL : id);
	g_autofree char *hdr =
		!square && (!preview || sealed) ? download_header(id) : NULL;
	g_autofree char *escaped = pop ? g_uri_escape_string(pop, NULL, FALSE) :
					 NULL;
	g_autofree char *query = escaped ? g_strconcat("?p=", escaped, NULL) :
					   g_strdup("");
	gint64 deadline =
		g_get_monotonic_time() + (preview ? 6 : 90) * G_USEC_PER_SEC;
	gsize limit = preview ? LINE_MEDIA_PREVIEW_LIMIT + (sealed ? 32 : 0) :
				LINE_MEDIA_LIMIT + 65536;
	const char *suffix = preview ? (sealed ? "/ud-preview" : "/preview") :
				       "";
	g_autofree char *fallback =
		fallback_oid && token ?
			g_strdup_printf(
				"https://obs.line-apps.com/r/%s/%s/%s%s%s",
				service, sid, fallback_oid, suffix, query) :
			NULL;
	g_autofree char *target = NULL;

	g_autoptr(GError) failure = NULL;

	GBytes *bytes = NULL;

	if (url && url[0] == '/' && url[1] != '/')
		target = g_strconcat("https://obs.line-apps.com", url, NULL);
	else
		target = g_strdup(url);
	gboolean private;

	g_autoptr(GUri) parsed = video_endpoint(target, &private);

	if (parsed) {
		gboolean own = private && video_scope(g_uri_get_path(parsed),
						      service, sid, oid);

		if (own && !g_uri_get_query(parsed) && escaped) {
			char *with_query = g_strconcat(target, query, NULL);

			g_free(target);
			target = with_query;
		}
		bytes = video_get(target, own ? token : NULL, mid, hdr, limit,
				  deadline, TRUE, &failure);
		if (bytes ||
		    !g_error_matches(failure, LINE_ERROR, LINE_ASSET_NOT_FOUND))
			goto done;
	}
	if (fallback && (!parsed || g_strcmp0(target, fallback))) {
		g_clear_error(&failure);
		bytes = video_get(fallback, token, mid, hdr, limit, deadline,
				  FALSE, &failure);
	}
	if (!bytes && fallback && sealed && preview &&
	    g_error_matches(failure, LINE_ERROR, LINE_ASSET_NOT_FOUND)) {
		g_autofree char *legacy = g_strdup_printf(
			"https://obs.line-apps.com/r/talk/emv/%s__ud-preview%s",
			oid, query);

		g_clear_error(&failure);
		bytes = video_get(legacy, token, mid, hdr, limit, deadline,
				  FALSE, &failure);
	}
done:
	if (!bytes && failure)
		g_propagate_error(error, g_steal_pointer(&failure));
	else if (!bytes)
		invalid(error);
	return bytes;
}

static const char *square_content_type(const char *name, const char *media_type)
{
	const char *dot = name ? strrchr(name, '.') : NULL;

	if (dot && dot[1]) {
		if (!g_ascii_strcasecmp(dot + 1, "jpg") ||
		    !g_ascii_strcasecmp(dot + 1, "jpeg"))
			return "Content-Type: image/jpeg";
		if (!g_ascii_strcasecmp(dot + 1, "png"))
			return "Content-Type: image/png";
		if (!g_ascii_strcasecmp(dot + 1, "gif"))
			return "Content-Type: image/gif";
		if (!g_ascii_strcasecmp(dot + 1, "webp"))
			return "Content-Type: image/webp";
		if (!g_ascii_strcasecmp(dot + 1, "mp4"))
			return "Content-Type: video/mp4";
		if (!g_ascii_strcasecmp(dot + 1, "m4a"))
			return "Content-Type: audio/mp4";
		if (!g_ascii_strcasecmp(dot + 1, "mp3"))
			return "Content-Type: audio/mpeg";
	}
	if (!strcmp(media_type, "IMAGE"))
		return "Content-Type: image/jpeg";
	if (!strcmp(media_type, "VIDEO"))
		return "Content-Type: video/mp4";
	if (!strcmp(media_type, "AUDIO"))
		return "Content-Type: audio/mp3";
	return "Content-Type: application/octet-stream";
}

/*
 * OpenChat (Square) attachments use LINE's V1 OBS flow (h51.z / w44.a in LINE
 * Android 26.19.0): POST directly to /r/g2/m/reqseq with tomid, oid="reqseq",
 * and reqseq. OBS delivers the message to the OpenChat and returns the created
 * message ID in the x-obs-oid response header.
 */
char *line_obs_upload_square(const char *token, const char *chat, gint32 seq,
			     const char *name, const char *media_type,
			     guint64 duration_ms, GBytes *plain, GError **e)
{
	if (!token || !chat || *chat != 'm' || !segment(chat) || seq <= 0 ||
	    !name || !*name || !media_type || !plain ||
	    !g_bytes_get_size(plain) ||
	    (strcmp(media_type, "IMAGE") && strcmp(media_type, "VIDEO") &&
	     strcmp(media_type, "AUDIO") && strcmp(media_type, "FILE"))) {
		invalid(e);
		return NULL;
	}
	gsize n = g_bytes_get_size(plain);
	g_autofree char *range = g_strdup_printf("bytes 0-%zu/%zu", n - 1, n);
	g_autofree char *seq_str = g_strdup_printf("%" G_GINT32_FORMAT, seq);

	g_autoptr(J) params = obj();

	json_object_object_add(params, "ver", json_object_new_string("1.0"));
	json_object_object_add(params, "name", json_object_new_string(name));
	json_object_object_add(params, "type",
			       json_object_new_string(media_type));
	json_object_object_add(params, "tomid", json_object_new_string(chat));
	json_object_object_add(params, "oid", json_object_new_string("reqseq"));
	json_object_object_add(params, "reqseq",
			       json_object_new_string(seq_str));
	json_object_object_add(params, "range", json_object_new_string(range));
	const char *ctype = square_content_type(name, media_type);

	if (!strcmp(media_type, "IMAGE")) {
		json_object_object_add(params, "quality",
				       json_object_new_string("100"));
		if (!strcmp(ctype, "Content-Type: image/gif"))
			json_object_object_add(
				params, "cat",
				json_object_new_string("original"));
	}
	if (duration_ms &&
	    (!strcmp(media_type, "VIDEO") || !strcmp(media_type, "AUDIO"))) {
		g_autofree char *dur =
			g_strdup_printf("%" G_GUINT64_FORMAT, duration_ms);

		json_object_object_add(params, "duration",
				       json_object_new_string(dur));
	}
	const char *json =
		json_object_to_json_string_ext(params, JSON_C_TO_STRING_PLAIN);
	g_autofree char *b64 =
		g_base64_encode((const guint8 *)json, strlen(json));
	g_autofree char *hdr = g_strconcat("x-obs-params: ", b64, NULL);
	char *oid = NULL;

	g_autoptr(GBytes) reply = transfer_full(
		"https://obs.line-apps.com/r/g2/m/reqseq", token, hdr, ctype,
		plain, &oid, NULL, 65536, 90L, NULL, e);

	if (!reply)
		return NULL;
	return oid;
}

GBytes *line_obs_download_square(const char *token, const char *message_id,
				 GError **e)
{
	if (!token || !digits(message_id)) {
		invalid(e);
		return NULL;
	}
	g_autofree char *url = g_strconcat("https://obs.line-apps.com/r/g2/m/",
					   message_id, NULL);

	return transfer(url, token, NULL, NULL, NULL, LINE_MEDIA_LIMIT + 65536,
			90L, e);
}

GBytes *line_obs_download_square_original(const char *token,
					  const char *message_id, GError **e)
{
	if (!token || !digits(message_id)) {
		invalid(e);
		return NULL;
	}
	g_autofree char *url = g_strconcat("https://obs.line-apps.com/r/g2/m/",
					   message_id, "/original", NULL);

	return transfer(url, token, NULL, NULL, NULL, LINE_MEDIA_LIMIT + 65536,
			90L, e);
}

GBytes *line_obs_download_square_preview(const char *token,
					 const char *message_id, GError **e)
{
	if (!token || !digits(message_id)) {
		invalid(e);
		return NULL;
	}
	g_autofree char *url = g_strconcat("https://obs.line-apps.com/r/g2/m/",
					   message_id, "/preview", NULL);

	return transfer(url, token, NULL, NULL, NULL, LINE_MEDIA_PREVIEW_LIMIT,
			6L, e);
}

char *line_obs_upload_square_profile(const char *token, const char *scope,
				     const char *oid, GBytes *jpeg, GError **e)
{
	if (!token || !scope ||
	    (strcmp(scope, "member") && strcmp(scope, "group")) ||
	    !segment(oid) || !jpeg || !g_bytes_get_size(jpeg)) {
		invalid(e);
		return NULL;
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
	g_autofree char *url = g_strdup_printf(
		"https://obs.line-apps.com/r/g2/%s/%s", scope, oid);
	char *hash = NULL;

	g_autoptr(GBytes) reply = transfer_full(
		url, token, hdr, NULL, jpeg, NULL, &hash, 65536, 60L, NULL, e);

	if (!reply) {
		g_free(hash);
		return NULL;
	}
	return hash ? hash : g_strdup(oid);
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
