// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static gsize curl_initialized;

gboolean line_curl_global(void)
{
	if (g_once_init_enter(&curl_initialized))
		g_once_init_leave(
			&curl_initialized,
			curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 1 :
									    2);
	return curl_initialized == 1;
}

/* Header lists are copies: an access token in one outlives the request. */
void line_curl_headers_free(struct curl_slist *headers)
{
	for (struct curl_slist *h = headers; h; h = h->next)
		if (h->data)
			explicit_bzero(h->data, strlen(h->data));
	curl_slist_free_all(headers);
}

/*
 * Share DNS answers and reusable TLS sessions. The connection cache belongs
 * to each easy handle: libcurl does not support sharing it across threads.
 * Keep these process-wide caches alive while detached syncs unwind.
 */
static CURLSH *share;
static GMutex share_locks[CURL_LOCK_DATA_LAST];

static void share_lock(CURL *handle, curl_lock_data data,
		       curl_lock_access access, void *user)
{
	(void)handle, (void)access, (void)user;
	g_mutex_lock(&share_locks[data]);
}

static void share_unlock(CURL *handle, curl_lock_data data, void *user)
{
	(void)handle, (void)user;
	g_mutex_unlock(&share_locks[data]);
}

static CURLSH *shared_cache(void)
{
	static gsize ready;

	if (g_once_init_enter(&ready)) {
		CURLSH *sh = line_curl_global() ? curl_share_init() : NULL;

		if (sh &&
		    (curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, share_lock) ||
		     curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC,
				       share_unlock) ||
		     curl_share_setopt(sh, CURLSHOPT_SHARE,
				       CURL_LOCK_DATA_DNS) ||
		     curl_share_setopt(sh, CURLSHOPT_SHARE,
				       CURL_LOCK_DATA_SSL_SESSION))) {
			curl_share_cleanup(sh);
			sh = NULL;
		}
		share = sh;
		g_once_init_leave(&ready, 1);
	}
	return share;
}

gboolean line_curl_tune(CURL *handle, gboolean pooled)
{
	CURLSH *sh = pooled ? shared_cache() : NULL;

	if (curl_easy_setopt(handle, CURLOPT_SSLVERSION,
			     (long)CURL_SSLVERSION_TLSv1_2) ||
	    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L) ||
	    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L))
		return FALSE;
	/* Negotiate HTTP/2 where supported, with HTTP/1.1 fallback. */
	curl_easy_setopt(handle, CURLOPT_HTTP_VERSION,
			 (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(handle, CURLOPT_TCP_NODELAY, 1L);
	curl_easy_setopt(handle, CURLOPT_TCP_FASTOPEN, 1L);
	curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 25L);
	curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 10L);
	curl_easy_setopt(handle, CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS, 200L);
	curl_easy_setopt(handle, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
	curl_easy_setopt(handle, CURLOPT_MAXAGE_CONN, 600L);
	if (sh)
		curl_easy_setopt(handle, CURLOPT_SHARE, sh);
	return TRUE;
}

/*
 * One deadline per host, so a cooldown on obs.line-apps.com doesn't stop
 * chat RPCs and the reverse. The table stays small: expired entries are
 * dropped whenever a new host is added.
 */
enum {
	COOLDOWN_DEFAULT_SECONDS = 30,
	COOLDOWN_HOSTS = 64,
};

static GMutex cooldown_lock;
static GHashTable *cooldowns; /* lowercase host → monotonic µs deadline */
static gint64 rpc_until;
static int rpc_dir = -1;

#define RPC_WAIT_FILE "rpc.retry-until"

/* No account details on disk, just the absolute UTC deadline in microseconds. */
static void save_rpc_wait(gint64 expiry)
{
	g_autofree char *path =
		g_strdup_printf("/proc/self/fd/%d/" RPC_WAIT_FILE, rpc_dir);
	gint64 record = GINT64_TO_LE(expiry);

	if (!g_file_set_contents_full(path, (const char *)&record,
				      sizeof(record),
				      G_FILE_SET_CONTENTS_CONSISTENT |
					      G_FILE_SET_CONTENTS_DURABLE,
				      0600, NULL))
		g_printerr(
			"Could not save LINE's retry time for the next launch.\n");
}

/* The store is private and exclusively locked by the worker. An absent or
 * damaged optional record must leave manual sign-in and sign-out usable.
 */
void line_cooldown_use_store(int dir)
{
	g_mutex_lock(&cooldown_lock);
	rpc_dir = dir;
	if (dir >= 0) {
		int fd = openat(dir, RPC_WAIT_FILE,
				O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		struct stat st;
		gint64 record;

		if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) &&
		    st.st_uid == geteuid() && !(st.st_mode & 077) &&
		    st.st_size == sizeof(record) &&
		    TEMP_FAILURE_RETRY(read(fd, &record, sizeof(record))) ==
			    sizeof(record)) {
			gint64 expiry = GINT64_FROM_LE(record);
			gint64 now = g_get_real_time();

			if (expiry <= now)
				unlinkat(dir, RPC_WAIT_FILE, 0);
			else if (expiry - now <=
				 (gint64)G_MAXINT * G_USEC_PER_SEC)
				rpc_until =
					MAX(rpc_until, g_get_monotonic_time() +
							       (expiry - now));
		}
		if (fd >= 0)
			close(fd);
	}
	g_mutex_unlock(&cooldown_lock);
}

static char *cooldown_host(const char *url)
{
	g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) :
				    NULL;
	const char *host = uri ? g_uri_get_host(uri) : NULL;

	return host && *host ? g_ascii_strdown(host, -1) : NULL;
}

static gboolean expired(gpointer key, gpointer value, gpointer now)
{
	(void)key;
	return *(gint64 *)value <= *(gint64 *)now;
}

int line_cooldown_seconds(const char *url)
{
	g_autofree char *host = cooldown_host(url);
	gint64 until = 0;

	if (!host)
		return 0;
	g_mutex_lock(&cooldown_lock);
	if (!strcmp(host, LINE_RPC_HOST))
		until = rpc_until;
	else if (cooldowns) {
		gint64 *found = g_hash_table_lookup(cooldowns, host);

		if (found)
			until = *found;
	}
	g_mutex_unlock(&cooldown_lock);
	gint64 now = g_get_monotonic_time();

	if (until <= now)
		return 0;
	return (until - now + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC;
}

void line_cooldown_set(const char *url, int seconds)
{
	g_autofree char *host = cooldown_host(url);

	if (!host || seconds <= 0)
		return;
	gint64 now = g_get_monotonic_time();
	gint64 until = now + (gint64)seconds * G_USEC_PER_SEC;

	g_mutex_lock(&cooldown_lock);
	if (!strcmp(host, LINE_RPC_HOST)) {
		if (until > rpc_until) {
			rpc_until = until;
			if (rpc_dir >= 0)
				save_rpc_wait(g_get_real_time() +
					      (until - g_get_monotonic_time()));
		}
		g_mutex_unlock(&cooldown_lock);
		return;
	}
	if (!cooldowns)
		cooldowns = g_hash_table_new_full(g_str_hash, g_str_equal,
						  g_free, g_free);
	gint64 *found = g_hash_table_lookup(cooldowns, host);

	if (found) {
		*found = MAX(*found, until);
	} else {
		g_hash_table_foreach_remove(cooldowns, expired, &now);
		if (g_hash_table_size(cooldowns) < COOLDOWN_HOSTS)
			g_hash_table_insert(cooldowns, g_steal_pointer(&host),
					    g_memdup2(&until, sizeof(until)));
	}
	g_mutex_unlock(&cooldown_lock);
}

void line_cooldown_note(const char *url, CURL *handle, long status)
{
	if (status != 429 && status != 503)
		return;
	curl_off_t seconds = 0;

	if (!handle ||
	    curl_easy_getinfo(handle, CURLINFO_RETRY_AFTER, &seconds) ||
	    seconds <= 0)
		seconds = COOLDOWN_DEFAULT_SECONDS;
	line_cooldown_set(url, (int)MIN(seconds, (curl_off_t)G_MAXINT));
}

gboolean line_cooldown_check(const char *url, GError **error)
{
	int wait = line_cooldown_seconds(url);

	if (!wait)
		return FALSE;
	g_set_error(error, LINE_ERROR, LINE_RATE_LIMITED,
		    "LINE asked to wait. Try again in %d seconds.", wait);
	return TRUE;
}
