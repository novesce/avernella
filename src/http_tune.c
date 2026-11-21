// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include <curl/curl.h>

static gsize curl_initialized;

gboolean line_curl_global(void)
{
	if (g_once_init_enter(&curl_initialized))
		g_once_init_leave(&curl_initialized,
				  curl_global_init(CURL_GLOBAL_DEFAULT) ==
						  CURLE_OK ?
					  1 :
					  2);
	return curl_initialized == 1;
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
