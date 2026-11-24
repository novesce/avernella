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

gboolean line_liff_debug_url(const char *url)
{
	static const char prefix[] = "http://127.0.0.1:";
	gsize n = sizeof(prefix) - 1, digits = 0;

	if (!url || strlen(url) >= LINE_LIFF_PACKET ||
	    g_ascii_strncasecmp(url, prefix, n))
		return FALSE;
	/*
	 * The authority must be exactly the address and a port, so neither
	 * user info ("127.0.0.1:80@example.com") nor a backslash, which
	 * WebKit reads as a slash, can move the real host elsewhere.
	 */
	while (g_ascii_isdigit(url[n + digits]))
		digits++;
	if (!digits || digits > 5 ||
	    (url[n + digits] && !strchr("/?#", url[n + digits])))
		return FALSE;
	for (const char *p = url; *p; p++)
		if (!g_ascii_isgraph(*p) || *p == '\\')
			return FALSE;
	g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);

	return uri && !g_uri_get_userinfo(uri) &&
	       !g_ascii_strcasecmp(g_uri_get_scheme(uri), "http") &&
	       !g_strcmp0(g_uri_get_host(uri), "127.0.0.1") &&
	       g_uri_get_port(uri) >= 1024 && g_uri_get_port(uri) <= 65535;
}

gboolean line_liff_web_url(const char *url, gboolean debug)
{
	if (!url)
		return FALSE;
	if (!g_ascii_strncasecmp(url, "https://", 8) ||
	    !g_ascii_strncasecmp(url, "about:", 6) ||
	    !g_ascii_strncasecmp(url, "data:", 5) ||
	    !g_ascii_strncasecmp(url, "blob:", 5))
		return TRUE;
	return debug && line_liff_debug_url(url);
}

#define DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

/* A real directory of ours that no other user can write to. */
static gboolean own_dir(int fd)
{
	struct stat st;

	return !fstat(fd, &st) && S_ISDIR(st.st_mode) &&
	       st.st_uid == geteuid() && !(st.st_mode & 022);
}

char *line_liff_storage_dir(const char *base)
{
	g_autofree char *home = g_build_filename(base, "avernella", NULL);
	static const char *const legacy[] = { "kitatide", "line-linux" };
	g_autofree char *target = g_build_filename(home, "liff", NULL);
	struct stat st;

	if (g_mkdir_with_parents(home, 0700) < 0)
		return NULL;
	/*
	 * WebKit opens the path by name, so check the parent first: when it
	 * is ours alone, no one else can swap the directory inside it.
	 */
	int dir = open(home, DIR_FLAGS);

	if (dir < 0)
		return NULL;
	if (!own_dir(dir)) {
		close(dir);
		return NULL;
	}
	if (fstatat(dir, "liff", &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT)
		for (gsize i = 0; i < G_N_ELEMENTS(legacy); i++) {
			g_autofree char *parent =
				g_build_filename(base, legacy[i], NULL);
			g_autofree char *source =
				g_build_filename(parent, "liff", NULL);

			if (lstat(source, &st) || !S_ISDIR(st.st_mode) ||
			    st.st_uid != geteuid())
				continue;
			if (renameat2(AT_FDCWD, source, dir, "liff",
				      RENAME_NOREPLACE) == 0) {
				/* Drop the old parent only if it is empty. */
				(void)rmdir(parent);
				break;
			}
		}
	if (mkdirat(dir, "liff", 0700) && errno != EEXIST) {
		close(dir);
		return NULL;
	}
	/* Never through a symlink, never someone else's; ours turns 0700. */
	int fd = openat(dir, "liff", DIR_FLAGS);
	gboolean ok = fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) &&
		      st.st_uid == geteuid() &&
		      (!(st.st_mode & 077) || !fchmod(fd, 0700));

	if (fd >= 0)
		close(fd);
	close(dir);
	return ok ? g_steal_pointer(&target) : NULL;
}
