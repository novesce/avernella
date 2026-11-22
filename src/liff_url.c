// SPDX-License-Identifier: GPL-3.0-or-later
/* LIFF URL recognition, shared by the UI, the worker and the browser. */
#include "liff_url.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

char *line_liff_storage_dir(const char *base)
{
	g_autofree char *home = g_build_filename(base, "avernella", NULL);
	static const char *const legacy[] = { "kitatide", "line-linux" };
	char *target = g_build_filename(home, "liff", NULL);
	struct stat st;

	if (!lstat(target, &st) || errno != ENOENT)
		return target;
	if (g_mkdir_with_parents(home, 0700) < 0)
		return target;
	for (gsize i = 0; i < G_N_ELEMENTS(legacy); i++) {
		g_autofree char *parent =
			g_build_filename(base, legacy[i], NULL);
		g_autofree char *source =
			g_build_filename(parent, "liff", NULL);

		if (lstat(source, &st) || !S_ISDIR(st.st_mode) ||
		    st.st_uid != geteuid())
			continue;
		if (renameat2(AT_FDCWD, source, AT_FDCWD, target,
			      RENAME_NOREPLACE) == 0) {
			/* Drop the old parent only if nothing else is in it. */
			(void)rmdir(parent);
			break;
		}
	}
	return target;
}
