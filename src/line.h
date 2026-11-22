/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <json-c/json.h>
#include <string.h>

typedef struct json_object J;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(J, json_object_put)

/*
 * Session state is private to the worker. keys is a JSON object managed by
 * sealing.c. access and refresh are printable tokens, mid is our LINE user ID.
 */
typedef struct {
	char *access, *refresh, *mid;
	J *keys;
	GArray *history_secrets; /* transient authenticated X25519 results, wiped on release */
	guint next_secret;
	GHashTable *profiles; /* bounded, session-only display names */
	J *pictures; /* session-only profile picture paths from LINE */
	char *obs_token; /* scoped to this live session, never serialized */
	gint64 obs_until;
	GPtrArray *emoji_packs; /* account-scoped catalog, held in RAM */
	gint64 emoji_catalog_until;
	struct line_shared *shared;
	gint64 seq, seq_reserved;
	/* Unix time when LINE asks for a token refresh; 0 when unknown. */
	gint64 refresh_at;
	int service_code; /* logical error code from the last RPC, -1 otherwise */
	long http_status; /* HTTP status of the last RPC, 0 without a response */
	/*
	 * LINE refused this session's credentials. As in LINE's own client, no
	 * further request goes out with them; the worker signs out.
	 */
	gboolean denied;
	gboolean token_changed;
} Line;

/*
 * Compact Thrift request structs are objects with typed keys ("1:str",
 * "2:i32", "3:i64", "4:bool", "5:obj", "6:list:str", "7:map:str:str").
 * str includes binary with json_object_new_string_len. Response field keys
 * are untyped decimal IDs ("1", "2" ...). A successful void result is {}.
 * Args are borrowed (NULL for empty). Caller owns the returned object.
 * line_rpc uses s->access and a 30s timeout.
 */
J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error);
/* Renew the access and refresh tokens now, as LINE's own clients do. */
gboolean line_refresh_session(Line *s, GError **error);
/* Record LINE's durationUntilRefreshInSec and tokenIssueTimeEpochSec. */
void line_schedule_refresh(Line *s, J *duration, J *issued);
/* Whether a scheduled renewal is due at Unix time now. */
gboolean line_refresh_due(const Line *s, gint64 now);
/* Exponential retry delay: first, 2*first, 4*first ... capped at cap. */
gint64 line_retry_delay(guint failures, gint64 first, gint64 cap);
/* Seconds remaining in the RPC origin's HTTP Retry-After window. */
int line_transport_retry_after(void);
J *line_call(Line *s, const char *path, const char *method, J *args,
	     const char *access, int poll_seconds, GError **error);
J *line_auth_call(Line *s, const char *path, const char *method, J *args,
		  GError **error);
J *line_auth_wait(Line *s, const char *verifier, GError **error);
J *line_decode_struct(const guint8 *bytes, gsize n, GError **error);
GBytes *line_encode_call(const char *method, J *args, GError **error);
J *line_decode_reply(const char *method, const guint8 *bytes, gsize n,
		     int *code, GError **error);
/*
 * The service exception struct of this thread's last failed reply, such as
 * a LiffException with its payload. Borrowed until the thread's next reply.
 */
J *line_service_exception(void);
GBytes *line_encode_auth(const char *method, J *args, GError **error);
J *line_decode_auth(const char *method, const guint8 *bytes, gsize n, int *code,
		    GError **error);

static inline J *obj(void)
{
	return json_object_new_object();
}

static inline J *field(J *o, const char *name)
{
	J *v = NULL;

	if (o && json_object_is_type(o, json_type_object))
		json_object_object_get_ex(o, name, &v);
	return v;
}

static inline const char *strfield(J *o, const char *name)
{
	J *v = field(o, name);

	return json_object_is_type(v, json_type_string) ?
		       json_object_get_string(v) :
		       NULL;
}

#define LINE_ERROR (g_quark_from_static_string("avernella"))

/*
 * The one client identity every LINE request carries. Keep the version the
 * same everywhere: a request that names another version than the session's
 * other requests looks like a second, different client.
 */
#define LINE_APP_HEADER \
	"x-line-application: DESKTOPWIN\t9.7.0.3556\tWINDOWS\t10.0.0-NT-x64"
#define LINE_USER_AGENT "Line/9.7.0.3556"

/*
 * LINE's language tag, as in the x-lal header and LiffViewRequest.lang:
 * the desktop locale such as th_TH or ja_JP, or en_US for C and POSIX.
 * Every request uses the same tag, as LINE's own clients do.
 */
static inline void line_language_tag(char tag[17])
{
	const char *const *names = g_get_language_names();
	const char *name = names && names[0] ? names[0] : "C";
	gsize n = strcspn(name, ".@");
	gboolean valid = n > 0 && n < 17;

	for (gsize i = 0; valid && i < n; i++)
		valid = g_ascii_isalnum(name[i]) || name[i] == '_' ||
			name[i] == '-';
	if (!valid || !g_ascii_isalpha(name[0]) || !strncmp(name, "C", n) ||
	    !strncmp(name, "POSIX", n)) {
		name = "en_US";
		n = 5;
	}
	memcpy(tag, name, n);
	tag[n] = 0;
}

enum {
	LINE_TIMEOUT = 2,
	LINE_METHOD_UNAVAILABLE = 3,
	LINE_SESSION_EXPIRED = 4,
	LINE_OBS_UNAUTHORIZED = 5,
	LINE_ASSET_NOT_FOUND = 6,
	LINE_OWN_KEY_MISSING = 7,
	/* A saved session could not reach LINE; it stays signed in. */
	LINE_RETRY = 8,
	LINE_RATE_LIMITED = 9,
	/* HTTP 401 or 403: LINE refused the request's credentials. */
	LINE_ACCESS_DENIED = 10
};

/*
 * TalkException codes that mean LINE no longer accepts this desktop's
 * session. LINE for Android signs a linked device out on the first of
 * AUTHENTICATION_FAILED (1), NOT_AVAILABLE_USER (7) or
 * NOT_AUTHORIZED_DEVICE (8) (ac5.e and ac5.b in LINE 26.x). This client has
 * also always ended it on NOT_AUTHORIZED_SESSION (14) and NOT_AUTHENTICATED
 * (17).
 */
static inline gboolean line_auth_rejected(int code)
{
	return code == 1 || code == 7 || code == 8 || code == 14 || code == 17;
}

/*
 * TalkException codes that ask a client to slow down: EXCESSIVE_ACCESS (4),
 * CONGESTION_CONTROL (58) and SERVER_BUSY (101).
 */
static inline gboolean line_busy_code(int code)
{
	return code == 4 || code == 58 || code == 101;
}

typedef void CURL;

/*
 * Per-host cooldowns shared by every thread and every HTTP path that reaches
 * LINE (RPCs, object storage, CDNs, albums and notes, card artwork). A 429 or
 * 503 answer sets one from Retry-After, or 30 seconds without it; until it
 * passes, requests to that host fail at once with LINE_RATE_LIMITED.
 */
int line_cooldown_seconds(const char *url);
/* Records a cooldown when status is 429 or 503. handle may be NULL. */
void line_cooldown_note(const char *url, CURL *handle, long status);
/* Starts or extends a cooldown for the URL's host. */
void line_cooldown_set(const char *url, int seconds);
/* Sets error to LINE_RATE_LIMITED and returns TRUE while url cools down. */
gboolean line_cooldown_check(const char *url, GError **error);

/* Linux and glibc tuning (tune.c). */
enum line_thread_role {
	LINE_THREAD_MEDIA,	/* call audio and video: tight timers */
	LINE_THREAD_BACKGROUND, /* image fetch and decode: batch scheduling */
};
void line_tune_process(void);
void line_tune_thread(enum line_thread_role role);
/* Returns freed heap pages to the kernel. */
void line_trim_memory(void);
/*
 * Aggressive libcurl defaults: HTTP/2, TCP Fast Open, keepalive and, when
 * 'pooled', process-wide DNS and TLS session caches. FALSE if TLS security
 * options could not be set.
 */
gboolean line_curl_tune(CURL *handle, gboolean pooled);
/* curl_global_init once per process; FALSE if it failed. */
gboolean line_curl_global(void);
/* Frees the calling thread's HTTPS handle. */
void line_transport_release(void);
void line_emit(J *packet);
void line_event(const char *event, const char *key, const char *value);
gboolean line_pair(Line *s, GError **error);
#define LINE_CREDENTIAL_LIMIT 255
#define LINE_CREDENTIAL_FD 3
struct line_credentials {
	char email[LINE_CREDENTIAL_LIMIT + 1];
	char password[LINE_CREDENTIAL_LIMIT + 1];
};
gboolean line_password_login(Line *s, const char *email, char *password,
			     GError **error);
