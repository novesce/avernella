// SPDX-License-Identifier: GPL-3.0-or-later
#include "flex_asset.h"
#include "media.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <string.h>
#include <sys/socket.h>

static gboolean domain_suffix(const char *host, gsize len, const char *domain)
{
	gsize suffix_len = strlen(domain);

	return len >= suffix_len &&
	       (len == suffix_len || host[len - suffix_len - 1] == '.') &&
	       !g_ascii_strncasecmp(host + len - suffix_len, domain,
				    suffix_len);
}

static const char *https_host(const char *url, gsize *host_len)
{
	const char *authority, *end, *label;
	guint labels = 0;
	gboolean tld_has_letter = FALSE;

	if (!url || strnlen(url, 2001) > 2000 ||
	    g_ascii_strncasecmp(url, "https://", 8))
		return NULL;
	authority = url + 8;
	end = authority + strcspn(authority, "/:?#");
	if (end == authority || end - authority > 253)
		return NULL;
	label = authority;
	for (const char *p = authority; p <= end; p++) {
		if (p == end || *p == '.') {
			if (p == label || p - label > 63 || *label == '-' ||
			    p[-1] == '-')
				return NULL;
			labels++;
			if (p == end)
				break;
			label = p + 1;
			tld_has_letter = FALSE;
		} else {
			if (!g_ascii_isalnum(*p) && *p != '-')
				return NULL;
			tld_has_letter |= g_ascii_isalpha(*p);
		}
	}
	*host_len = end - authority;
	if (labels < 2 || !tld_has_letter ||
	    domain_suffix(authority, *host_len, "localhost") ||
	    domain_suffix(authority, *host_len, "local") ||
	    domain_suffix(authority, *host_len, "internal") ||
	    domain_suffix(authority, *host_len, "arpa"))
		return NULL;
	if (*end == ':') {
		if (strncmp(end + 1, "443", 3))
			return NULL;
		end += 4;
		if (*end && !strchr("/?#", *end))
			return NULL;
	}
	for (const char *p = end; *p; p++) {
		if (!g_ascii_isgraph(*p) || *p == '\\')
			return NULL;
		if (*p == '%' &&
		    (!g_ascii_isxdigit(p[1]) || !g_ascii_isxdigit(p[2])))
			return NULL;
	}
	return authority;
}

gboolean line_flex_link_allowed(const char *url)
{
	gsize host_len;

	return https_host(url, &host_len) != NULL;
}

gboolean line_flex_image_allowed(const char *url)
{
	return line_flex_link_allowed(url);
}

static gboolean public_v4(const struct sockaddr_in *addr)
{
	guint32 ip = ntohl(addr->sin_addr.s_addr);
	guint first = ip >> 24, first_two = ip >> 16;
	guint first_three = ip >> 8;

	return addr->sin_port == htons(443) && first && first != 10 &&
	       first != 127 && first < 224 && first_two != 0xa9fe &&
	       (ip >> 20) != 0xac1 && (ip >> 22) != 0x191 &&
	       first_two != 0xc0a8 && first_three != 0xc00000 &&
	       first_three != 0xc00002 && first_three != 0xc63364 &&
	       first_three != 0xcb0071 && (ip >> 17) != 0x6309;
}

static gboolean public_v6(const struct sockaddr_in6 *addr)
{
	const guint8 *ip = addr->sin6_addr.s6_addr;

	/* Accept native global unicast and exclude special/tunnel ranges. */
	if (addr->sin6_port != htons(443) || (ip[0] & 0xe0) != 0x20 ||
	    addr->sin6_scope_id)
		return FALSE;
	return !(ip[0] == 0x20 &&
		 (ip[1] == 2 ||
		  (ip[1] == 1 &&
		   (ip[2] < 2 || (ip[2] == 0x0d && ip[3] == 0xb8))))) &&
	       !(ip[0] == 0x3f && ip[1] == 0xff && !(ip[2] & 0xf0));
}

static curl_socket_t asset_socket(void *data, curlsocktype purpose,
				  struct curl_sockaddr *addr)
{
	(void)data;
	/* libcurl 8.x passes SOCK_CLOEXEC (and may pass SOCK_NONBLOCK) here. */
	int type = addr->socktype & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (purpose != CURLSOCKTYPE_IPCXN || type != SOCK_STREAM)
		return CURL_SOCKET_BAD;
	switch (addr->family) {
	case AF_INET: {
		struct sockaddr_in ip;

		if (addr->addrlen < sizeof(ip))
			return CURL_SOCKET_BAD;
		memcpy(&ip, &addr->addr, sizeof(ip));
		if (!public_v4(&ip))
			return CURL_SOCKET_BAD;
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 ip;

		if (addr->addrlen < sizeof(ip))
			return CURL_SOCKET_BAD;
		memcpy(&ip, &addr->addr, sizeof(ip));
		if (!public_v6(&ip))
			return CURL_SOCKET_BAD;
		break;
	}
	default:
		return CURL_SOCKET_BAD;
	}
	return socket(addr->family, addr->socktype | SOCK_CLOEXEC,
		      addr->protocol);
}

static size_t receive(char *data, size_t chunk, size_t count, void *user)
{
	GByteArray *body = user;

	if (chunk && count > (LINE_FLEX_ASSET_LIMIT - body->len) / chunk)
		return 0;
	g_byte_array_append(body, (const guint8 *)data, chunk * count);
	return chunk * count;
}

static gboolean bitmap(const GByteArray *body)
{
	const guint8 *d = body->data;

	return (body->len >= 3 && !memcmp(d, "\xff\xd8\xff", 3)) ||
	       (body->len >= 8 && !memcmp(d, "\211PNG\r\n\032\n", 8)) ||
	       (body->len >= 12 && !memcmp(d, "RIFF", 4) &&
		!memcmp(d + 8, "WEBP", 4));
}

/*
 * One anonymous GET over a vetted socket. *location receives a redirect
 * target, which the caller follows only to another allowed CDN URL.
 */
static GByteArray *fetch(const char *url, CURLcode *rc, long *status,
			 char **location)
{
	CURL *curl = curl_easy_init();
	GByteArray *body = g_byte_array_new();
	char *where = NULL;

	*rc = CURLE_FAILED_INIT;
	*status = 0;
	*location = NULL;
	if (!curl)
		return body;
#define OPT(name, value)                                   \
	do {                                               \
		*rc = curl_easy_setopt(curl, name, value); \
		if (*rc != CURLE_OK)                       \
			goto done;                         \
	} while (0)
	/* Vetted sockets: tuned, but never pooled with other hosts. */
	if (!line_curl_tune(curl, FALSE))
		goto done;
	OPT(CURLOPT_URL, url);
	OPT(CURLOPT_PROTOCOLS_STR, "https");
	OPT(CURLOPT_PROXY, "");
	OPT(CURLOPT_FOLLOWLOCATION, 0L);
	OPT(CURLOPT_NOSIGNAL, 1L);
	OPT(CURLOPT_CONNECTTIMEOUT_MS, 3000L);
	OPT(CURLOPT_TIMEOUT_MS, 8000L);
	OPT(CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)LINE_FLEX_ASSET_LIMIT);
	OPT(CURLOPT_OPENSOCKETFUNCTION, asset_socket);
	OPT(CURLOPT_WRITEFUNCTION, receive);
	OPT(CURLOPT_WRITEDATA, body);
#undef OPT
	*rc = curl_easy_perform(curl);
	if (*rc == CURLE_OK)
		*rc = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	if (*rc == CURLE_OK)
		line_cooldown_note(url, curl, *status);
	if (*rc == CURLE_OK && *status >= 300 && *status < 400 &&
	    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &where) != CURLE_OK)
		where = NULL;
	*location = g_strdup(where);
done:
	curl_easy_cleanup(curl);
	return body;
}

/*
 * LINE's imagemap convention asks for <base>/<width>, but rich messages
 * hosted in LINE's object store answer only at the plain base URL.
 */
static gsize imagemap_base(const char *url)
{
	static const char *const widths[] = { "/240", "/300", "/460", "/700",
					      "/1040" };
	gsize n = strlen(url);

	for (guint i = 0; i < G_N_ELEMENTS(widths); i++)
		if (g_str_has_suffix(url, widths[i]))
			return n - strlen(widths[i]);
	return 0;
}

GBytes *line_flex_image_download(const char *url, GError **error)
{
	if (!line_flex_image_allowed(url)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Unsupported Flex image source.");
		return NULL;
	}
	g_autofree char *current = g_strdup(url);
	gboolean debug = !g_strcmp0(g_getenv("LINE_DEBUG_MEDIA"), "1");
	gboolean plain = FALSE;

	for (int hop = 0; hop < 4; hop++) {
		g_autofree char *location = NULL;
		CURLcode rc;
		long status;

		/* A host that asked us to wait gets no request, LINE or not. */
		if (line_cooldown_check(current, error))
			return NULL;
		g_autoptr(GByteArray) body =
			fetch(current, &rc, &status, &location);

		if (rc == CURLE_OK && status == 200 && bitmap(body))
			return g_byte_array_free_to_bytes(
				g_steal_pointer(&body));
		if (debug)
			g_printerr(
				"line-media image fetch %d: curl %d, HTTP %ld, %u bytes\n",
				hop + 1, (int)rc, status, body->len);
		if (rc != CURLE_OK)
			break;
		if (status >= 300 && status < 400 && location &&
		    line_flex_image_allowed(location)) {
			g_free(current);
			current = g_steal_pointer(&location);
			continue;
		}
		/*
		 * fetch() recorded the host's cooldown. Never answer "slow
		 * down" by trying another URL at once.
		 */
		if (status == 429 || status == 503)
			break;
		gsize base = imagemap_base(current);

		if (status >= 400 && status < 500 && !plain && base) {
			current[base] = 0;
			plain = TRUE;
			continue;
		}
		break;
	}
	g_set_error_literal(error, LINE_ERROR, 1,
			    "Could not load this Flex image.");
	return NULL;
}

const char *line_hosted_image_url(J *message)
{
	static const char *const keys[] = { "DOWNLOAD_URL", "PREVIEW_URL" };
	J *meta = field(message, "18");

	if (json_object_get_int(field(message, "15")) != LINE_MEDIA_IMAGE)
		return NULL;
	for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
		const char *url = strfield(meta, keys[i]);

		if (url && line_flex_image_allowed(url))
			return url;
	}
	return NULL;
}
