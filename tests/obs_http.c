// SPDX-License-Identifier: GPL-3.0-or-later
#include "obs_http.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <string.h>

static curl_write_callback write_body, write_header;
static void *body_data, *header_data;
static struct curl_slist *headers;
static const char *url, *token_reply = "3600\x1e"
				       "test-obs-token";
static const char *expected_url;
static gboolean upload, profile_upload, anonymous, legacy_preview,
	old_preview_path, large_preview;
static int token_requests, preview_requests, performs;
static long http_status = 200, timeout;

J *line_rpc(Line *s, const char *path, const char *method, J *args, GError **e)
{
	(void)s;
	(void)e;
	token_requests++;
	g_assert_cmpstr(path, ==, "/S4");
	g_assert_cmpstr(method, ==, "acquireEncryptedAccessToken");
	g_assert_cmpint(json_object_get_int(field(args, "2:i32")), ==, 2);
	return json_object_new_string(token_reply);
}

CURLcode __wrap_curl_easy_setopt(CURL *c, CURLoption opt, ...)
{
	(void)c;
	va_list v;

	va_start(v, opt);
	switch (opt) {
	case CURLOPT_URL:
		url = va_arg(v, const char *);
		break;
	case CURLOPT_HTTPHEADER:
		headers = va_arg(v, struct curl_slist *);
		break;
	case CURLOPT_WRITEFUNCTION:
		write_body = va_arg(v, curl_write_callback);
		break;
	case CURLOPT_WRITEDATA:
		body_data = va_arg(v, void *);
		break;
	case CURLOPT_HEADERFUNCTION:
		write_header = va_arg(v, curl_write_callback);
		break;
	case CURLOPT_HEADERDATA:
		header_data = va_arg(v, void *);
		break;
	case CURLOPT_FOLLOWLOCATION:
		g_assert_cmpint(va_arg(v, long), ==, 0);
		break;
	case CURLOPT_TIMEOUT:
		timeout = va_arg(v, long);
		break;
	default:
		break;
	}
	va_end(v);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_getinfo(CURL *c, CURLINFO info, ...)
{
	(void)c;
	va_list v;

	va_start(v, info);
	if (info == CURLINFO_RETRY_AFTER) {
		*va_arg(v, curl_off_t *) = 0; /* none: the 30 s default */
	} else {
		g_assert_cmpint(info, ==, CURLINFO_RESPONSE_CODE);
		*va_arg(v, long *) = http_status;
	}
	va_end(v);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_perform(CURL *c)
{
	(void)c;
	performs++;
	if (old_preview_path) {
		g_assert_cmpstr(
			url, ==,
			preview_requests ?
				"https://obs.line-apps.com/r/talk/emv/clip__ud-preview" :
				"https://obs.line-apps.com/r/talk/emv/clip/ud-preview");
		http_status = preview_requests++ ? 200 : 404;
	} else {
		g_assert_cmpstr(url, ==, expected_url);
	}
	gboolean auth = FALSE, meta = FALSE;

	for (struct curl_slist *it = headers; it; it = it->next) {
		if (!strcmp(it->data, "x-line-access: test-obs-token"))
			auth = TRUE;
		const char *p = strchr(it->data, ' ');

		if (!p)
			continue;
		if (g_str_has_prefix(it->data, upload ? "x-obs-params: " :
							"x-talk-meta: ")) {
			gsize len;

			g_autofree guint8 *blob = g_base64_decode(p + 1, &len);
			g_autofree char *str = g_strndup((char *)blob, len);

			g_autoptr(J) params = json_tokener_parse(str);
			if (profile_upload) {
				g_assert_cmpstr(strfield(params, "type"), ==,
						"image");
				g_assert_cmpstr(strfield(params, "ver"), ==,
						"2.0");
				g_assert_null(field(params, "range"));
			} else if (upload) {
				g_assert_cmpstr(strfield(params, "range"), ==,
						"bytes 0-2/3");
			} else {
				g_autofree guint8 *s = g_base64_decode(
					strfield(params, "message"), &len);
				const guint8 actual[] = { 11, 0,  4,   0,  0,
							  0,  1,  '9', 15, 0,
							  27, 12, 0,   0,  0,
							  0,  0 };
				g_assert_cmpmem(s, len, actual, sizeof(actual));
			}
			meta = TRUE;
		}
	}
	g_assert_cmpint(auth, ==, !anonymous);
	g_assert_cmpint(meta, ==, (!anonymous && !legacy_preview));
	static const char oid[] = "x-obs-oid: new-oid\r\n";
	static const char hash[] = "x-obs-hash: 0hSquareHash\r\n";

	if (upload) {
		write_header((char *)oid, 1, strlen(oid), header_data);
		write_header((char *)hash, 1, strlen(hash), header_data);
	}
	if (large_preview) {
		char chunk[65536] = { 0 };

		for (int i = 0; i < 32; i++)
			g_assert_cmpuint(write_body(chunk, 1, sizeof(chunk),
						    body_data),
					 ==, sizeof(chunk));
		g_assert_cmpuint(write_body(chunk, 1, sizeof(chunk), body_data),
				 ==, 0);
		return CURLE_WRITE_ERROR;
	}
	write_body("abc", 1, 3, body_data);
	return CURLE_OK;
}

static void images(void)
{
	g_autoptr(GError) e = NULL;
	g_autoptr(GBytes) bytes = g_bytes_new_static("abc", 3);
	Line s = { 0 };
	g_autofree char *token = line_obs_token(&s, &e);

	g_assert_cmpstr(token, ==, "test-obs-token");
	g_autofree char *reused = line_obs_token(&s, &e);

	g_assert_cmpstr(reused, ==, token);
	g_assert_cmpint(token_requests, ==, 1);
	g_free(s.obs_token);
	upload = TRUE;
	expected_url = "https://obs.line-apps.com/r/talk/emi/reqid-test";
	g_autofree char *oid = line_obs_upload(token, "emi", "reqid-test",
					       "name.jpg", bytes, &e);
	g_assert_cmpstr(oid, ==, "new-oid");
	upload = FALSE;
	expected_url = "https://obs.line-apps.com/r/talk/emi/new-oid";
	g_autoptr(GBytes) data = line_obs_download(token, "emi", oid, "9", &e);
	g_assert_true(g_bytes_equal(data, bytes));
	g_assert_cmpint(timeout, ==, 90);
	anonymous = TRUE;
	expected_url = "https://profile.line-scdn.net/0hEXAMPLE";
	g_autoptr(GBytes) avatar = line_avatar_download("/0hEXAMPLE", &e);
	g_assert_no_error(e);
	g_assert_nonnull(avatar);
	http_status = 302;
	g_assert_null(line_avatar_download("/0hEXAMPLE", &e));
	g_assert_nonnull(e);
}

static void profile_picture(void)
{
	g_autoptr(GError) e = NULL;
	g_autoptr(GBytes) jpeg = g_bytes_new_static("\xff\xd8\xff", 3);
	static const char mid[] = "u00000000000000000000000000000002";

	upload = profile_upload = TRUE;
	anonymous = legacy_preview = old_preview_path = large_preview = FALSE;
	http_status = 200;
	expected_url = "https://obs.line-apps.com/r/talk/p/"
		       "u00000000000000000000000000000002";
	g_assert_true(line_obs_upload_profile("test-obs-token", mid, jpeg, &e));
	g_assert_no_error(e);
	g_assert_cmpint(timeout, ==, 60);
	g_assert_false(
		line_obs_upload_profile("test-obs-token", "../x", jpeg, &e));
	g_clear_error(&e);
	upload = profile_upload = FALSE;
}

static void invalid(void)
{
	g_autoptr(GError) e = NULL;
	token_reply = "3600\x1e"
		      "invalid\rheader";
	g_assert_null(line_obs_token(NULL, &e));
	g_clear_error(&e);
	g_assert_null(line_avatar_download("/../elsewhere", &e));
	g_clear_error(&e);
	g_assert_null(line_obs_download("token", "emf", "../", "3", &e));
}

static void previews(void)
{
	const char *token = "test-obs-token";

	g_autoptr(GError) e = NULL;

	anonymous = upload = FALSE;
	legacy_preview = TRUE;
	http_status = 200;
	expected_url = "https://obs.line-apps.com/r/talk/m/9/preview";
	g_autoptr(GBytes) plain =
		line_obs_download_preview(token, "m", "9", "9", &e);
	g_assert_no_error(e);
	g_assert_cmpuint(g_bytes_get_size(plain), ==, 3);
	g_assert_cmpint(timeout, ==, 6);

	legacy_preview = FALSE;
	old_preview_path = TRUE;
	preview_requests = 0;
	g_autoptr(GBytes) sealed =
		line_obs_download_preview(token, "emv", "clip", "9", &e);
	g_assert_no_error(e);
	g_assert_cmpuint(g_bytes_get_size(sealed), ==, 3);
	g_assert_cmpint(preview_requests, ==, 2);
	old_preview_path = FALSE;

	g_assert_null(
		line_obs_download_preview(token, "emv", "clip/path", "9", &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_clear_error(&e);

	expected_url = "https://obs.line-apps.com/r/talk/emv/clip/ud-preview";
	http_status = 410;
	g_assert_null(line_obs_download_preview(token, "emv", "clip", "9", &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_nonnull(strstr(e->message, "expired"));
	g_clear_error(&e);

	http_status = 200;
	large_preview = TRUE;
	g_assert_null(line_obs_download_preview(token, "emv", "clip", "9", &e));
	g_assert_error(e, LINE_ERROR, 1);
	large_preview = FALSE;
}

/* A 429 from object storage stops its requests; other hosts carry on. */
static void cooldown(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, 0);
		g_test_trap_assert_passed();
		return;
	}
	g_autoptr(GError) e = NULL;

	expected_url = "https://obs.line-apps.com/r/talk/emi/oid";
	http_status = 429;
	performs = 0;
	g_assert_null(
		line_obs_download("test-obs-token", "emi", "oid", "9", &e));
	g_assert_error(e, LINE_ERROR, LINE_RATE_LIMITED);
	g_clear_error(&e);
	g_assert_null(
		line_obs_download("test-obs-token", "emi", "oid", "9", &e));
	g_assert_error(e, LINE_ERROR, LINE_RATE_LIMITED);
	g_assert_cmpint(performs, ==, 1);
	g_clear_error(&e);
	anonymous = TRUE;
	http_status = 200;
	expected_url = "https://profile.line-scdn.net/0hEXAMPLE";
	g_autoptr(GBytes) avatar = line_avatar_download("/0hEXAMPLE", &e);
	g_assert_no_error(e);
	g_assert_cmpint(performs, ==, 2);
}

static void square_obs(void)
{
	g_autoptr(GError) e = NULL;
	g_autoptr(GBytes) bytes = g_bytes_new_static("abc", 3);

	upload = profile_upload = TRUE;
	anonymous = legacy_preview = old_preview_path = large_preview = FALSE;
	http_status = 200;
	expected_url = "https://obs.line-apps.com/r/g2/m/1001";
	g_autofree char *oid = line_obs_upload_square(
		"test-obs-token", "1001", "photo.jpg", "image", bytes, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(oid, ==, "1001");

	profile_upload = TRUE;
	expected_url = "https://obs.line-apps.com/r/g2/member/"
		       "p00000000000000000000000000000001";
	g_autofree char *hash = line_obs_upload_square_profile(
		"test-obs-token", "member", "p00000000000000000000000000000001",
		bytes, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(hash, ==, "0hSquareHash");

	upload = profile_upload = FALSE;
	legacy_preview = TRUE;
	expected_url = "https://obs.line-apps.com/r/g2/m/1001";
	g_autoptr(GBytes) dl =
		line_obs_download_square("test-obs-token", "1001", &e);
	g_assert_no_error(e);
	g_assert_true(g_bytes_equal(dl, bytes));

	expected_url = "https://obs.line-apps.com/r/g2/m/1001/preview";
	g_autoptr(GBytes) pv =
		line_obs_download_square_preview("test-obs-token", "1001", &e);
	g_assert_no_error(e);
	g_assert_true(g_bytes_equal(pv, bytes));
	legacy_preview = FALSE;
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/obs/transfer", images);
	g_test_add_func("/obs/invalid", invalid);
	g_test_add_func("/obs/profile-picture-upload", profile_picture);
	g_test_add_func("/obs/video-preview-paths-and-limit", previews);
	g_test_add_func("/obs/square", square_obs);
	g_test_add_func("/obs/cooldown", cooldown);
	return g_test_run();
}
