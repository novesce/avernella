// SPDX-License-Identifier: GPL-3.0-or-later
#include "flex_asset.h"
#include <fcntl.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static curl_opensocket_callback open_socket;
static curl_write_callback write_body;
static void *body_data;
static const char *asset_url;
static gsize hits;
static long response_status;
static gboolean oversized, webp, sockets_checked;
/* Per-request statuses for multi-step transfers, and what was asked. */
static const long *script;
static guint script_len, script_hit;
static const char *redirect_to;
static GPtrArray *requested;

CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption name, ...)
{
	(void)curl;
	va_list args;

	va_start(args, name);
	switch (name) {
	case CURLOPT_URL:
		asset_url = va_arg(args, const char *);
		break;
	case CURLOPT_OPENSOCKETFUNCTION:
		open_socket = va_arg(args, curl_opensocket_callback);
		break;
	case CURLOPT_WRITEFUNCTION:
		write_body = va_arg(args, curl_write_callback);
		break;
	case CURLOPT_WRITEDATA:
		body_data = va_arg(args, void *);
		break;
	case CURLOPT_PROTOCOLS_STR:
		g_assert_cmpstr(va_arg(args, const char *), ==, "https");
		break;
	case CURLOPT_PROXY:
		g_assert_cmpstr(va_arg(args, const char *), ==, "");
		break;
	case CURLOPT_FOLLOWLOCATION:
		g_assert_cmpint(va_arg(args, long), ==, 0);
		break;
	case CURLOPT_MAXFILESIZE_LARGE:
		g_assert_cmpint(va_arg(args, curl_off_t), ==,
				LINE_FLEX_ASSET_LIMIT);
		break;
	case CURLOPT_HTTPHEADER:
	case CURLOPT_USERPWD:
	case CURLOPT_COOKIE:
	case CURLOPT_COOKIEFILE:
		g_assert_not_reached();
	default:
		break;
	}
	va_end(args);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO name, ...)
{
	(void)curl;
	va_list args;

	va_start(args, name);
	switch (name) {
	case CURLINFO_RESPONSE_CODE:
		*va_arg(args, long *) = script && script_hit < script_len ?
						script[script_hit] :
						response_status;
		break;
	case CURLINFO_REDIRECT_URL:
		*va_arg(args, const char **) = redirect_to;
		break;
	case CURLINFO_CONTENT_TYPE:
		*va_arg(args, const char **) = "image/png";
		break;
	case CURLINFO_RETRY_AFTER:
		*va_arg(args, curl_off_t *) = 120;
		break;
	default:
		g_assert_not_reached();
	}
	va_end(args);
	return CURLE_OK;
}

static int try_typed(int family, const char *ip, guint16 port, int type)
{
	union {
		struct curl_sockaddr info;
		guint8 space[offsetof(struct curl_sockaddr, addr) +
			     sizeof(struct sockaddr_storage)];
	} address = { .info = { .family = family,
				.socktype = type,
				.protocol = IPPROTO_TCP } };

	if (family == AF_INET) {
		struct sockaddr_in addr = { .sin_family = AF_INET,
					    .sin_port = htons(port) };

		g_assert_cmpint(inet_pton(family, ip, &addr.sin_addr), ==, 1);
		address.info.addrlen = sizeof(addr);
		memcpy(address.space + offsetof(struct curl_sockaddr, addr),
		       &addr, sizeof(addr));
	} else {
		struct sockaddr_in6 addr = { .sin6_family = AF_INET6,
					     .sin6_port = htons(port) };

		g_assert_cmpint(inet_pton(family, ip, &addr.sin6_addr), ==, 1);
		address.info.addrlen = sizeof(addr);
		memcpy(address.space + offsetof(struct curl_sockaddr, addr),
		       &addr, sizeof(addr));
	}
	return open_socket(NULL, CURLSOCKTYPE_IPCXN, &address.info);
}

static int try_address(int family, const char *ip, guint16 port)
{
	return try_typed(family, ip, port, SOCK_STREAM);
}

CURLcode __wrap_curl_easy_perform(CURL *curl)
{
	(void)curl;
	hits++;
	if (script && hits > 1)
		script_hit++;
	if (requested)
		g_ptr_array_add(requested, g_strdup(asset_url));
	g_assert_true(g_str_has_prefix(asset_url, "https://"));
	g_assert_nonnull(open_socket);
	g_assert_nonnull(write_body);
	static const char *const private4[] = {
		"0.0.0.0",     "10.1.2.3",   "100.71.64.1",  "127.0.0.2",
		"169.254.1.1", "172.28.5.3", "192.0.0.1",    "192.0.2.1",
		"192.168.1.1", "198.18.1.2", "198.51.100.1", "203.0.113.2",
		"224.0.0.1",
	};
	static const char *const private6[] = {
		"::",
		"::1",
		"::ffff:192.168.1.5",
		"64:ff9b::a00:1",
		"2001:db8::1",
		"2002:a01:203::1",
		"3fff::1",
		"fd00::1",
		"fe80::1",
		"ff02::1",
	};

	if (sockets_checked)
		goto respond;
	sockets_checked = TRUE;
	for (guint i = 0; i < G_N_ELEMENTS(private4); i++)
		g_assert_cmpint(try_address(AF_INET, private4[i], 443), ==,
				CURL_SOCKET_BAD);
	for (guint i = 0; i < G_N_ELEMENTS(private6); i++)
		g_assert_cmpint(try_address(AF_INET6, private6[i], 443), ==,
				CURL_SOCKET_BAD);
	g_assert_cmpint(try_address(AF_INET, "1.2.3.4", 80), ==,
			CURL_SOCKET_BAD);
	int fd = try_address(AF_INET, "1.2.3.4", 443);

	g_assert_cmpint(fd, >=, 0);
	close(fd);
	/* libcurl 8 asks for SOCK_STREAM | SOCK_CLOEXEC; a datagram never. */
	fd = try_typed(AF_INET, "1.2.3.4", 443, SOCK_STREAM | SOCK_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	close(fd);
	g_assert_cmpint(try_typed(AF_INET, "1.2.3.4", 443,
				  SOCK_DGRAM | SOCK_CLOEXEC),
			==, CURL_SOCKET_BAD);
	g_assert_cmpint(try_typed(AF_INET, "10.1.2.3", 443,
				  SOCK_STREAM | SOCK_CLOEXEC),
			==, CURL_SOCKET_BAD);
respond:
	if (oversized) {
		char byte = 0;

		g_assert_cmpuint(write_body(&byte, LINE_FLEX_ASSET_LIMIT + 1, 1,
					    body_data),
				 ==, 0);
		return CURLE_WRITE_ERROR;
	}
	char bitmap[] = "\211PNG\r\n\032\n";
	char riff[] = "RIFF\0\0\0\0WEBPVP8 ";
	gsize n = sizeof(bitmap) - 1;

	if (webp) {
		g_assert_cmpuint(write_body(riff, 1, sizeof(riff) - 1,
					    body_data),
				 ==, sizeof(riff) - 1);
		return CURLE_OK;
	}

	g_assert_cmpuint(write_body(bitmap, 1, n, body_data), ==, n);
	return CURLE_OK;
}

static void url_policy(void)
{
	g_assert_true(line_flex_image_allowed("https://vos.line-scdn.net/a"));
	g_assert_true(line_flex_image_allowed("https://scdn.line-apps.com/a"));
	g_assert_true(line_flex_image_allowed(
		"https://VOS.Line-SCDN.net:443/a?b=%40"));
	g_assert_true(
		line_flex_link_allowed("https://miniapp.line.me/my-card"));
	g_assert_true(line_flex_link_allowed("https://example.org/a#b"));
	/* A sender's own image host loads too, as in LINE's phone app. */
	g_assert_true(line_flex_image_allowed(
		"https://aramen-public-assets.s3.ap-southeast-1.amazonaws.com/a.png"));
	g_assert_false(line_flex_image_allowed("http://example.org/a"));
	static const char *const invalid[] = {
		NULL,
		"",
		"https://",
		"http://example.org",
		"file:///etc/passwd",
		"javascript:alert(1)",
		"https://example.org@127.0.0.1/",
		"https://example.org%40foo.test/a",
		"https://127.1/",
		"https://[::1]/",
		"https://0x7f000001/",
		"https://home.local/",
		"https://xxx.localhost/",
		"https://server.home.arpa/",
		"https://example.org:4",
		"https://example.org:4433/",
		"https://example.org\\@127.0.0.1/",
		"https://example.org/a%",
		"https://example.org/a a",
		"https://example.org/a\n",
	};

	for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_assert_false(line_flex_link_allowed(invalid[i]));
		g_assert_false(line_flex_image_allowed(invalid[i]));
	}
}

static void transfer_policy(void)
{
	const char *url = "https://vos.line-scdn.net/flex/pixel.png";

	g_autoptr(GError) error = NULL;

	response_status = 200;
	g_assert_null(line_flex_image_download("http://example.org/image.png",
					       &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_cmpuint(hits, ==, 0);
	g_autoptr(GBytes) body = line_flex_image_download(url, &error);
	g_assert_no_error(error);
	g_assert_nonnull(body);
	g_assert_cmpuint(g_bytes_get_size(body), ==, 8);
	response_status = 302;
	g_assert_null(line_flex_image_download(url, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	response_status = 200;
	oversized = TRUE;
	g_assert_null(line_flex_image_download(url, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpuint(hits, ==, 3);
}

static void run_script(const long *statuses, guint n)
{
	script = statuses;
	script_len = n;
	script_hit = 0;
	hits = 0;
	g_ptr_array_set_size(requested, 0);
}

static void fallbacks(void)
{
	g_autoptr(GError) error = NULL;
	static const long rejected_width[] = { 400, 200 };
	static const long moved[] = { 302, 200 };
	static const long missing[] = { 404 };

	requested = g_ptr_array_new_with_free_func(g_free);
	oversized = FALSE;
	/* An object-store base rejects /700: ask for the plain base. */
	run_script(rejected_width, 2);
	g_autoptr(GBytes) rich = line_flex_image_download(
		"https://vos.line-scdn.net/rich/base/700", &error);
	g_assert_no_error(error);
	g_assert_nonnull(rich);
	g_assert_cmpuint(requested->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(requested, 1), ==,
			"https://vos.line-scdn.net/rich/base");
	/* Only an imagemap width is retried. */
	run_script(missing, 1);
	g_assert_null(line_flex_image_download(
		"https://vos.line-scdn.net/flex/pixel.png", &error));
	g_clear_error(&error);
	g_assert_cmpuint(requested->len, ==, 1);
	/* Redirects stay on LINE's CDN. */
	run_script(moved, 2);
	redirect_to = "https://vos.line-scdn.net/flex/moved.png";
	g_autoptr(GBytes) followed = line_flex_image_download(
		"https://vos.line-scdn.net/flex/old.png", &error);
	g_assert_no_error(error);
	g_assert_nonnull(followed);
	g_assert_cmpstr(g_ptr_array_index(requested, 1), ==, redirect_to);
	/* Another public HTTPS host is fine; plain HTTP or .local is not. */
	run_script(moved, 2);
	redirect_to = "https://images.example.org/elsewhere.png";
	g_autoptr(GBytes) elsewhere = line_flex_image_download(
		"https://vos.line-scdn.net/flex/old.png", &error);
	g_assert_no_error(error);
	g_assert_nonnull(elsewhere);
	g_assert_cmpstr(g_ptr_array_index(requested, 1), ==, redirect_to);
	run_script(moved, 2);
	redirect_to = "http://example.org/elsewhere.png";
	g_assert_null(line_flex_image_download(
		"https://vos.line-scdn.net/flex/old.png", &error));
	g_clear_error(&error);
	g_assert_cmpuint(requested->len, ==, 1);
	run_script(moved, 2);
	redirect_to = "https://router.local/admin.png";
	g_assert_null(line_flex_image_download(
		"https://vos.line-scdn.net/flex/old.png", &error));
	g_clear_error(&error);
	g_assert_cmpuint(requested->len, ==, 1);
	redirect_to = NULL;
	/* LINE's CDN also serves WebP. */
	run_script(NULL, 0);
	response_status = 200;
	webp = TRUE;
	g_autoptr(GBytes) picture = line_flex_image_download(
		"https://vos.line-scdn.net/flex/photo", &error);
	g_assert_no_error(error);
	g_assert_nonnull(picture);
	webp = FALSE;
	g_clear_pointer(&requested, g_ptr_array_unref);
}

/*
 * "Slow down" from an imagemap host is never answered with the plain-base
 * retry, and the host gets no request at all until its Retry-After passes.
 */
static void slow_down(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 0, 0);
		g_test_trap_assert_passed();
		return;
	}
	g_autoptr(GError) error = NULL;
	static const long busy[] = { 429, 200 };

	requested = g_ptr_array_new_with_free_func(g_free);
	run_script(busy, 2);
	g_assert_null(line_flex_image_download(
		"https://vos.line-scdn.net/rich/base/700", &error));
	g_assert_cmpuint(requested->len, ==, 1);
	g_clear_error(&error);
	g_assert_null(line_flex_image_download(
		"https://vos.line-scdn.net/rich/other/700", &error));
	g_assert_error(error, LINE_ERROR, LINE_RATE_LIMITED);
	g_assert_cmpuint(requested->len, ==, 1);
	g_assert_cmpint(line_cooldown_seconds("https://vos.line-scdn.net/"), >=,
			119);
	g_ptr_array_unref(requested);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/flex-assets/https-and-cdn-origins", url_policy);
	g_test_add_func("/flex-assets/bounded-anonymous-transfer",
			transfer_policy);
	g_test_add_func("/flex-assets/redirects-widths-and-webp", fallbacks);
	g_test_add_func("/flex-assets/slow-down", slow_down);
	return g_test_run();
}
