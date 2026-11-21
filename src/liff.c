// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * LiffService, as LINE's clients use it (LINE 26.16 for Android, see
 * analysis/liff.md): issueLiffView on /LIFF1 returns the app's endpoint
 * and the tokens the LIFF SDK reads from location.hash in liff.init().
 */
#include "liff.h"
#include <string.h>

/* LiffErrorCode values in LiffException.code. */
enum {
	LIFF_INVALID_REQUEST = 1,
	LIFF_UNAUTHORIZED = 2,
	LIFF_CONSENT_REQUIRED = 3,
	LIFF_VERSION_UPDATE_REQUIRED = 4,
	LIFF_COMPREHENSIVE_AGREEMENT_REQUIRED = 5,
	LIFF_SPLASH_SCREEN_REQUIRED = 6,
	LIFF_PERMANENT_LINK_INVALID_REQUEST = 7,
	LIFF_NO_DESTINATION_URL = 8,
	LIFF_SERVICE_ALREADY_TERMINATED = 9,
	LIFF_ID_FEDERATION_REQUIRED = 10,
};

/* LINE's language tag, as in the x-lal header: en_US, ja_JP. */
static char *liff_lang(void)
{
	const char *const *names = g_get_language_names();
	const char *name = names && names[0] ? names[0] : "C";
	gsize n = strcspn(name, ".@");

	if (!n || !g_ascii_isalpha(name[0]) || !strncmp(name, "C", n) ||
	    !strncmp(name, "POSIX", n) || n > 16)
		return g_strdup("en_US");
	return g_strndup(name, n);
}

static void put_str(J *o, const char *key, const char *value)
{
	json_object_object_add(o, key, json_object_new_string(value));
}

static void add_token(GString *fragment, const char *name, const char *value)
{
	if (!value || !*value)
		return;
	if (fragment->len)
		g_string_append_c(fragment, '&');
	g_string_append(fragment, name);
	g_string_append_c(fragment, '=');
	g_string_append_uri_escaped(fragment, value, NULL, FALSE);
}

char *line_liff_launch_url(const char *endpoint, const char *rest, J *response)
{
	if (!endpoint || g_ascii_strncasecmp(endpoint, "https://", 8))
		return NULL;
	g_autoptr(GUri) uri = g_uri_parse(endpoint, G_URI_FLAGS_ENCODED, NULL);

	if (!uri || !g_uri_get_host(uri) || !*g_uri_get_host(uri))
		return NULL;
	rest = rest ? rest : "";
	gsize path_len = strcspn(rest, "?#");
	const char *query = rest[path_len] == '?' ? rest + path_len + 1 : NULL;
	gsize query_len = query ? strcspn(query, "#") : 0;
	const char *hash = strchr(rest, '#');

	/* LINE appends the deep link's path below the endpoint's own path. */
	g_autoptr(GString) path = g_string_new(g_uri_get_path(uri));

	if (path_len && strncmp(rest, "/", path_len)) {
		while (path->len && path->str[path->len - 1] == '/')
			g_string_truncate(path, path->len - 1);
		g_string_append_len(path, rest, path_len);
	}
	g_autoptr(GString) search = g_string_new(g_uri_get_query(uri));

	if (query_len) {
		if (search->len)
			g_string_append_c(search, '&');
		g_string_append_len(search, query, query_len);
	}
	/* liff.init() takes these from the fragment, then strips them. */
	g_autoptr(GString) fragment = g_string_new(NULL);

	add_token(fragment, "context_token", strfield(response, "2"));
	add_token(fragment, "feature_token", strfield(response, "4"));
	add_token(fragment, "access_token", strfield(response, "3"));
	add_token(fragment, "id_token", strfield(response, "7"));
	add_token(fragment, "client_id", strfield(response, "6"));
	if (hash && hash[1]) {
		if (fragment->len)
			g_string_append_c(fragment, '&');
		g_string_append(fragment, hash + 1);
	}
	return g_uri_join(G_URI_FLAGS_ENCODED, "https", g_uri_get_userinfo(uri),
			  g_uri_get_host(uri), g_uri_get_port(uri), path->str,
			  search->len ? search->str : NULL,
			  fragment->len ? fragment->str : NULL);
}

static const char *view_size(J *view)
{
	const char *type = strfield(view, "1");

	if (type && (!g_ascii_strcasecmp(type, "compact") ||
		     !g_ascii_strcasecmp(type, "tall")))
		return !g_ascii_strcasecmp(type, "tall") ? "tall" : "compact";
	return "full";
}

static J *page(const char *url, const char *size, const char *title)
{
	J *result = obj();

	put_str(result, "url", url);
	put_str(result, "size", size);
	if (title && *title && g_utf8_validate(title, -1, NULL))
		put_str(result, "title", title);
	return result;
}

static const char *refusal(int code)
{
	switch (code) {
	case LIFF_VERSION_UPDATE_REQUIRED:
		return "This LINE app needs a newer LINE.";
	case LIFF_COMPREHENSIVE_AGREEMENT_REQUIRED:
	case LIFF_SPLASH_SCREEN_REQUIRED:
	case LIFF_ID_FEDERATION_REQUIRED:
		return "Open this LINE app on your phone once, then try again.";
	case LIFF_NO_DESTINATION_URL:
	case LIFF_INVALID_REQUEST:
		return "LINE could not find this LINE app.";
	case LIFF_SERVICE_ALREADY_TERMINATED:
		return "This LINE app has ended its service.";
	case LIFF_UNAUTHORIZED:
		return "LINE did not let this account open the app.";
	default:
		return NULL;
	}
}

/*
 * LiffException.payload is a union: consentRequired (3) carries the page
 * where the user agrees, permanentLinkInvalidRequest (4) a fallback page.
 */
static J *exception_page(int code)
{
	J *payload = field(line_service_exception(), "3");
	const char *url = NULL;

	if (code == LIFF_CONSENT_REQUIRED)
		url = strfield(field(payload, "3"), "2");
	else if (code == LIFF_PERMANENT_LINK_INVALID_REQUEST)
		url = strfield(field(payload, "4"), "2");
	if (!url || g_ascii_strncasecmp(url, "https://", 8))
		return NULL;
	return page(url, "full", NULL);
}

J *line_liff_view(Line *s, const char *url, const char *chat, GError **error)
{
	g_autofree char *liff_id = NULL, *rest = NULL;

	if (!line_liff_url(url, &liff_id, &rest)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This is not a LINE app link.");
		return NULL;
	}
	g_autofree char *lang = liff_lang();
	J *request = obj(), *context = obj();

	put_str(request, "1:str", liff_id);
	if (chat && *chat) {
		J *in_chat = obj();

		put_str(in_chat, "1:str", chat);
		json_object_object_add(context, "2:obj", in_chat);
	} else {
		json_object_object_add(context, "1:obj", obj());
	}
	json_object_object_add(request, "2:obj", context);
	put_str(request, "3:str", lang);
	g_autoptr(J) args = obj();

	json_object_object_add(args, "1:obj", request);
	g_autoptr(GError) failure = NULL;
	g_autoptr(J) response =
		line_rpc(s, "/LIFF1", "issueLiffView", args, &failure);

	if (!response) {
		int code = s->service_code;
		J *fallback = code > 0 ? exception_page(code) : NULL;
		const char *reason = code > 0 ? refusal(code) : NULL;

		if (fallback)
			return fallback;
		if (reason &&
		    !g_error_matches(failure, LINE_ERROR, LINE_SESSION_EXPIRED))
			g_set_error_literal(error, LINE_ERROR, 1, reason);
		else
			g_propagate_error(error, g_steal_pointer(&failure));
		return NULL;
	}
	J *view = field(response, "1");
	g_autofree char *launch =
		line_liff_launch_url(strfield(view, "2"), rest, response);

	if (!launch) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "LINE returned no page for this LINE app.");
		return NULL;
	}
	return page(launch, view_size(view), strfield(view, "22"));
}
