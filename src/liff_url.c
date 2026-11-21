// SPDX-License-Identifier: GPL-3.0-or-later
/* LIFF URL recognition, shared by the UI, the worker and the browser. */
#include "liff_url.h"
#include <string.h>

static const char *const prefixes[] = {
	"https://liff.line.me/",
	"https://miniapp.line.me/",
	"https://line.me/R/app/",
	"line://app/",
};

/* LIFF IDs are a channel ID and a suffix: 1661444788-rnvmoOo8. */
static gsize liff_id_length(const char *p)
{
	gsize n = 0, digits;

	while (g_ascii_isdigit(p[n]))
		n++;
	digits = n;
	if (!digits || digits > 20 || p[n] != '-')
		return 0;
	n++;
	while (g_ascii_isalnum(p[n]))
		n++;
	if (n == digits + 1 || n > 64)
		return 0;
	return n;
}

gboolean line_liff_url(const char *url, char **liff_id, char **rest)
{
	if (!url || strlen(url) > 4096)
		return FALSE;
	for (gsize i = 0; i < G_N_ELEMENTS(prefixes); i++) {
		gsize skip = strlen(prefixes[i]);

		if (g_ascii_strncasecmp(url, prefixes[i], skip))
			continue;
		const char *id = url + skip;
		gsize n = liff_id_length(id);

		if (!n || (id[n] && !strchr("/?#", id[n])))
			return FALSE;
		for (const char *p = id + n; *p; p++)
			if (!g_ascii_isgraph(*p) || *p == '\\')
				return FALSE;
		if (liff_id)
			*liff_id = g_strndup(id, n);
		if (rest)
			*rest = g_strdup(id + n);
		return TRUE;
	}
	return FALSE;
}

gboolean line_liff_line_scheme(const char *url)
{
	return url && (!g_ascii_strncasecmp(url, "line://", 7) ||
		       !g_ascii_strncasecmp(url, "lineb://", 8));
}
