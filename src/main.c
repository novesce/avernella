// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include "login_retry.h"
#include "messaging.h"
#include "liff.h"
#include "friends.h"
#include "groups.h"
#include "square.h"
#include "call.h"
#include "call_group.h"
#include "emoji.h"
#include "flex_asset.h"
#include "media.h"
#include "media_io.h"
#include "obs_http.h"
#include "offline_store.h"
#include "profile_image.h"
#include "shared_content.h"
#include "state.h"
#include "state_crypto.h"
#include "state_keyring_config.h"
#if LINE_HAVE_LIBSECRET
#include "state_keyring.h"
#endif
#include "stickers.h"
#include "talk_sync.h"
#include "live_sync.h"
#include "net_route.h"
#include "monitor_picker.h"
#include "sandbox.h"
#include "sealing.h"
#include "security/ipc.h"
#include "security/sandbox.h"
#include "security/vault.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <unistd.h>

int line_ui(int argc, char **argv);

struct image_job {
	char *path, *product, *emoji;
	char *read_chat, *read_self, *read_sync, *read_next_sync;
	gboolean read_changed;
	char *access, *next_access;
	J *ranges;
	gint64 request_id;
	GBytes *bytes;
	GError *error;
	guint generation;
	gboolean animated, sticker, sticker_sound, flex;
};

struct line_worker {
	Line session;
	LineStickers *stickers;
	int dir, timer, renewal_timer, credentials_fd;
	gboolean ready, latest_history, foreground;
	gboolean offline, verify_on_reconnect, online_chats, archived_page;
	gboolean archive_cleanup_failed;
	LineOffline *archive;
	J *outbox;
	GHashTable *checked_sends;
	gint64 outbox_due;
	struct line_talk_sync sync;
	/* Long-poll sync thread; the timer polls only while it fails. */
	struct line_live_sync *live;
	gboolean live_failing, route_lost, route_probe;
	gint64 recovered_at;
	/* Monotonic time the long poll started failing, or 0. */
	gint64 live_failing_since;
	gint64 displayed_revision;
	guint sync_failures;
	/* Retry deadlines use monotonic microseconds; renewal uses Unix seconds. */
	gint64 refresh_retry, save_retry, armed_renewal;
	guint refresh_failures;
	J *group_call_members;
	gint64 group_roster_due;
	/* Chat IDs map to borrowed chat_snapshot rows, or NULL before first sync. */
	GHashTable *chats;
	GHashTable *leases, *left_groups;
	J *messages, *friends, *opened_chats, *lookup_result, *invited_groups;
	J *recommendations;
	/* OpenChat (Square) session state. */
	J *squares, *square_members, *square_authorities,
		*square_chat_to_square, *square_chats_raw;
	GHashTable *square_history_tokens;
	char *square_find_token, *square_sync_token, *square_read_sync;
	gint64 square_sub_id, square_poll_due;
	guint square_poll_failures;
	gboolean square_disabled;
	/* Pending group invite link preview. */
	J *group_ticket_chat;
	char *group_ticket_id;
	/* Profiles the window opened, and incoming friend requests. */
	J *cards, *requests;
	J *unread_heads, *read_acks, *unsent;
	/* Only the open chat needs peer receipts. One lookup may be in flight. */
	J *peer_ranges, *peer_notices;
	gboolean need_ranges, publish_reads, ranges_pending;
	gint64 ranges_retry_at;
	J *announcements, *chat_snapshot, *search_chats;
	char *global_query, *global_token;
	size_t global_offset;
	char *announcement_chat;
	J *find_boundary;
	char *find_chat, *find_query, *find_token;
	char *lookup_token, *lookup_query;
	const char *lookup_kind;
	gint64 lookup_until;
	/* Friend searches and adds, paced so they never look automated. */
	struct line_pace lookup_pace, add_pace;
	char *name, *history_chat, *history_first;
	/*
	 * Recently opened chats, newest last, as in Discord: reopening one
	 * answers from memory and revalidates with LINE right after.
	 */
	GHashTable *history_cache;
	GQueue history_lru;
	char *revalidate;
	GThread *image_thread;
	GAsyncQueue *image_requests, *image_results;
	guint image_count;
	int image_wake;
	guint generation;
	struct avernella_worker_proc vault_broker;
	int vault_fd;
	gint64 vault_retry_at;
	gboolean vault_has_keys;
};

/* One JSON line to the UI, with a lease descriptor when fd >= 0. */
static void emit_line(J *packet, int fd)
{
	static GMutex output_lock;
	size_t length;
	const char *text = json_object_to_json_string_length(
		packet, JSON_C_TO_STRING_PLAIN, &length);

	g_mutex_lock(&output_lock);
	line_wire_send(STDOUT_FILENO, text, length, fd);
	g_mutex_unlock(&output_lock);
}

void line_emit(J *packet)
{
	emit_line(packet, -1);
}

/* A reply, with the lease's memory attached when its result is a lease. */
static void emit_reply(struct line_worker *w, J *reply)
{
	const char *lease = strfield(field(reply, "result"), "lease");
	int fd = lease ? line_media_lease_open(w->leases, lease) : -1;

	emit_line(reply, fd);
	if (fd >= 0)
		close(fd);
}

void line_event(const char *name, const char *key, const char *value)
{
	g_autoptr(J) event = obj();
	json_object_object_add(event, "event", json_object_new_string(name));
	if (key)
		json_object_object_add(event, key,
				       json_object_new_string(value));
	line_emit(event);
}

static void wake_after(struct line_worker *w, int seconds)
{
	struct itimerspec timer = { .it_value.tv_sec = seconds };

	if (w->timer >= 0)
		timerfd_settime(w->timer, 0, &timer, NULL);
}

static void cache_clear(struct line_worker *w);
static void open_archive(struct line_worker *w);
static void reconcile_page(struct line_worker *w, const char *chat, J *raw);
static J *send_entry(J *intent, const char *status, const char *fingerprint);

static J *problem(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return NULL;
}

static void clear_lookup(struct line_worker *w)
{
	g_clear_pointer(&w->lookup_result, json_object_put);
	g_clear_pointer(&w->lookup_token, g_free);
	g_clear_pointer(&w->lookup_query, g_free);
	w->lookup_kind = NULL;
	w->lookup_until = 0;
}

static void clear_message_search(struct line_worker *w)
{
	g_clear_pointer(&w->find_boundary, json_object_put);
	g_clear_pointer(&w->find_chat, g_free);
	g_clear_pointer(&w->find_query, g_free);
	g_clear_pointer(&w->find_token, g_free);
}

static void clear_global_search(struct line_worker *w)
{
	g_clear_pointer(&w->search_chats, json_object_put);
	g_clear_pointer(&w->global_query, g_free);
	g_clear_pointer(&w->global_token, g_free);
	w->global_offset = 0;
}

static void clear_view(struct line_worker *w)
{
	line_voice_close();
	g_clear_pointer(&w->group_call_members, json_object_put);
	w->group_roster_due = 0;
	line_shared_clear(&w->session);
	g_clear_pointer(&w->stickers, line_stickers_free);
	g_clear_pointer(&w->history_chat, g_free);
	g_clear_pointer(&w->history_first, g_free);
	g_clear_pointer(&w->messages, json_object_put);
	g_clear_pointer(&w->announcements, json_object_put);
	g_clear_pointer(&w->announcement_chat, g_free);
	clear_lookup(w);
	clear_message_search(w);
	clear_global_search(w);
	g_clear_pointer(&w->chat_snapshot, json_object_put);
	g_clear_pointer(&w->friends, json_object_put);
	g_clear_pointer(&w->recommendations, json_object_put);
	g_clear_pointer(&w->cards, json_object_put);
	g_clear_pointer(&w->requests, json_object_put);
	g_clear_pointer(&w->opened_chats, json_object_put);
	g_clear_pointer(&w->invited_groups, json_object_put);
	g_clear_pointer(&w->squares, json_object_put);
	g_clear_pointer(&w->square_members, json_object_put);
	g_clear_pointer(&w->square_authorities, json_object_put);
	g_clear_pointer(&w->square_chat_to_square, json_object_put);
	g_clear_pointer(&w->square_chats_raw, json_object_put);
	if (w->square_history_tokens)
		g_hash_table_remove_all(w->square_history_tokens);
	g_clear_pointer(&w->square_find_token, g_free);
	g_clear_pointer(&w->square_sync_token, g_free);
	g_clear_pointer(&w->square_read_sync, g_free);
	w->square_sub_id = 0;
	w->square_poll_due = 0;
	w->square_poll_failures = 0;
	w->square_disabled = FALSE;
	g_clear_pointer(&w->group_ticket_chat, json_object_put);
	g_clear_pointer(&w->group_ticket_id, g_free);
	g_clear_pointer(&w->unread_heads, json_object_put);
	g_clear_pointer(&w->read_acks, json_object_put);
	g_clear_pointer(&w->unsent, json_object_put);
	g_clear_pointer(&w->outbox, json_object_put);
	g_clear_pointer(&w->archive, line_offline_close);
	g_hash_table_remove_all(w->checked_sends);
	w->offline = w->verify_on_reconnect = w->online_chats =
		w->archived_page = FALSE;
	w->outbox_due = 0;
	g_clear_pointer(&w->peer_ranges, json_object_put);
	g_clear_pointer(&w->peer_notices, json_object_put);
	w->need_ranges = w->publish_reads = FALSE;
	w->latest_history = FALSE;
	g_hash_table_remove_all(w->leases);
	line_talk_sync_clear(&w->sync);
	cache_clear(w);
	g_clear_pointer(&w->live, line_live_sync_stop);
	w->live_failing = w->route_lost = w->route_probe = FALSE;
	w->recovered_at = 0;
	w->live_failing_since = 0;
	w->sync_failures = 0;
	w->refresh_retry = w->save_retry = 0;
	w->refresh_failures = 0;
	w->generation++;
	w->ready = w->foreground = FALSE;
	w->displayed_revision = -1;
	wake_after(w, 0);
	g_clear_pointer(&w->name, g_free);
	g_hash_table_remove_all(w->chats);
	g_hash_table_remove_all(w->left_groups);
}

static void drop_vault(struct line_worker *w)
{
	if (w->vault_fd >= 0)
		close(w->vault_fd);
	w->vault_fd = -1;
}

static gboolean ensure_vault(struct line_worker *w, GError **error)
{
	if (w->vault_fd >= 0)
		return TRUE;
	gint64 now = g_get_monotonic_time();

	if (w->vault_broker.ctrl_fd >= 0 && now >= w->vault_retry_at) {
		int fd = avernella_vault_reconnect(w->vault_broker.ctrl_fd);

		w->vault_retry_at = g_get_monotonic_time() + G_USEC_PER_SEC;
		if (fd >= 0) {
			if (!avernella_vault_client_ping(fd) &&
			    avernella_vault_client_restore(
				    fd, &w->session,
				    line_store_session_fd(w->dir), error)) {
				w->vault_fd = fd;
				w->vault_retry_at = 0;
				return TRUE;
			}
			close(fd);
			if (error && *error)
				return FALSE;
		}
	}
	g_set_error_literal(error, LINE_ERROR, 1, "Key Vault is unavailable.");
	return FALSE;
}

/* Retry only a local operation. The network request has not been sent. */
static gboolean retry_vault(struct line_worker *w, GError **error)
{
	if (!avernella_vault_disconnected(*error))
		return FALSE;
	drop_vault(w);
	g_clear_error(error);
	return ensure_vault(w, error);
}

static gboolean worker_vault_ecdh(void *ctx, gint32 key_id,
				  const guint8 peer_pub[32],
				  guint8 shared_out[32], GError **error)
{
	struct line_worker *w = ctx;

	g_autoptr(GError) failure = NULL;

	if (!ensure_vault(w, &failure))
		goto fail;
	if (avernella_vault_client_ecdh(w->vault_fd, key_id, peer_pub,
					shared_out, &failure) ||
	    (retry_vault(w, &failure) &&
	     avernella_vault_client_ecdh(w->vault_fd, key_id, peer_pub,
					 shared_out, &failure)))
		return TRUE;
fail:
	if (avernella_vault_disconnected(failure))
		drop_vault(w);
	g_propagate_error(error, g_steal_pointer(&failure));
	return FALSE;
}

static gboolean worker_vault_pub(void *ctx, gint32 key_id, guint8 pub_out[32],
				 GError **error)
{
	struct line_worker *w = ctx;

	g_autoptr(GError) failure = NULL;

	if (!ensure_vault(w, &failure))
		goto fail;
	if (avernella_vault_client_own_public(w->vault_fd, key_id, pub_out,
					      &failure) ||
	    (retry_vault(w, &failure) &&
	     avernella_vault_client_own_public(w->vault_fd, key_id, pub_out,
					       &failure)))
		return TRUE;
fail:
	if (avernella_vault_disconnected(failure))
		drop_vault(w);
	g_propagate_error(error, g_steal_pointer(&failure));
	return FALSE;
}

static const struct line_sealing_vault_ops worker_vault_ops = {
	.ecdh = worker_vault_ecdh,
	.pub = worker_vault_pub,
};

static void scrub_worker_own_keys(J *keys)
{
	J *own = field(keys, "own");

	if (!own || !json_object_is_type(own, json_type_object))
		return;
	json_object_object_foreach(own, k, v) {
		(void)k;
		if (v && json_object_is_type(v, json_type_string)) {
			size_t len = (size_t)json_object_get_string_len(v);
			char *raw = (char *)json_object_get_string(v);

			if (raw && len)
				explicit_bzero(raw, len);
		}
	}
	json_object_object_del(keys, "own");
}

static void sync_vault_session(struct line_worker *w)
{
	if (w->vault_has_keys || !w->session.access || !w->session.mid ||
	    w->dir < 0 || !ensure_vault(w, NULL))
		return;
	w->vault_has_keys = TRUE;
	line_sealing_set_vault(&worker_vault_ops, w);
	scrub_worker_own_keys(w->session.keys);
}

static void purge_archive(struct line_worker *w, int dir)
{
	g_autoptr(GError) error = NULL;

	w->archive_cleanup_failed = !line_offline_clear_all(dir, &error);
	if (error)
		g_printerr("Avernella: %s\n", error->message);
}

static gboolean forget(struct line_worker *w, GError **error)
{
	/* Wait for the old Vault before unlinking its session file. */
	drop_vault(w);
	int rc = w->vault_has_keys ?
			 avernella_vault_disconnect(w->vault_broker.ctrl_fd) :
			 0;

	if (rc < 0 && rc != -EPIPE && rc != -ECONNRESET) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Could not close the Key Vault session.");
		return FALSE;
	}
	purge_archive(w, w->dir);
	clear_view(w);
	w->vault_has_keys = FALSE;
	w->vault_retry_at = 0;
	line_sealing_set_vault(NULL, NULL);
	line_session_clear(&w->session);
	return line_store_forget(w->dir, error);
}

static gboolean session_rejected(Line *s, const GError *error)
{
	return s->denied ||
	       g_error_matches(error, LINE_ERROR, LINE_SESSION_EXPIRED) ||
	       line_auth_rejected(s->service_code);
}

/*
 * Whether a saved session's failed check is worth repeating on its own:
 * only failures a network can cause. Any answer LINE itself gave (a
 * TalkException, an HTTP 4xx such as 401 or 403) stops the retries; the
 * user can choose sign in again, but the app never keeps knocking.
 */
static gboolean transient_failure(const Line *s, const GError *error)
{
	long status = s->http_status;

	if (s->service_code >= 0 ||
	    g_error_matches(error, LINE_ERROR, LINE_ACCESS_DENIED))
		return FALSE;
	return !status || status == 408 || status == 429 || status >= 500;
}

static void went_offline(struct line_worker *w)
{
	if (w->offline)
		return;
	w->offline = TRUE;
	w->online_chats = FALSE;
	line_event("offline", NULL, NULL);
	wake_after(w, 1);
}

static void open_archive(struct line_worker *w)
{
	guint8 key[LINE_STATE_KEY_SIZE];

	if (w->archive)
		return;
	if (line_store_offline_key(w->session.mid, key))
		w->archive = line_offline_open(w->dir, w->session.mid, key);
	explicit_bzero(key, sizeof(key));
}

static void archive_profile(struct line_worker *w, const char *name,
			    const char *picture)
{
	if (!w->archive)
		return;
	g_autoptr(J) profile = obj();

	json_object_object_add(profile, "name", json_object_new_string(name));
	if (picture)
		json_object_object_add(profile, "picture",
				       json_object_new_string(picture));
	line_offline_save_profile(w->archive, profile);
}

static gboolean valid_outbox(J *rows)
{
	if (!rows || json_object_array_length(rows) > 32)
		return FALSE;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		const char *chat = strfield(row, "chat"),
			   *id = strfield(row, "intentId"),
			   *status = strfield(row, "status"),
			   *fingerprint = strfield(row, "fingerprint");

		if (!chat || !*chat || line_is_square_chat(chat) || !id ||
		    !g_uuid_string_is_valid(id) ||
		    !strfield(field(row, "params"), "text"))
			return FALSE;
		if (!g_strcmp0(status, "queued") && !fingerprint)
			continue;
		if (g_strcmp0(status, "uncertain") || !fingerprint ||
		    strlen(fingerprint) != 64 ||
		    fingerprint[strspn(fingerprint, "0123456789abcdef")])
			return FALSE;
	}
	return TRUE;
}

/*
 * getProfile, the check LINE's own client uses to confirm a session. As its
 * HTTP clients do, a 401 or 403 renews the token once and asks once more.
 */
static J *checked_profile(Line *s, GError **error)
{
	J *profile = line_rpc(s, "/S4", "getProfile", NULL, error);

	if (profile ||
	    !g_error_matches(*error, LINE_ERROR, LINE_ACCESS_DENIED) ||
	    !s->refresh || !*s->refresh)
		return profile;
	g_autoptr(GError) renewal = NULL;

	if (line_refresh_session(s, &renewal)) {
		g_clear_error(error);
		return line_rpc(s, "/S4", "getProfile", NULL, error);
	}
	if (s->denied) {
		g_clear_error(error);
		g_propagate_error(error, g_steal_pointer(&renewal));
	}
	return NULL;
}

/* LINE refused the session: forget it and return the window to sign-in. */
static void end_session(struct line_worker *w)
{
	g_autoptr(GError) removal = NULL;

	forget(w, &removal);
	line_event(
		"signed_out", "message",
		removal ?
			removal->message :
		w->archive_cleanup_failed ?
			"LINE ended the session. The local chat archive could not be removed." :
			"LINE ended this desktop session. Choose sign in to pair again.");
}

/*
 * Sign-in refusals that retrying can never fix, explained in the words of
 * LINE's own help. TalkException numbers from LINE 26.x (iu8.n9).
 */
static const char *sign_in_refusal(int code)
{
	switch (code) {
	case 44: /* NOT_ALLOWED_SECONDARY_DEVICE */
		return "LINE doesn't allow sign-in on other devices for this account. Turn on Allow login in LINE on your phone (Settings, Account), then try again.";
	case 16: /* NO_AVAILABLE_VERIFICATION_METHOD */
		return "LINE has no way to confirm this sign-in. Sign in with a QR code instead.";
	case 22: /* DEACTIVATED_ACCOUNT_BOUND_TO_THIS_IDENTITY */
		return "The LINE account for this email address has been deactivated.";
	case 11: /* INCOMPATIBLE_APP_VERSION */
	case 50: /* MUST_UPGRADE */
	case 124: /* INCOMPATIBLE_APP_TYPE */
		return "LINE no longer accepts sign-in from this app version.";
	default:
		return NULL;
	}
}

static gboolean pair_worker(struct line_worker *w, GError **error)
{
	if (w->credentials_fd < 0)
		return line_pair(&w->session, error);
	struct line_credentials creds;
	ssize_t n;

	do {
		n = read(w->credentials_fd, &creds, sizeof(creds));
	} while (n < 0 && errno == EINTR);
	close(w->credentials_fd);
	w->credentials_fd = -1;
	gboolean ok = FALSE;

	if (n == sizeof(creds) && memchr(creds.email, 0, sizeof(creds.email)) &&
	    memchr(creds.password, 0, sizeof(creds.password)))
		ok = line_password_login(&w->session, creds.email,
					 creds.password, error);
	else
		problem(error, "Could not read the sign-in credentials.");
	explicit_bzero(&creds, sizeof(creds));
	return ok;
}

static J *login(struct line_worker *w, GError **error)
{
	Line *s = &w->session;

	if (w->dir < 0) {
		w->dir = line_store_open(s, error);
		if (w->dir < 0)
			return NULL;
		line_cooldown_use_store(w->dir);
	}
	if (!w->ready) {
		if (!s->access) {
			if (!pair_worker(w, error)) {
				const char *why =
					sign_in_refusal(s->service_code);

				if (why && error && *error) {
					g_clear_error(error);
					problem(error, why);
				}
				return NULL;
			}
			if (!line_store_save(w->dir, s, error)) {
				/*
				 * The phone has already approved a live session. End it
				 * before returning the local storage failure to the UI.
				 */
				g_autoptr(GError) revoke_error = NULL,
						  remove_error = NULL;
				g_autoptr(J) ack = line_rpc(s, "/AS4",
							    "logoutV2", NULL,
							    &revoke_error);
				if (!ack && !session_rejected(s, revoke_error))
					g_prefix_error(
						error,
						"Check linked devices on your phone. Sign-out could not be confirmed. ");
				if (!forget(w, &remove_error))
					g_prefix_error(error, "%s ",
						       remove_error->message);
				return NULL;
			}
		}
		if (w->credentials_fd >= 0) {
			close(w->credentials_fd);
			w->credentials_fd = -1;
		}
		open_archive(w);
		g_autoptr(J) saved = NULL;
		g_autoptr(J) profile = checked_profile(s, error);

		if (!profile) {
			if (session_rejected(s, *error)) {
				g_clear_error(error);
				if (!forget(w, error))
					return NULL;
				return problem(
					error,
					"LINE ended the previous session. Choose sign in to pair again.");
			}
			/*
			 * A network failure leaves the saved session valid as far as we
			 * know: keep it and let the UI retry. LINE's own refusals keep
			 * it too, but end the attempt so nothing retries by itself.
			 */
			if (*error && transient_failure(s, *error)) {
				if (w->archive && s->mid)
					saved = line_offline_load_profile(
						w->archive);
				if (strfield(saved, "name")) {
					g_clear_error(error);
					w->offline = w->verify_on_reconnect =
						TRUE;
				} else {
					(*error)->code = LINE_RETRY;
					return NULL;
				}
			} else if (*error) {
				g_prefix_error(
					error,
					"LINE refused this desktop session. Choose sign in to try again. ");
				return NULL;
			}
		}
		const char *mid = saved ? s->mid : strfield(profile, "1"),
			   *name = saved ? strfield(saved, "name") :
					   strfield(profile, "20");
		if (!mid || !*mid || !name || !g_utf8_validate(name, -1, NULL))
			return problem(error,
				       "LINE returned an incomplete profile.");
		if (s->mid && strcmp(s->mid, mid)) {
			if (!forget(w, error))
				return NULL;
			return problem(
				error,
				"The account changed. Pair this computer again.");
		}
		if (!s->mid) {
			s->mid = g_strdup(mid);
			s->token_changed = TRUE;
		}
		if (!s->pictures)
			s->pictures = obj();
		const char *picture = saved ? strfield(saved, "picture") :
					      strfield(profile, "33");

		if (picture)
			json_object_object_add(s->pictures, mid,
					       json_object_new_string(picture));
		w->name = g_strdup(name);
		w->friends = obj();
		w->opened_chats = obj();
		w->read_acks = obj();
		w->unsent = line_store_read_unsent(w->dir, s->mid);
		open_archive(w);
		if (!saved)
			archive_profile(w, name, picture);
		w->outbox = w->archive ?
				    line_offline_load_outbox(w->archive, NULL) :
				    json_object_new_array();
		if (!valid_outbox(w->outbox))
			g_clear_pointer(&w->outbox, json_object_put);
		w->ready = TRUE;
		/*
		 * Move Letter Sealing private keys into the memfd_secret() Key
		 * Vault process and scrub them from the worker heap.
		 */
		sync_vault_session(w);
		/*
		 * Start with one baseline revision. Afterwards only a changed
		 * revision refreshes visible chats. Back off when disconnected.
		 */
		wake_after(w, 1);
	}
	J *who = obj();

	json_object_object_add(who, "name", json_object_new_string(w->name));
	json_object_object_add(who, "selfId", json_object_new_string(s->mid));
	if (w->offline)
		json_object_object_add(who, "offline",
				       json_object_new_boolean(TRUE));
	if (w->outbox && json_object_array_length(w->outbox)) {
		J *list = json_object_new_array();

		for (size_t i = 0; i < json_object_array_length(w->outbox);
		     i++) {
			J *entry = json_object_array_get_idx(w->outbox, i);

			json_object_array_add(
				list,
				send_entry(entry, strfield(entry, "status"),
					   NULL));
		}
		json_object_object_add(who, "outbox", list);
	}
	if (line_store_temporary())
		json_object_object_add(who, "temporarySession",
				       json_object_new_boolean(TRUE));
	J *caps = obj();
	const char *const enabled[] = {
		"photos",	"files",	 "calls",	 "avatars",
		"muteCalls",	"incomingCalls", "emojis",	 "friends",
		"friendInvite", "groups",	 "findMessages", "stickers",
		"openchats"
	};
	for (guint i = 0; i < G_N_ELEMENTS(enabled); i++)
		json_object_object_add(caps, enabled[i],
				       json_object_new_boolean(TRUE));
	json_object_object_add(caps, "videoCalls",
			       json_object_new_boolean(line_video_enabled()));
	json_object_object_add(
		caps, "groupVoiceCalls",
		json_object_new_boolean(line_group_voice_enabled()));
	json_object_object_add(
		caps, "groupVideoCalls",
		json_object_new_boolean(line_group_video_enabled()));
	json_object_object_add(who, "capabilities", caps);
	return who;
}

/*
 * Reserve on disk before issuing a mutating RPC. A timeout may mean the
 * service already accepted the request, so never reuse its sequence.
 */
static gint32 reserve_sequence(struct line_worker *w, GError **error)
{
	/* A startup failure might have cleared since the last attempt. */
	sync_vault_session(w);
	if (!w->vault_has_keys)
		return line_store_sequence(w->dir, &w->session, error);
	g_autoptr(GError) failure = NULL;
	gint32 seq = 0;

	if (ensure_vault(w, &failure)) {
		seq = avernella_vault_client_reserve_seq(w->vault_fd, &failure);
		if (seq <= 0 && retry_vault(w, &failure))
			seq = avernella_vault_client_reserve_seq(w->vault_fd,
								 &failure);
	}
	if (seq > 0) {
		w->session.seq = seq;
		return seq;
	}
	if (avernella_vault_disconnected(failure))
		drop_vault(w);
	if (failure)
		g_propagate_error(error, g_steal_pointer(&failure));
	return 0;
}

static void remember_friend(struct line_worker *w, J *row)
{
	const char *id = strfield(row, "id");

	if (!id)
		return;
	if (json_object_get_boolean(field(row, "friend")) &&
	    !json_object_get_boolean(field(row, "self")))
		json_object_object_add(w->friends, id, json_object_get(row));
	else
		json_object_object_del(w->friends, id);
}

static gboolean visible_friend(J *row)
{
	return row && !json_object_get_boolean(field(row, "blocked")) &&
	       !json_object_get_boolean(field(row, "hidden"));
}

static J *open_conversation(struct line_worker *w, J *row)
{
	const char *id = strfield(row, "id");
	J *chat = obj();

	json_object_object_add(chat, "id", json_object_new_string(id));
	const char *name = strfield(row, "name");

	json_object_object_add(chat, "name",
			       json_object_new_string(name ? name : id));
	const char *avatar = strfield(row, "avatarKey");

	json_object_object_add(chat, "avatarKey",
			       json_object_new_string(avatar ? avatar : ""));
	json_object_object_add(chat, "time",
			       json_object_new_int64(json_object_get_int64(
				       field(row, "time"))));
	if (json_object_get_boolean(field(row, "square"))) {
		json_object_object_add(chat, "square",
				       json_object_new_boolean(TRUE));
		if (strfield(row, "squareMid"))
			json_object_object_add(chat, "squareMid",
					       json_object_new_string(strfield(
						       row, "squareMid")));
		if (strfield(row, "myMemberId"))
			json_object_object_add(chat, "myMemberId",
					       json_object_new_string(strfield(
						       row, "myMemberId")));
		if (strfield(row, "myRole"))
			json_object_object_add(chat, "myRole",
					       json_object_new_string(strfield(
						       row, "myRole")));
	}
	if (!w->opened_chats)
		w->opened_chats = obj();
	if (!g_hash_table_contains(w->chats, id)) {
		json_object_object_add(w->opened_chats, id,
				       json_object_get(chat));
		g_hash_table_insert(w->chats, g_strdup(id), NULL);
	}
	return chat;
}

/*
 * Refuses an action that would come too soon after the previous ones. The
 * message says how long to wait, so the user is never left guessing.
 */
static gboolean paced(struct line_pace *pace, const struct line_pace_rule *rule,
		      GError **error)
{
	gint64 wait = line_pace_wait(pace, rule, g_get_monotonic_time());

	if (!wait)
		return TRUE;
	if (wait < 60)
		g_set_error(
			error, LINE_ERROR, LINE_RATE_LIMITED,
			"To keep your account safe, wait %lld seconds before trying again.",
			(long long)wait);
	else
		g_set_error(
			error, LINE_ERROR, LINE_RATE_LIMITED,
			"To keep your account safe, wait %lld minutes before trying again.",
			(long long)((wait + 59) / 60));
	return FALSE;
}

/* Counts an attempt that reached LINE, and pauses if LINE said too many. */
static void count_attempt(struct line_pace *pace,
			  const struct line_pace_rule *rule, GError *error)
{
	line_pace_record(pace, rule, g_get_monotonic_time(),
			 g_error_matches(error, LINE_ERROR, LINE_RATE_LIMITED));
}

static J *find_friend(struct line_worker *w, const char *query, GError **error)
{
	clear_lookup(w);
	if (!query || !*query || strlen(query) > 2048)
		return problem(error, "Enter a LINE ID or personal QR link.");
	g_autofree char *value = g_strdup(query);

	g_strstrip(value);
	const char *kind = strstr(value, "://") ? "link" :
			   value[0] == '+'	? "phone" :
						  "id";
	if (!paced(&w->lookup_pace, &line_lookup_pace, error))
		return NULL;
	J *row = line_friend_lookup(&w->session, kind, value, error);

	count_attempt(&w->lookup_pace, &line_lookup_pace, row ? NULL : *error);

	if (!row)
		return NULL;
	remember_friend(w, row);
	w->lookup_kind = kind;
	w->lookup_query = g_steal_pointer(&value);
	w->lookup_result = json_object_get(row);
	w->lookup_token = g_uuid_string_random();
	w->lookup_until = g_get_monotonic_time() + 5 * 60 * G_USEC_PER_SEC;
	json_object_object_add(row, "lookup",
			       json_object_new_string(w->lookup_token));
	return row;
}

static J *add_friend(struct line_worker *w, const char *token, GError **error)
{
	if (!token || g_strcmp0(w->lookup_token, token) ||
	    g_get_monotonic_time() >= w->lookup_until)
		return problem(
			error,
			"Search for this person again before adding them.");
	if (!json_object_get_boolean(field(w->lookup_result, "canAdd")))
		return problem(error, "This profile cannot be added.");
	if (!paced(&w->add_pace, &line_add_pace, error))
		return NULL;
	gint32 seq = reserve_sequence(w, error);

	if (!seq)
		return NULL;
	J *row = line_friend_add(&w->session, w->lookup_result, w->lookup_kind,
				 w->lookup_query, seq, error);

	count_attempt(&w->add_pace, &line_add_pace, row ? NULL : *error);
	clear_lookup(w);
	if (row)
		remember_friend(w, row);
	else
		g_prefix_error(error, "Search again to check the friendship. ");
	return row;
}

static J *change_group(struct line_worker *w, const char *chat, J *params,
		       GError **error)
{
	J *users = field(params, "users");

	if (!json_object_is_type(users, json_type_array))
		return problem(error, "Choose friends for the group.");
	for (size_t i = 0; i < json_object_array_length(users); i++) {
		J *id = json_object_array_get_idx(users, i);

		if (!json_object_is_type(id, json_type_string) ||
		    !visible_friend(
			    field(w->friends, json_object_get_string(id))))
			return problem(error,
				       "Select people from your friend list.");
	}
	gint32 seq = reserve_sequence(w, error);

	if (!seq)
		return NULL;
	w->session.seq--;
	g_autoptr(J) row =
		chat ? line_group_invite(&w->session, chat, users, error) :
		       line_group_create(&w->session, strfield(params, "name"),
					 users, error);
	w->session.seq = seq;
	if (!row)
		return NULL;
	return chat ? g_steal_pointer(&row) : open_conversation(w, row);
}

static char *search_text(const char *text)
{
	if (!text || !g_utf8_validate(text, -1, NULL))
		return NULL;
	g_autofree char *folded = g_utf8_casefold(text, -1);

	return g_utf8_normalize(folded, -1, G_NORMALIZE_DEFAULT_COMPOSE);
}

static void append_matches(J *matches, J *rows, const char *needle, J *chat)
{
	for (size_t i = json_object_array_length(rows); i > 0; i--) {
		J *row = json_object_array_get_idx(rows, i - 1);

		if (json_object_get_boolean(field(row, "unreadable")))
			continue;
		const char *text = !g_strcmp0(strfield(row, "type"), "text") ?
					   strfield(row, "text") :
					   strfield(row, "filename");
		g_autofree char *haystack = search_text(text);

		if (!haystack || !strstr(haystack, needle))
			continue;
		if (chat)
			json_object_object_add(row, "conversation",
					       json_object_get(chat));
		json_object_array_add(matches, json_object_get(row));
	}
}

static guint64 history_clear_id(struct line_worker *w, const char *chat)
{
	J *summary = g_hash_table_lookup(w->chats, chat);

	return json_object_get_uint64(field(summary, "clearThrough"));
}

static const char *square_for_chat(struct line_worker *w, const char *chat)
{
	return w->square_chat_to_square ?
		       strfield(w->square_chat_to_square, chat) :
		       NULL;
}

static J *my_square_member(struct line_worker *w, const char *square_mid)
{
	return square_mid && w->square_members ?
		       field(w->square_members, square_mid) :
		       NULL;
}

static const char *my_square_member_id(struct line_worker *w, const char *chat)
{
	J *m = my_square_member(w, square_for_chat(w, chat));

	return m ? strfield(m, "1") : NULL;
}

static gboolean my_square_can_announce(struct line_worker *w, const char *chat)
{
	const char *sq = square_for_chat(w, chat);
	J *m = my_square_member(w, sq);
	J *auth = sq && w->square_authorities ?
			  field(w->square_authorities, sq) :
			  NULL;
	int role = m ? json_object_get_int(field(m, "8")) : 0;
	int need = auth ? json_object_get_int(field(auth, "12")) : 2;

	return role > 0 && need > 0 && role <= need;
}

static void remember_square_raw(struct line_worker *w, J *result)
{
	J *sq = field(result, "rawSquare");
	J *mem = field(result, "rawMember");
	J *auth = field(result, "rawAuthority");
	J *chat = field(result, "rawChat");
	const char *smid = sq  ? strfield(sq, "1") :
			   mem ? strfield(mem, "2") :
				 NULL;

	if (smid && line_square_id(smid, 's')) {
		if (!w->squares)
			w->squares = obj();
		if (!w->square_members)
			w->square_members = obj();
		if (!w->square_authorities)
			w->square_authorities = obj();
		if (sq)
			json_object_object_add(w->squares, smid,
					       json_object_get(sq));
		if (mem)
			json_object_object_add(w->square_members, smid,
					       json_object_get(mem));
		if (auth)
			json_object_object_add(w->square_authorities, smid,
					       json_object_get(auth));
	}
	if (chat && line_square_id(strfield(chat, "1"), 'm')) {
		if (!w->square_chats_raw)
			w->square_chats_raw = obj();
		json_object_object_add(w->square_chats_raw, strfield(chat, "1"),
				       json_object_get(chat));
		if (smid && line_square_id(smid, 's')) {
			if (!w->square_chat_to_square)
				w->square_chat_to_square = obj();
			json_object_object_add(w->square_chat_to_square,
					       strfield(chat, "1"),
					       json_object_new_string(smid));
		}
	}
	json_object_object_del(result, "rawSquare");
	json_object_object_del(result, "rawMember");
	json_object_object_del(result, "rawAuthority");
	json_object_object_del(result, "rawChat");
}

static J *recent_history(struct line_worker *w, const char *chat, J **raw,
			 GError **error)
{
	if (line_is_square_chat(chat)) {
		g_autofree char *next = NULL;
		gboolean more = FALSE;
		J *rows = line_square_history(&w->session, chat,
					      my_square_member_id(w, chat),
					      NULL, raw, &next, &more, error);

		if (rows) {
			if (next)
				g_hash_table_replace(w->square_history_tokens,
						     g_strdup(chat),
						     g_steal_pointer(&next));
			else
				g_hash_table_remove(w->square_history_tokens,
						    chat);
		}
		return rows;
	}
	J *rows = line_history_with_summary(&w->session, chat,
					    g_hash_table_lookup(w->chats, chat),
					    raw, error);

	if (rows)
		line_redact_unsent(field(w->unsent, chat), rows,
				   raw ? *raw : NULL, history_clear_id(w, chat),
				   TRUE);
	return rows;
}

static J *previous_history(struct line_worker *w, const char *chat, J *boundary,
			   J **raw, gboolean *more, GError **error)
{
	if (line_is_square_chat(chat)) {
		const char *token =
			g_hash_table_lookup(w->square_history_tokens, chat);

		if (!token || !*token) {
			if (more)
				*more = FALSE;
			if (raw)
				*raw = obj();
			return json_object_new_array();
		}
		g_autofree char *next = NULL;
		J *rows = line_square_history(&w->session, chat,
					      my_square_member_id(w, chat),
					      token, raw, &next, more, error);

		if (rows) {
			if (next)
				g_hash_table_replace(w->square_history_tokens,
						     g_strdup(chat),
						     g_steal_pointer(&next));
			else
				g_hash_table_remove(w->square_history_tokens,
						    chat);
		}
		return rows;
	}
	guint64 cleared = history_clear_id(w, chat);
	J *rows = line_history_older_since(&w->session, chat, boundary, cleared,
					   raw, more, error);

	if (rows)
		line_redact_unsent(field(w->unsent, chat), rows,
				   raw ? *raw : NULL, cleared, FALSE);
	return rows;
}

static J *find_messages(struct line_worker *w, const char *chat, J *params,
			GError **error)
{
	const char *query = strfield(params, "query"),
		   *token = strfield(params, "cursor");
	if (!query || strlen(query) > 1024)
		return problem(error,
			       "Enter a search term of up to 1,024 bytes.");
	g_autofree char *needle = search_text(query);

	if (!needle || !*g_strstrip(needle))
		return problem(error, "Enter some text to find in this chat.");
	if (token &&
	    (g_strcmp0(token, w->find_token) || g_strcmp0(chat, w->find_chat) ||
	     g_strcmp0(needle, w->find_query)))
		return problem(error, "Start this conversation search again.");
	if (!token)
		clear_message_search(w);
	g_autoptr(J) raw = NULL;
	gboolean more = FALSE;

	g_autoptr(J) rows = token ? previous_history(w, chat, w->find_boundary,
						     &raw, &more, error) :
				    recent_history(w, chat, &raw, error);
	if (!rows)
		return NULL;
	size_t count = json_object_array_length(rows);

	if (!token)
		more = count >= 50;
	J *page = obj(), *matches = json_object_new_array();

	json_object_object_add(page, "matches", matches);
	append_matches(matches, rows, needle, NULL);
	clear_message_search(w);
	if (more)
		w->find_boundary = line_history_cursor(raw);
	if (w->find_boundary) {
		w->find_chat = g_strdup(chat);
		w->find_query = g_steal_pointer(&needle);
		w->find_token = g_uuid_string_random();
		json_object_object_add(page, "next",
				       json_object_new_string(w->find_token));
	}
	return page;
}

enum {
	HISTORY_CACHE_CHATS = 24
};

struct cached_history {
	J *rows, *raw;
	guint64 cleared_through;
	gboolean stale;
};

static J *find_all_messages(struct line_worker *w, J *params, GError **error)
{
	const char *query = strfield(params, "query");
	const char *token = strfield(params, "cursor");

	if (!query || strlen(query) > 1024)
		return problem(error,
			       "Enter a search term of up to 1,024 bytes.");
	g_autofree char *needle = search_text(query);

	if (!needle || !*g_strstrip(needle))
		return problem(error, "Enter some text to search for.");
	if (token) {
		if (g_strcmp0(token, w->global_token) ||
		    g_strcmp0(needle, w->global_query))
			return problem(error,
				       "Start this message search again.");
	} else {
		clear_global_search(w);
		w->search_chats = json_object_get(w->chat_snapshot);
		w->global_query = g_steal_pointer(&needle);
	}
	J *page = obj(), *matches = json_object_new_array();
	size_t count =
		w->search_chats ? json_object_array_length(w->search_chats) : 0;
	guint skipped = 0;

	json_object_object_add(page, "matches", matches);
	/* Leave room for urgent chat work between pages. */
	gint64 deadline = g_get_monotonic_time() + 350000;

	for (int i = 0; i < 6 && w->global_offset < count; i++) {
		J *chat = json_object_array_get_idx(w->search_chats,
						    w->global_offset++);
		const char *id = strfield(chat, "id");

		if (!id || !g_hash_table_contains(w->chats, id))
			continue;
		/*
		 * A chat opened recently is already in memory and current; reuse
		 * it rather than asking LINE for the same page again.
		 */
		struct cached_history *cached =
			g_hash_table_lookup(w->history_cache, id);
		g_autoptr(GError) failure = NULL;
		g_autoptr(J) rows =
			cached && !cached->stale ?
				json_object_get(cached->rows) :
				recent_history(w, id, NULL, &failure);

		if (rows)
			append_matches(matches, rows, w->global_query, chat);
		else
			skipped++;
		if (g_get_monotonic_time() >= deadline)
			break;
	}
	if (skipped)
		json_object_object_add(page, "skipped",
				       json_object_new_int(skipped));
	if (w->global_offset < count) {
		g_free(w->global_token);
		w->global_token = g_uuid_string_random();
		json_object_object_add(page, "next",
				       json_object_new_string(w->global_token));
	} else {
		clear_global_search(w);
	}
	return page;
}

static J *friend_action(struct line_worker *w, const char *method, J *params,
			GError **error)
{
	const char *id = strfield(params, "id");
	J *row = id ? field(w->friends, id) : NULL;

	if (!row)
		return problem(error, "Select a person from your friends.");
	gboolean hide = !strcmp(method, "setFriendHidden"),
		 block = !strcmp(method, "setFriendBlocked"),
		 del = !strcmp(method, "deleteFriend");
	if (!hide && !block && !del && !visible_friend(row))
		return problem(error,
			       "Choose a visible friend from your directory.");
	if (!strcmp(method, "openFriend"))
		return open_conversation(w, row);
	if (del) {
		gint32 seq = reserve_sequence(w, error);

		if (!seq)
			return NULL;
		J *ack = line_friend_delete(&w->session, row, seq, error);

		if (ack && w->friends)
			json_object_object_del(w->friends, id);
		return ack;
	}
	gboolean rename = !strcmp(method, "renameFriend");
	const char *name = strfield(params, "name");
	J *flag = field(params, hide  ? "hidden" :
				block ? "blocked" :
					"favorite");
	if ((rename && !name) ||
	    (!rename && !json_object_is_type(flag, json_type_boolean)))
		return problem(
			error,
			"Choose a friend setting or enter a display name.");
	gboolean value = json_object_get_boolean(flag);
	gint32 seq = reserve_sequence(w, error);

	if (!seq)
		return NULL;
	J *updated;

	if (hide)
		updated = line_friend_visibility(&w->session, row, value, seq,
						 error);
	else if (block)
		updated =
			line_friend_block(&w->session, row, value, seq, error);
	else if (rename)
		updated =
			line_friend_rename(&w->session, row, name, seq, error);
	else
		updated = line_friend_favorite(&w->session, row, value, seq,
					       error);
	if (updated) {
		remember_friend(w, updated);
		J *opened = field(w->opened_chats, id);

		if (opened && rename)
			json_object_object_add(opened, "name",
					       json_object_new_string(strfield(
						       updated, "name")));
	}
	return updated;
}

static int merged_chat_order(const void *a, const void *b)
{
	J *lhs = *(J *const *)a, *rhs = *(J *const *)b;
	gint64 lt = json_object_get_int64(field(lhs, "time"));
	gint64 rt = json_object_get_int64(field(rhs, "time"));

	return lt == rt ? strcmp(strfield(lhs, "id"), strfield(rhs, "id")) :
			  (lt > rt ? -1 : 1);
}

static void redact_directory(struct line_worker *w, J *rows)
{
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		const char *id = strfield(row, "id"),
			   *last = strfield(row, "lastId");

		if (!id || !last || !field(field(w->unsent, id), last))
			continue;
		json_object_object_del(row, "preview");
		json_object_object_add(row, "lastUnsent",
				       json_object_new_boolean(TRUE));
	}
}

static J *offline_directory(struct line_worker *w)
{
	if (w->chat_snapshot)
		return json_object_get(w->chat_snapshot);
	J *rows = w->archive ? line_offline_load_chats(w->archive) : NULL;

	if (!rows)
		return json_object_new_array();
	redact_directory(w, rows);
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		const char *id = strfield(row, "id");

		if (id)
			g_hash_table_insert(w->chats, g_strdup(id), row);
	}
	w->chat_snapshot = json_object_get(rows);
	return rows;
}

static void drop_chat_archive(struct line_worker *w, const char *chat)
{
	g_hash_table_remove(w->history_cache, chat);
	if (w->archive)
		line_offline_drop_history(w->archive, chat);
	for (size_t i = 0;
	     w->chat_snapshot && i < json_object_array_length(w->chat_snapshot);
	     i++) {
		J *row = json_object_array_get_idx(w->chat_snapshot, i);

		if (g_strcmp0(chat, strfield(row, "id")))
			continue;
		json_object_array_del_idx(w->chat_snapshot, i, 1);
		if (w->archive)
			line_offline_save_chats(w->archive, w->chat_snapshot);
		break;
	}
}

static J *chat_directory(struct line_worker *w, GError **error)
{
	Line *s = &w->session;
	if (w->offline)
		return offline_directory(w);
	J *rows = line_chats(s, error);

	if (!rows && *error && transient_failure(s, *error)) {
		g_clear_error(error);
		went_offline(w);
		return offline_directory(w);
	}
	if (!rows)
		return NULL;
	if (!w->square_disabled) {
		g_autoptr(GError) sq_err = NULL;
		g_autoptr(J) sqs = NULL, mems = NULL, auths = NULL, c2s = NULL;
		g_autoptr(J) sq_rows = line_square_chats(s, &sqs, &mems, &auths,
							 &c2s, &sq_err);

		if (sq_rows) {
			g_clear_pointer(&w->squares, json_object_put);
			g_clear_pointer(&w->square_members, json_object_put);
			g_clear_pointer(&w->square_authorities,
					json_object_put);
			g_clear_pointer(&w->square_chat_to_square,
					json_object_put);
			w->squares = g_steal_pointer(&sqs);
			w->square_members = g_steal_pointer(&mems);
			w->square_authorities = g_steal_pointer(&auths);
			w->square_chat_to_square = g_steal_pointer(&c2s);
			for (size_t i = 0;
			     i < json_object_array_length(sq_rows); i++)
				json_object_array_add(
					rows, json_object_get(
						      json_object_array_get_idx(
							      sq_rows, i)));
			json_object_array_sort(rows, merged_chat_order);
			if (json_object_object_length(w->squares) &&
			    !w->square_poll_due)
				w->square_poll_due = g_get_monotonic_time() +
						     1 * G_USEC_PER_SEC;
		} else if (g_error_matches(sq_err, LINE_ERROR,
					   LINE_METHOD_UNAVAILABLE)) {
			w->square_disabled = TRUE;
		}
	}
	if (w->archive) {
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			J *row = json_object_array_get_idx(rows, i);
			const char *id = strfield(row, "id");
			guint64 floor = json_object_get_uint64(
				field(row, "clearThrough"));

			if (id && floor > history_clear_id(w, id)) {
				line_offline_drop_history(w->archive, id);
				g_hash_table_remove(w->history_cache, id);
			}
		}
	}
	g_hash_table_remove_all(w->chats);
	g_clear_pointer(&w->unread_heads, json_object_put);
	w->unread_heads = obj();
	for (size_t i = 0; i < json_object_array_length(rows);) {
		J *row = json_object_array_get_idx(rows, i);
		const char *id = strfield(row, "id");

		if (id && g_hash_table_contains(w->left_groups, id)) {
			json_object_array_del_idx(rows, i, 1);
			continue;
		}
		i++;
		if (id) {
			g_hash_table_insert(w->chats, g_strdup(id), row);
			json_object_object_del(w->opened_chats, id);
			const char *last_id = strfield(row, "lastId");

			if (last_id && field(field(w->unsent, id), last_id)) {
				json_object_object_del(row, "preview");
				json_object_object_add(
					row, "lastUnsent",
					json_object_new_boolean(TRUE));
			}
			if (json_object_get_int64(field(row, "unread")) > 0 &&
			    strfield(row, "lastId"))
				json_object_object_add(
					w->unread_heads, id,
					json_object_get(field(row, "lastId")));
		}
	}
	json_object_object_foreach(w->opened_chats, id, row) {
		g_hash_table_insert(w->chats, g_strdup(id), row);
		json_object_array_add(rows, json_object_get(row));
	}
	if (w->archive && !json_object_equal(rows, w->chat_snapshot))
		line_offline_save_chats(w->archive, rows);
	json_object_put(w->chat_snapshot);
	w->chat_snapshot = json_object_get(rows);
	if (!w->online_chats) {
		w->online_chats = TRUE;
		w->outbox_due = g_get_monotonic_time();
	}
	return rows;
}

static J *friend_directory(struct line_worker *w, J *params, GError **error)
{
	Line *s = &w->session;
	const char *view = strfield(params, "view"),
		   *cursor = strfield(params, "cursor");
	J *page = view && strcmp(view, "visible") ?
			  line_managed_friends_page(s, view, cursor, error) :
			  line_friends_page(s, cursor, error);
	if (!page)
		return NULL;
	J *rows = field(page, "friends");

	gboolean recommendations = !g_strcmp0(view, "recommendations");

	if (recommendations && (!cursor || !w->recommendations)) {
		json_object_put(w->recommendations);
		w->recommendations = obj();
	}
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		const char *id = strfield(row, "id");

		if (recommendations) {
			if (id && json_object_object_length(
					  w->recommendations) < 4096)
				json_object_object_add(
					w->recommendations, id,
					json_object_new_boolean(TRUE));
		} else {
			remember_friend(w, row);
		}
	}
	return page;
}

static J *add_recommendation(struct line_worker *w, const char *id,
			     GError **error)
{
	if (!id || !field(w->recommendations, id))
		return problem(error,
			       "Choose a person from the recommendations.");
	if (!paced(&w->add_pace, &line_add_pace, error))
		return NULL;
	gint32 seq = reserve_sequence(w, error);
	J *row = seq ? line_add_recommendation(&w->session, id, seq, error) :
		       NULL;

	if (seq)
		count_attempt(&w->add_pace, &line_add_pace,
			      row ? NULL : *error);

	if (row) {
		json_object_object_del(w->recommendations, id);
		remember_friend(w, row);
	}
	return row;
}

static J *pending_groups(struct line_worker *w, GError **error)
{
	Line *s = &w->session;
	J *rows = line_group_invitations(s, error);

	if (!rows)
		return NULL;
	g_clear_pointer(&w->invited_groups, json_object_put);
	w->invited_groups = obj();
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);

		json_object_object_add(w->invited_groups, strfield(row, "id"),
				       json_object_get(row));
	}
	return rows;
}

static J *answer_group(struct line_worker *w, J *params, gboolean join,
		       GError **error)
{
	Line *s = &w->session;
	const char *id = strfield(params, "chat");
	J *invitation = id ? field(w->invited_groups, id) : NULL;

	if (!invitation)
		return problem(error, "Choose a pending group invitation.");
	gint32 seq = reserve_sequence(w, error);

	if (!seq)
		return NULL;
	s->seq--;
	g_autoptr(J) ack = line_group_respond(s, id, join, error);
	s->seq = seq;
	if (!ack)
		return NULL;
	if (join)
		g_hash_table_remove(w->left_groups, id);
	J *result = join ? open_conversation(w, invitation) :
			   g_steal_pointer(&ack);
	json_object_object_del(w->invited_groups, id);
	return result;
}

static void cached_history_free(gpointer data)
{
	struct cached_history *c = data;

	json_object_put(c->rows);
	json_object_put(c->raw);
	g_free(c);
}

static void cache_touch(struct line_worker *w, const char *chat)
{
	GList *link = g_queue_find_custom(&w->history_lru, chat,
					  (GCompareFunc)g_strcmp0);

	if (link) {
		g_queue_unlink(&w->history_lru, link);
		g_queue_push_tail_link(&w->history_lru, link);
		return;
	}
	g_queue_push_tail(&w->history_lru, g_strdup(chat));
	while (w->history_lru.length > HISTORY_CACHE_CHATS) {
		char *old = g_queue_pop_head(&w->history_lru);

		g_hash_table_remove(w->history_cache, old);
		g_free(old);
	}
}

static void cache_store(struct line_worker *w, const char *chat, J *rows,
			J *raw)
{
	struct cached_history *c = g_new0(struct cached_history, 1);

	c->rows = json_object_get(rows);
	c->raw = json_object_get(raw);
	c->cleared_through = history_clear_id(w, chat);
	g_hash_table_replace(w->history_cache, g_strdup(chat), c);
	cache_touch(w, chat);
}

static void cache_clear(struct line_worker *w)
{
	g_hash_table_remove_all(w->history_cache);
	g_queue_clear_full(&w->history_lru, g_free);
	g_clear_pointer(&w->revalidate, g_free);
}

/* Replace the content, retaining at most the ID and the send time. */
static void message_unsent(struct line_worker *w, const char *chat,
			   const char *id)
{
	J *ids = field(w->unsent, chat);
	J *summary = g_hash_table_lookup(w->chats, chat);
	gboolean last = !g_strcmp0(strfield(summary, "lastId"), id);
	gboolean open = !g_strcmp0(w->history_chat, chat);
	struct cached_history *cached =
		g_hash_table_lookup(w->history_cache, chat);
	J *raw = open ? field(w->messages, id) : NULL;
	gint64 time = 0;

	if (!w->unsent)
		return;
	if (!raw && cached)
		raw = field(cached->raw, id);
	time = json_object_get_int64(field(raw, "5"));
	if (!time && last)
		time = json_object_get_int64(field(summary, "time"));
	if (!json_object_is_type(ids, json_type_object)) {
		if (json_object_object_length(w->unsent) >= 64) {
			struct json_object_iterator oldest =
				json_object_iter_begin(w->unsent);

			json_object_object_del(
				w->unsent, json_object_iter_peek_name(&oldest));
		}
		ids = obj();
		json_object_object_add(w->unsent, chat, ids);
	}
	J *previous = field(ids, id);
	gboolean changed = !previous ||
			   (time > 0 && !json_object_get_int64(previous));

	line_remember_unsent(ids, id, time);
	if (cached)
		line_redact_unsent(ids, cached->rows, cached->raw,
				   cached->cleared_through, TRUE);
	if (open && (!cached || cached->raw != w->messages))
		line_redact_unsent(ids, NULL, w->messages, 0, FALSE);
	if (last) {
		json_object_object_del(summary, "preview");
		json_object_object_add(summary, "lastUnsent",
				       json_object_new_boolean(TRUE));
	}
	if (changed)
		line_store_write_unsent(w->session.mid, w->unsent);
	if (w->archive)
		line_offline_unsend(w->archive, chat, id, time);

	g_autoptr(J) event = obj();

	json_object_object_add(event, "event",
			       json_object_new_string("message_unsent"));
	json_object_object_add(event, "chat", json_object_new_string(chat));
	json_object_object_add(event, "row", line_unsent_row(id, time));
	line_emit(event);
}

static void history_position(struct line_worker *w, J *rows, gboolean archived)
{
	J *first = json_object_array_get_idx(rows, 0);

	g_free(w->history_first);
	w->history_first = g_strdup(strfield(first, "id"));
	w->archived_page = archived;
}

static void show_history(struct line_worker *w, const char *chat, J *rows,
			 J *raw, gboolean archived)
{
	history_position(w, rows, archived);
	json_object_put(w->messages);
	w->messages = json_object_get(raw);
	w->latest_history = TRUE;
	w->publish_reads = TRUE;
	if (g_strcmp0(w->history_chat, chat)) {
		g_free(w->history_chat);
		w->history_chat = g_strdup(chat);
		g_clear_pointer(&w->peer_ranges, json_object_put);
		g_clear_pointer(&w->peer_notices, json_object_put);
		g_clear_pointer(&w->square_read_sync, g_free);
		w->peer_notices = obj();
		w->need_ranges = TRUE;
		w->ranges_retry_at = 0;
	}
}

static J *archived_history(struct line_worker *w, const char *chat,
			   const char *before, J **raw, gboolean *more)
{
	J *rows = w->archive ? line_offline_load_history(w->archive, chat,
							 before, raw, more) :
			       NULL;
	if (!rows)
		return NULL;
	guint64 floor = history_clear_id(w, chat);

	for (size_t i = 0; floor && i < json_object_array_length(rows);) {
		J *row = json_object_array_get_idx(rows, i);
		const char *id = strfield(row, "id");

		if (!id || json_object_get_uint64(field(row, "id")) > floor) {
			i++;
			continue;
		}
		if (*raw)
			json_object_object_del(*raw, id);
		json_object_array_del_idx(rows, i, 1);
	}
	line_redact_unsent(field(w->unsent, chat), rows, *raw, floor, !before);
	return rows;
}

static J *history_page(struct line_worker *w, const char *chat, gboolean older,
		       GError **error)
{
	if (older && g_strcmp0(chat, w->history_chat))
		return problem(
			error,
			"Open this conversation to read its earlier messages.");
	if (!older) {
		/* A later visit or refresh retries an unavailable snapshot. */
		if (!w->peer_ranges)
			w->need_ranges = TRUE;
		struct cached_history *c =
			g_hash_table_lookup(w->history_cache, chat);

		if (c && c->cleared_through == history_clear_id(w, chat)) {
			/* Answer from memory now; check LINE after replying. */
			show_history(w, chat, c->rows, c->raw, FALSE);
			cache_touch(w, chat);
			if (!w->offline) {
				g_free(w->revalidate);
				w->revalidate = g_strdup(chat);
			}
			return json_object_get(c->rows);
		}
		g_autoptr(J) raw = NULL;
		J *history = archived_history(w, chat, NULL, &raw, NULL);

		if (history) {
			show_history(w, chat, history, raw, TRUE);
			if (!w->offline) {
				g_free(w->revalidate);
				w->revalidate = g_strdup(chat);
			}
			return history;
		}
		if (w->offline)
			history = json_object_new_array();
		else
			history = recent_history(w, chat, &raw, error);
		if (!history && *error &&
		    transient_failure(&w->session, *error)) {
			g_clear_error(error);
			went_offline(w);
			history = json_object_new_array();
		}
		if (!history)
			return NULL;
		show_history(w, chat, history, raw, w->offline);
		if (!w->offline) {
			cache_store(w, chat, history, raw);
			if (w->archive)
				line_offline_save_history(w->archive, chat,
							  history, raw);
			reconcile_page(w, chat, raw);
		}
		return history;
	}
	J *raw = NULL;
	gboolean more = FALSE;
	gboolean archived = w->offline ||
			    (w->archived_page && line_is_square_chat(chat));
	J *history = NULL;

	if (!archived) {
		g_autoptr(J) boundary = line_history_cursor(w->messages);

		history =
			previous_history(w, chat, boundary, &raw, &more, error);
		if (!history && *error &&
		    transient_failure(&w->session, *error)) {
			g_clear_error(error);
			went_offline(w);
			archived = TRUE;
		}
	}
	if (archived) {
		history = w->history_first ?
				  archived_history(w, chat, w->history_first,
						   &raw, &more) :
				  NULL;
		if (!history)
			history = json_object_new_array();
	}
	if (!history)
		return NULL;
	if (json_object_array_length(history)) {
		history_position(w, history, archived);
		json_object_put(w->messages);
		w->messages = raw;
		w->latest_history = FALSE;
		w->publish_reads = TRUE;
	} else
		json_object_put(raw);
	if (!archived && w->archive && json_object_array_length(history))
		line_offline_save_history(w->archive, chat, history, raw);
	if (!archived && json_object_array_length(history))
		reconcile_page(w, chat, raw);
	J *page = obj();

	json_object_object_add(page, "messages", history);
	json_object_object_add(page, "more", json_object_new_boolean(more));
	return page;
}

/*
 * Fetch a chat's latest page after the reply went out and push it to the
 * window if it changed. The window then shows fresh messages without
 * asking again.
 */
static void revalidate_history(struct line_worker *w, const char *chat)
{
	if (w->offline)
		return;
	g_autoptr(GError) error = NULL;
	g_autoptr(J) raw = NULL;
	g_autoptr(J) rows = recent_history(w, chat, &raw, &error);
	struct cached_history *c = g_hash_table_lookup(w->history_cache, chat);

	if (!rows) {
		if (error && transient_failure(&w->session, error))
			went_offline(w);
		return;
	}
	gboolean same = c && json_object_equal(c->rows, rows);

	if (w->archive && !same)
		line_offline_save_history(w->archive, chat, rows, raw);
	reconcile_page(w, chat, raw);
	if (!g_hash_table_contains(w->history_cache, chat) &&
	    g_strcmp0(chat, w->history_chat))
		return;
	cache_store(w, chat, rows, raw);
	if (g_strcmp0(chat, w->history_chat) || !w->latest_history)
		return;
	show_history(w, chat, rows, raw, FALSE);
	if (same)
		return;
	g_autoptr(J) event = obj();

	json_object_object_add(event, "event",
			       json_object_new_string("history"));
	json_object_object_add(event, "chat", json_object_new_string(chat));
	json_object_object_add(event, "messages", json_object_get(rows));
	line_emit(event);
}

static J *pending_send(struct line_worker *w, const char *chat)
{
	for (size_t i = 0; w->outbox && i < json_object_array_length(w->outbox);
	     i++) {
		J *entry = json_object_array_get_idx(w->outbox, i);

		if (!g_strcmp0(chat, strfield(entry, "chat")))
			return entry;
	}
	return NULL;
}

static J *send_entry(J *intent, const char *status, const char *fingerprint)
{
	J *entry = obj();
	const char *const names[] = { "chat", "intentId", "params" };

	for (guint i = 0; i < G_N_ELEMENTS(names); i++)
		json_object_object_add(
			entry, names[i],
			json_object_get(field(intent, names[i])));
	json_object_object_add(entry, "status", json_object_new_string(status));
	if (fingerprint)
		json_object_object_add(entry, "fingerprint",
				       json_object_new_string(fingerprint));
	return entry;
}

static gboolean save_send(struct line_worker *w, const char *chat, J *entry,
			  GError **error)
{
	if (!w->outbox) {
		problem(error, "The saved message outbox could not be opened.");
		return FALSE;
	}
	g_autoptr(J) rows = json_object_new_array();

	for (size_t i = 0; i < json_object_array_length(w->outbox); i++) {
		J *old = json_object_array_get_idx(w->outbox, i);

		if (g_strcmp0(chat, strfield(old, "chat")))
			json_object_array_add(rows, json_object_get(old));
	}
	if (entry)
		json_object_array_add(rows, json_object_get(entry));
	if (json_object_array_length(rows) > 32) {
		problem(error,
			"The outbox is full. Check an older pending send first.");
		return FALSE;
	}
	if (w->archive && !line_offline_save_outbox(w->archive, rows, error))
		return FALSE;
	json_object_put(w->outbox);
	w->outbox = g_steal_pointer(&rows);
	return TRUE;
}

static J *pending_result(J *entry)
{
	J *result = send_entry(entry, strfield(entry, "status"), NULL);

	json_object_object_add(result, "pending",
			       json_object_new_boolean(TRUE));
	return result;
}

static void send_outcome(J *entry, const char *status, J *receipt,
			 const GError *error)
{
	g_autoptr(J) event = send_entry(entry, status, NULL);

	json_object_object_add(event, "event",
			       json_object_new_string("send_outcome"));
	if (receipt)
		json_object_object_add(event, "receipt",
				       json_object_get(receipt));
	if (error)
		json_object_object_add(event, "error",
				       json_object_new_string(error->message));
	line_emit(event);
}

static void archive_send(struct line_worker *w, J *entry, J *receipt)
{
	const char *chat = strfield(entry, "chat"),
		   *id = strfield(receipt, "id");
	J *params = field(entry, "params");
	gint64 time = json_object_get_int64(field(receipt, "sentAt"));

	if (!id)
		return;
	if (time <= 0)
		time = g_get_real_time() / 1000;
	if (w->archive) {
		g_autoptr(J) rows = json_object_new_array();
		g_autoptr(J) raw = obj();
		J *row = obj(), *message = obj();
		const char *const fields[] = { "text", "emojis", "mentions",
					       "replyTo" };

		for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
			J *value = field(params, fields[i]);

			if (value)
				json_object_object_add(row, fields[i],
						       json_object_get(value));
		}
		json_object_object_add(row, "id", json_object_new_string(id));
		json_object_object_add(row, "from",
				       json_object_new_string("You"));
		json_object_object_add(row, "senderId",
				       json_object_new_string(w->session.mid));
		json_object_object_add(row, "type",
				       json_object_new_string("text"));
		json_object_object_add(row, "time",
				       json_object_new_int64(time));
		json_object_object_add(row, "own",
				       json_object_new_boolean(TRUE));
		json_object_object_add(message, "1",
				       json_object_new_string(w->session.mid));
		json_object_object_add(message, "2",
				       json_object_new_string(chat));
		json_object_object_add(message, "4",
				       json_object_new_string(id));
		json_object_object_add(message, "5",
				       json_object_new_int64(time));
		json_object_object_add(raw, id, message);
		json_object_array_add(rows, row);
		line_offline_save_history(w->archive, chat, rows, raw);
	}
	g_hash_table_remove(w->history_cache, chat);
	if (w->messages && !g_strcmp0(chat, w->history_chat) &&
	    !field(w->messages, id)) {
		J *stub = obj();

		json_object_object_add(stub, "1",
				       json_object_new_string(w->session.mid));
		json_object_object_add(stub, "4", json_object_new_string(id));
		json_object_object_add(w->messages, id, stub);
		w->publish_reads = TRUE;
	}
}

static J *matched_send(struct line_worker *w, J *entry, J *message)
{
	return line_sent_receipt(w->session.mid, strfield(entry, "chat"),
				 strfield(entry, "fingerprint"), message);
}

static gboolean finish_send(struct line_worker *w, J *entry, J *receipt,
			    gboolean archive_receipt)
{
	const char *chat = strfield(entry, "chat");

	if (archive_receipt)
		archive_send(w, entry, receipt);
	if (!save_send(w, chat, NULL, NULL))
		return FALSE;
	g_hash_table_remove(w->checked_sends, strfield(entry, "intentId"));
	return TRUE;
}

static void reconcile_message(struct line_worker *w, const char *chat,
			      J *message)
{
	g_autoptr(J) entry = json_object_get(pending_send(w, chat));
	g_autoptr(J) receipt = matched_send(w, entry, message);

	if (receipt && finish_send(w, entry, receipt, FALSE))
		send_outcome(entry, "sent", receipt, NULL);
}

static void reconcile_page(struct line_worker *w, const char *chat, J *raw)
{
	g_autoptr(J) entry = json_object_get(pending_send(w, chat));

	if (!raw || !strfield(entry, "fingerprint"))
		return;
	json_object_object_foreach(raw, id, message) {
		g_autoptr(J) receipt = matched_send(w, entry, message);

		(void)id;
		if (receipt && finish_send(w, entry, receipt, FALSE)) {
			send_outcome(entry, "sent", receipt, NULL);
			return;
		}
	}
}

struct outgoing_attempt {
	struct line_worker *worker;
	J *entry;
	gboolean started;
};

static gboolean record_attempt(const char *fingerprint, void *data,
			       GError **error)
{
	struct outgoing_attempt *attempt = data;

	g_autoptr(J) entry =
		send_entry(attempt->entry, "uncertain", fingerprint);

	attempt->started = save_send(attempt->worker, strfield(entry, "chat"),
				     entry, error);
	return attempt->started;
}

static gboolean send_must_wait(const GError *error)
{
	return g_error_matches(error, LINE_ERROR, LINE_CONNECT_FAILED) ||
	       g_error_matches(error, LINE_ERROR, LINE_RATE_LIMITED);
}

static gboolean send_rejected(const Line *s, const GError *error)
{
	return !s->rpc_may_have_run || line_auth_rejected(s->service_code) ||
	       s->http_status == 401 || s->http_status == 403 ||
	       g_error_matches(error, LINE_ERROR, LINE_LETTER_SEALING_RELINK) ||
	       g_error_matches(error, LINE_ERROR, LINE_SESSION_EXPIRED) ||
	       g_error_matches(error, LINE_ERROR, LINE_METHOD_UNAVAILABLE);
}

static J *deliver_send(struct line_worker *w, J *entry, GError **error)
{
	Line *s = &w->session;
	const char *chat = strfield(entry, "chat");
	J *params = field(entry, "params");
	const char *reply = strfield(params, "replyTo");

	g_autoptr(J) ack = NULL;

	struct outgoing_attempt attempt = { .worker = w, .entry = entry };

	if (w->offline || !w->online_chats)
		return pending_result(entry);
	int wait = line_transport_retry_after();

	if (wait) {
		w->outbox_due =
			g_get_monotonic_time() + (gint64)wait * G_USEC_PER_SEC;
		return pending_result(entry);
	}
	if ((!g_hash_table_contains(w->chats, chat) &&
	     g_strcmp0(chat, w->session.mid)) ||
	    (reply && field(field(w->unsent, chat), reply))) {
		problem(error,
			"This conversation or reply is no longer available.");
		goto failed;
	}
	gint32 seq = reserve_sequence(w, error);

	if (seq) {
		s->seq--;
		ack = line_send_decorated_tracked(
			s, chat, strfield(params, "text"), reply,
			field(params, "emojis"), field(params, "mentions"),
			record_attempt, &attempt, error);
		s->seq = seq;
	}
	if (ack && finish_send(w, entry, ack, TRUE))
		return g_steal_pointer(&ack);
	if (attempt.started && (ack || !send_rejected(s, *error))) {
		if (!s->http_status &&
		    g_error_matches(*error, LINE_ERROR, LINE_TIMEOUT))
			went_offline(w);
		g_clear_error(error);
		w->outbox_due = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
		return pending_result(pending_send(w, chat));
	}
	if ((!attempt.started || !s->rpc_may_have_run) &&
	    send_must_wait(*error)) {
		gboolean disconnected = g_error_matches(*error, LINE_ERROR,
							LINE_CONNECT_FAILED);

		g_clear_error(error);
		if (!attempt.started || save_send(w, chat, entry, NULL)) {
			if (disconnected)
				went_offline(w);
			else
				w->outbox_due =
					g_get_monotonic_time() +
					MAX(1, line_transport_retry_after()) *
						G_USEC_PER_SEC;
		}
		return pending_result(pending_send(w, chat));
	}
failed:
	if (save_send(w, chat, NULL, NULL))
		return NULL;
	g_clear_error(error);
	return pending_result(pending_send(w, chat));
}

static J *submit_send(struct line_worker *w, const char *chat, J *params,
		      GError **error)
{
	J *existing = pending_send(w, chat);

	if (existing)
		return pending_result(existing);
	g_autofree char *intent = g_uuid_string_random();

	g_autoptr(J) entry = obj();
	g_autoptr(J) payload = obj();
	const char *const names[] = { "text", "replyTo", "emojis", "mentions" };

	for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
		J *value = field(params, names[i]);

		if (value)
			json_object_object_add(payload, names[i],
					       json_object_get(value));
	}
	json_object_object_add(entry, "chat", json_object_new_string(chat));
	json_object_object_add(entry, "intentId",
			       json_object_new_string(intent));
	json_object_object_add(entry, "status",
			       json_object_new_string("queued"));
	json_object_object_add(entry, "params", json_object_get(payload));
	if (!save_send(w, chat, entry, error))
		return NULL;
	return deliver_send(w, entry, error);
}

static J *check_send(struct line_worker *w, J *entry, GError **error)
{
	const char *chat = strfield(entry, "chat");

	if (w->offline || !w->online_chats)
		return pending_result(entry);
	if (!g_strcmp0(strfield(entry, "status"), "queued"))
		return deliver_send(w, entry, error);
	g_autoptr(J) raw = NULL;
	g_autoptr(J) rows = recent_history(w, chat, &raw, error);

	if (rows && raw) {
		if (w->archive)
			line_offline_save_history(w->archive, chat, rows, raw);
		json_object_object_foreach(raw, id, message) {
			J *receipt = matched_send(w, entry, message);

			(void)id;
			if (!receipt)
				continue;
			if (finish_send(w, entry, receipt, FALSE))
				return receipt;
			json_object_put(receipt);
			break;
		}
	}
	if (*error && transient_failure(&w->session, *error))
		went_offline(w);
	g_clear_error(error);
	return pending_result(entry);
}

static J *outbox_action(struct line_worker *w, J *params, gboolean discard,
			GError **error)
{
	const char *chat = strfield(params, "chat"),
		   *id = strfield(params, "intentId");
	g_autoptr(J) entry =
		json_object_get(chat ? pending_send(w, chat) : NULL);

	if (!entry || !id || g_strcmp0(id, strfield(entry, "intentId")))
		return problem(error, "This send is no longer pending.");
	if (discard) {
		if (!save_send(w, chat, NULL, error))
			return NULL;
		g_hash_table_remove(w->checked_sends, id);
		send_outcome(entry, "discarded", NULL, NULL);
		return obj();
	}
	g_autoptr(GError) failure = NULL;
	g_autoptr(J) result = check_send(w, entry, &failure);

	if (!result)
		send_outcome(entry, "failed", NULL, failure);
	else if (!field(result, "pending"))
		send_outcome(entry, "sent", result, NULL);
	else
		send_outcome(result, strfield(result, "status"), NULL, NULL);
	return result ? json_object_get(result) : obj();
}

static J *forward_message(struct line_worker *w, const char *destination,
			  J *message, GError **error)
{
	if (!message || !destination ||
	    !g_hash_table_contains(w->chats, destination))
		return problem(error,
			       "Select a message and a destination chat.");
	int kind = json_object_get_int(field(message, "15"));

	if (kind != 0 && kind != 7 && kind != 13 && kind != 15 &&
	    kind != LINE_MEDIA_IMAGE && kind != LINE_MEDIA_FILE &&
	    kind != LINE_MEDIA_AUDIO && kind != LINE_MEDIA_VIDEO)
		return problem(error, "This message type cannot be forwarded.");
	gint32 seq = reserve_sequence(w, error);

	if (!seq)
		return NULL;
	Line *s = &w->session;

	if (line_is_square_chat(destination)) {
		const char *my_mid = my_square_member_id(w, destination);

		if (kind == 0) {
			g_autofree char *text =
				line_message_preview(s, message, error);

			return text ? line_square_send_text(s, destination,
							    my_mid, text, NULL,
							    NULL, NULL, seq,
							    error) :
				      NULL;
		}
		if (kind == 7)
			return line_square_forward_sticker(
				s, destination, my_mid, message, seq, error);
		if (kind == 15)
			return line_square_forward_location(
				s, destination, my_mid, message, seq, error);
		if (kind == 13)
			return line_square_forward_contact(
				s, destination, my_mid, message, seq, error);
		return line_forward_square_attachment(s, destination, my_mid,
						      message, seq, error);
	}
	s->seq--;
	J *receipt =
		kind == 0 ?
			line_forward_text(s, destination, message, error) :
		kind == 7 ?
			line_forward_sticker(s, destination, message, error) :
		kind == 15 ?
			line_forward_location(s, destination, message, error) :
		kind == 13 ?
			line_forward_contact(s, destination, message, error) :
			line_forward_attachment(s, destination, message, error);

	s->seq = seq;
	return receipt;
}

static J *announcement_list(struct line_worker *w, const char *chat,
			    GError **error)
{
	J *rows = line_is_square_chat(chat) ?
			  line_square_announcements(
				  &w->session, chat,
				  my_square_can_announce(w, chat), error) :
			  line_announcements(&w->session, chat, error);

	if (rows) {
		json_object_put(w->announcements);
		w->announcements = json_object_get(rows);
		g_free(w->announcement_chat);
		w->announcement_chat = g_strdup(chat);
	}
	return rows;
}

static J *remove_announcement(struct line_worker *w, const char *chat,
			      const char *id, GError **error)
{
	if (id && w->announcements && !g_strcmp0(chat, w->announcement_chat)) {
		for (size_t i = 0;
		     i < json_object_array_length(w->announcements); i++) {
			J *row = json_object_array_get_idx(w->announcements, i);

			if (g_strcmp0(id, strfield(row, "id")) ||
			    !json_object_get_boolean(field(row, "canRemove")))
				continue;
			J *receipt;

			if (line_is_square_chat(chat)) {
				receipt = line_square_remove_announcement(
					&w->session, chat, id, error);
			} else {
				gint32 seq = reserve_sequence(w, error);

				receipt = seq ? line_remove_announcement(
							&w->session, chat, id,
							seq, error) :
						NULL;
			}
			if (receipt)
				json_object_array_del_idx(w->announcements, i,
							  1);
			return receipt;
		}
	}
	return problem(
		error,
		"Refresh the list and select an announcement you can remove.");
}

enum {
	CARD_LIMIT = 64,
	/* Live profile changes fetched per batch before a full reload. */
	LIVE_CARD_LIMIT = 8
};

static J *ensure(J **slot)
{
	if (!*slot)
		*slot = obj();
	return *slot;
}

/* json-c objects keep insertion order, so the head is the oldest card. */
static void remember_card(struct line_worker *w, J *row)
{
	const char *id = strfield(row, "id");

	if (!id)
		return;
	J *cards = ensure(&w->cards);

	if (!field(cards, id) &&
	    json_object_object_length(cards) >= CARD_LIMIT) {
		struct lh_entry *oldest = json_object_get_object(cards)->head;

		if (oldest) {
			g_autofree char *key =
				g_strdup((const char *)lh_entry_k(oldest));
			json_object_object_del(cards, key);
		}
	}
	json_object_object_add(cards, id, json_object_get(row));
	if (w->friends)
		remember_friend(w, row);
}

static J *profile_card(struct line_worker *w, const char *id, GError **error)
{
	Line *s = &w->session;

	if (id && !g_strcmp0(id, s->mid)) {
		J *row = line_own_profile(s, error);

		if (row)
			json_object_object_add(row, "self",
					       json_object_new_boolean(TRUE));
		return row;
	}
	if (line_square_id(id, 'p'))
		return line_square_member_card(s, id, error);
	J *row = line_friend_card(s, id, error);

	if (row)
		remember_card(w, row);
	return row;
}

static J *profile_action(struct line_worker *w, const char *method, J *params,
			 GError **error)
{
	Line *s = &w->session;
	const char *id = strfield(params, "id");
	J *row = id ? field(w->cards, id) : NULL;

	if (!row && w->friends && id)
		row = field(w->friends, id);
	if (!row)
		return problem(error, "Open this person's profile again.");
	if (!strcmp(method, "openProfileChat")) {
		ensure(&w->opened_chats);
		return open_conversation(w, row);
	}
	gboolean adding = !strcmp(method, "addProfileFriend");

	if (adding && !paced(&w->add_pace, &line_add_pace, error))
		return NULL;
	gint32 seq = reserve_sequence(w, error);

	if (!seq)
		return NULL;
	J *updated;

	if (adding) {
		const char *chat = strfield(params, "chat");

		updated = line_friend_add_from(s, row,
					       chat && *chat == 'c' ?
						       LINE_FROM_GROUP :
						       LINE_FROM_STRANGER,
					       seq, error);
		count_attempt(&w->add_pace, &line_add_pace,
			      updated ? NULL : *error);
	} else {
		J *flag = field(params, "blocked");

		if (!json_object_is_type(flag, json_type_boolean))
			return problem(error, "Choose block or unblock.");
		updated = line_friend_block(
			s, row, json_object_get_boolean(flag), seq, error);
	}
	if (updated)
		remember_card(w, updated);
	return updated;
}

static J *friend_requests(struct line_worker *w, GError **error)
{
	J *rows = line_friend_requests(&w->session, error);

	if (!rows)
		return NULL;
	g_clear_pointer(&w->requests, json_object_put);
	w->requests = obj();
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);

		json_object_object_add(w->requests, strfield(row, "id"),
				       json_object_get(row));
	}
	return rows;
}

static J *answer_request(struct line_worker *w, const char *id, gboolean accept,
			 GError **error)
{
	J *row = id ? field(w->requests, id) : NULL;

	if (!row)
		return problem(error, "Refresh your friend requests.");
	J *result;

	if (accept) {
		gint32 seq = reserve_sequence(w, error);

		result = seq ? line_friend_add_from(&w->session, row,
						    LINE_FROM_REQUEST, seq,
						    error) :
			       NULL;
		if (result)
			remember_card(w, result);
	} else {
		result = line_friend_request_decline(&w->session, row, error);
	}
	if (result)
		json_object_object_del(w->requests, id);
	return result;
}

/*
 * Signing out always ends the session on this computer, whether or not it
 * is running. LINE is asked to end it too, unless it has already refused
 * these credentials or asked the client to wait. "confirmed" says whether
 * LINE ended it, so the window can ask the user to check linked devices
 * when it couldn't. Only a failure to delete the files keeps the session.
 */
static J *sign_out(struct line_worker *w, GError **error)
{
	Line *s = &w->session;
	gboolean confirmed = FALSE;

	if (w->dir < 0) {
		g_autoptr(GError) unreadable = NULL;

		w->dir = line_store_open(s, &unreadable);
		if (w->dir >= 0)
			line_cooldown_use_store(w->dir);
		/* A session file this version cannot read is removed unread. */
		if (w->dir < 0) {
			int discarded = -1;

			if (!line_store_discard(&discarded, error))
				return NULL;
			if (discarded >= 0) {
				purge_archive(w, discarded);
				close(discarded);
			}
		}
	}
	if (w->dir >= 0) {
		g_autoptr(GError) revoke_error = NULL;

		confirmed = s->denied || !s->access;
		if (!confirmed && !line_transport_retry_after()) {
			g_autoptr(J) ack = line_rpc(s, "/AS4", "logoutV2", NULL,
						    &revoke_error);

			confirmed = ack || session_rejected(s, revoke_error);
		}
		if (!forget(w, error))
			return NULL;
	}
	J *reply = obj();

	json_object_object_add(reply, "confirmed",
			       json_object_new_boolean(confirmed));
	if (w->archive_cleanup_failed)
		json_object_object_add(reply, "archiveCleanupFailed",
				       json_object_new_boolean(TRUE));
	return reply;
}

static J *group_roster(struct line_worker *w, J *members)
{
	Line *s = &w->session;
	g_autoptr(J) names = obj();
	J *roster = json_object_new_array();
	size_t count = members ? json_object_array_length(members) : 0;

	/* Resolve the visible tiles in one batch and retain the full count. */
	for (size_t i = 0; i < MIN(count, 12); i++) {
		const char *id = json_object_get_string(
			json_object_array_get_idx(members, i));

		if (g_strcmp0(id, s->mid))
			json_object_object_add(names, id,
					       json_object_new_string(""));
	}
	line_resolve_names(s, names, 'u');
	for (size_t i = 0; i <= count; i++) {
		const char *id = i == 0 ? s->mid :
					  json_object_get_string(
						  json_object_array_get_idx(
							  members, i - 1));

		if (i && !g_strcmp0(id, s->mid))
			continue;
		const char *name = i ? strfield(names, id) : w->name;
		const char *avatar = strfield(s->pictures, id);
		J *member = obj();

		json_object_object_add(member, "id",
				       json_object_new_string(id));
		if (name && *name)
			json_object_object_add(member, "name",
					       json_object_new_string(name));
		if (avatar)
			json_object_object_add(member, "avatarKey",
					       json_object_new_string(avatar));
		json_object_array_add(roster, member);
	}
	return roster;
}

static J *square_list_hub(struct line_worker *w, GError **error)
{
	Line *s = &w->session;
	g_autoptr(J) sqs = NULL, mems = NULL, auths = NULL, c2s = NULL;
	g_autoptr(J) chat_rows =
		line_square_chats(s, &sqs, &mems, &auths, &c2s, error);

	if (!chat_rows)
		return NULL;
	g_clear_pointer(&w->squares, json_object_put);
	g_clear_pointer(&w->square_members, json_object_put);
	g_clear_pointer(&w->square_authorities, json_object_put);
	g_clear_pointer(&w->square_chat_to_square, json_object_put);
	w->squares = g_steal_pointer(&sqs);
	w->square_members = g_steal_pointer(&mems);
	w->square_authorities = g_steal_pointer(&auths);
	w->square_chat_to_square = g_steal_pointer(&c2s);
	w->square_disabled = FALSE;
	for (size_t i = 0; i < json_object_array_length(chat_rows); i++) {
		J *cr = json_object_array_get_idx(chat_rows, i);
		const char *cid = strfield(cr, "id");

		if (cid && !g_hash_table_contains(w->chats, cid))
			json_object_put(open_conversation(w, cr));
	}
	if (json_object_object_length(w->squares) && !w->square_poll_due)
		w->square_poll_due =
			g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
	J *out = obj(), *groups = json_object_new_array();

	json_object_object_foreach(w->squares, smid, sq) {
		J *mem = field(w->square_members, smid);
		J *entry = obj();
		const char *name = strfield(sq, "2");
		const char *desc = strfield(sq, "5");
		const char *hash = strfield(sq, "4");
		int role = mem ? json_object_get_int(field(mem, "8")) : 10;

		json_object_object_add(entry, "squareMid",
				       json_object_new_string(smid));
		json_object_object_add(
			entry, "name",
			json_object_new_string(name && *name ? name : smid));
		json_object_object_add(entry, "description",
				       json_object_new_string(desc ? desc :
								     ""));
		json_object_object_add(
			entry, "myRole",
			json_object_new_string(role == 1 ? "admin" :
					       role == 2 ? "coadmin" :
							   "member"));
		if (hash && *hash) {
			g_autofree char *av = g_strconcat("/", hash, NULL);

			json_object_object_add(entry, "avatarKey",
					       json_object_new_string(av));
		}
		J *rooms = json_object_new_array();

		for (size_t i = 0; i < json_object_array_length(chat_rows);
		     i++) {
			J *cr = json_object_array_get_idx(chat_rows, i);

			if (!g_strcmp0(strfield(cr, "squareMid"), smid))
				json_object_array_add(rooms,
						      json_object_get(cr));
		}
		json_object_object_add(entry, "chats", rooms);
		json_object_array_add(groups, entry);
	}
	json_object_object_add(out, "squares", groups);
	return out;
}

static J *square_dispatch(struct line_worker *w, const char *method, J *params,
			  GError **error)
{
	Line *s = &w->session;

	if (!strcmp(method, "squareList"))
		return square_list_hub(w, error);
	if (!strcmp(method, "squareCategories"))
		return line_square_categories(s, error);
	if (!strcmp(method, "squarePopularKeywords"))
		return line_square_popular_keywords(s, error);
	if (!strcmp(method, "squareSearch"))
		return line_square_search(s, strfield(params, "query"),
					  strfield(params, "cursor"), error);
	if (!strcmp(method, "squareFindTicket"))
		return line_square_find_ticket(s, strfield(params, "input"),
					       error);
	if (!strcmp(method, "squareJoin")) {
		J *res = line_square_join(s, strfield(params, "squareMid"),
					  strfield(params, "chatMid"),
					  strfield(params, "displayName"),
					  strfield(params, "joinCode"),
					  strfield(params, "approvalMessage"),
					  error);

		if (res) {
			remember_square_raw(w, res);
			J *chat = field(res, "chat");

			if (chat) {
				J *opened = open_conversation(w, chat);

				json_object_object_add(res, "chat", opened);
			}
			if (!w->square_poll_due)
				w->square_poll_due = g_get_monotonic_time() +
						     4 * G_USEC_PER_SEC;
		}
		return res;
	}
	if (!strcmp(method, "squareJoinChat")) {
		g_autoptr(J) row = line_square_join_chat(
			s, strfield(params, "chat"), error);

		return row ? open_conversation(w, row) : NULL;
	}
	if (!strcmp(method, "squareCreate")) {
		gint32 seq = reserve_sequence(w, error);

		if (!seq)
			return NULL;
		J *res = line_square_create(
			s, strfield(params, "name"),
			strfield(params, "description"),
			strfield(params, "welcome"),
			strfield(params, "displayName"),
			json_object_get_int(field(params, "categoryId")),
			json_object_get_boolean(field(params, "searchable")),
			json_object_get_int(field(params, "joinMethod")),
			strfield(params, "joinCode"),
			strfield(params, "approvalQuestion"), seq, error);

		if (res) {
			remember_square_raw(w, res);
			J *chat = field(res, "chat");

			if (chat) {
				J *opened = open_conversation(w, chat);

				json_object_object_add(res, "chat", opened);
			}
		}
		return res;
	}
	if (!strcmp(method, "squareCreateChat")) {
		gint32 seq = reserve_sequence(w, error);

		if (!seq)
			return NULL;
		g_autoptr(J) row = line_square_create_chat(
			s, strfield(params, "squareMid"),
			strfield(params, "name"), seq, error);

		if (!row)
			return NULL;
		const char *cid = strfield(row, "id");
		const char *smid = strfield(params, "squareMid");

		if (cid && smid) {
			if (!w->square_chat_to_square)
				w->square_chat_to_square = obj();
			json_object_object_add(w->square_chat_to_square, cid,
					       json_object_new_string(smid));
		}
		return open_conversation(w, row);
	}
	const char *chat = strfield(params, "chat");
	const char *smid = strfield(params, "squareMid");

	if (!smid && chat)
		smid = square_for_chat(w, chat);
	if (!strcmp(method, "squareDetail")) {
		J *res = line_square_detail(s, smid, chat, error);

		if (res)
			remember_square_raw(w, res);
		return res;
	}
	if (!strcmp(method, "squareInviteLink"))
		return line_square_invite_link(s, chat ? chat : smid, error);
	if (!strcmp(method, "squareUpdate")) {
		J *cur = smid && w->squares ? field(w->squares, smid) : NULL;
		J *res = line_square_update(s, cur, params, error);

		if (res)
			remember_square_raw(w, res);
		return res;
	}
	if (!strcmp(method, "squareUpdateChat")) {
		J *cur = chat && w->square_chats_raw ?
				 field(w->square_chats_raw, chat) :
				 NULL;

		return line_square_update_chat(s, cur, strfield(params, "name"),
					       error);
	}
	if (!strcmp(method, "squareUpdateProfile")) {
		J *mem = my_square_member(w, smid);
		J *res = line_square_update_my_profile(
			s, mem, strfield(params, "displayName"),
			strfield(params, "bio"), NULL, error);

		if (res)
			remember_square_raw(w, res);
		return res;
	}
	if (!strcmp(method, "squareUpdatePicture")) {
		J *mem = my_square_member(w, smid);
		J *res = line_square_update_picture(
			s, mem, strfield(params, "path"), error);

		if (res)
			remember_square_raw(w, res);
		return res;
	}
	if (!strcmp(method, "squareLeave")) {
		J *res = line_square_leave(s, smid, error);

		if (res && smid) {
			if (w->squares)
				json_object_object_del(w->squares, smid);
			if (w->square_members)
				json_object_object_del(w->square_members, smid);
			if (w->square_authorities)
				json_object_object_del(w->square_authorities,
						       smid);
			if (chat) {
				g_hash_table_remove(w->chats, chat);
				json_object_object_del(w->opened_chats, chat);
				json_object_object_del(w->unread_heads, chat);
				drop_chat_archive(w, chat);
			}
			clear_global_search(w);
		}
		return res;
	}
	if (!strcmp(method, "squareLeaveChat")) {
		J *res = line_square_leave_chat(s, chat, 0, error);

		if (res && chat) {
			g_hash_table_remove(w->chats, chat);
			json_object_object_del(w->opened_chats, chat);
			json_object_object_del(w->unread_heads, chat);
			drop_chat_archive(w, chat);
			clear_global_search(w);
		}
		return res;
	}
	if (!strcmp(method, "squareMembers") ||
	    !strcmp(method, "squarePendingMembers"))
		return line_square_members(
			s, smid, strfield(params, "cursor"),
			strfield(params, "query"),
			!strcmp(method, "squarePendingMembers"), error);
	if (!strcmp(method, "squareModerateMember"))
		return line_square_moderate_member(s, smid,
						   strfield(params, "member"),
						   strfield(params, "action"),
						   error);
	if (!strcmp(method, "squareApproveMember") ||
	    !strcmp(method, "squareRejectMember"))
		return line_square_answer_join_request(
			s, smid, strfield(params, "member"),
			!strcmp(method, "squareApproveMember"), error);
	if (!strcmp(method, "squareDestroyMessage")) {
		if (!chat || g_strcmp0(chat, w->history_chat) ||
		    !field(w->messages, strfield(params, "id")))
			return problem(error,
				       "Select a message in this OpenChat.");
		return line_square_destroy_message(
			s, chat, strfield(params, "id"), error);
	}
	return NULL;
}

static J *dispatch(struct line_worker *w, const char *method, J *params,
		   GError **error)
{
	Line *s = &w->session;

	if (!method)
		return problem(error, "Missing request method.");
	if (!strcmp(method, "login"))
		return login(w, error);
	/* A saved session can be signed out before it ever finished signing in. */
	if (!strcmp(method, "logout"))
		return sign_out(w, error);
	if (!w->ready)
		return problem(error, "Sign in to LINE first.");
	if (!strcmp(method, "foreground")) {
		gboolean active =
			json_object_get_boolean(field(params, "active"));

		if (active && !w->foreground && !w->sync_failures &&
		    (!w->live || w->live_failing) && w->timer >= 0) {
			struct itimerspec timer;

			if (!timerfd_gettime(w->timer, &timer) &&
			    timer.it_value.tv_sec >= 1)
				wake_after(w, 1);
		}
		if (active && !w->foreground && w->square_poll_due)
			w->square_poll_due = g_get_monotonic_time();
		w->foreground = active;
		return obj();
	}
	if (!strcmp(method, "chats"))
		return chat_directory(w, error);
	if (!strcmp(method, "recoverSend") || !strcmp(method, "discardSend"))
		return outbox_action(w, params, !strcmp(method, "discardSend"),
				     error);
	if (!strcmp(method, "friends"))
		return friend_directory(w, params, error);
	if (!strcmp(method, "openKeepMemo")) {
		if (!s->mid)
			return problem(error, "Sign in to open Keep Memo.");
		g_autoptr(J) row = obj();

		json_object_object_add(row, "id",
				       json_object_new_string(s->mid));
		json_object_object_add(row, "name",
				       json_object_new_string("Keep Memo"));
		ensure(&w->opened_chats);
		return open_conversation(w, row);
	}
	if (!strcmp(method, "myProfile"))
		return line_own_profile(s, error);
	if (!strcmp(method, "updateMyProfile")) {
		gint32 seq = reserve_sequence(w, error);
		J *row = seq ? line_own_profile_update(s, params, seq, error) :
			       NULL;
		const char *name = strfield(row, "name");

		if (name) {
			g_free(w->name);
			w->name = g_strdup(name);
		}
		return row;
	}
	if (!strcmp(method, "profileCard"))
		return profile_card(w, strfield(params, "id"), error);
	if (!strcmp(method, "openProfileChat") ||
	    !strcmp(method, "addProfileFriend") ||
	    !strcmp(method, "setProfileBlocked"))
		return profile_action(w, method, params, error);
	if (!strcmp(method, "friendRequests"))
		return friend_requests(w, error);
	if (!strcmp(method, "acceptFriendRequest") ||
	    !strcmp(method, "declineFriendRequest"))
		return answer_request(w, strfield(params, "id"),
				      !strcmp(method, "acceptFriendRequest"),
				      error);
	if (!strcmp(method, "updateMyPicture"))
		return line_own_picture_update(s, strfield(params, "path"),
					       error);
	if (!strcmp(method, "accountSettings"))
		return line_account_settings(s, error);
	if (!strcmp(method, "updateAccountSetting")) {
		const char *key = strfield(params, "key");
		J *val = field(params, "value");

		if (!key || !json_object_is_type(val, json_type_boolean))
			return problem(error, "Choose a LINE account setting.");
		gint32 seq = reserve_sequence(w, error);

		return seq ? line_account_settings_update(
				     s, key, json_object_get_boolean(val), seq,
				     error) :
			     NULL;
	}
	if (!strcmp(method, "addRecommendation"))
		return add_recommendation(w, strfield(params, "id"), error);
	if (!strcmp(method, "findFriend"))
		return find_friend(w, strfield(params, "query"), error);
	if (!strcmp(method, "addFriend"))
		return add_friend(w, strfield(params, "lookup"), error);
	if (!strcmp(method, "friendInvite")) {
		J *refresh = field(params, "refresh");
		J *expires = field(params, "expirationTime");
		J *max_uses = field(params, "maxUseCount");
		gint64 count = json_object_get_int64(max_uses);

		if (!refresh && !expires && !max_uses)
			return line_friend_invite(s, error);
		if ((refresh &&
		     !json_object_is_type(refresh, json_type_boolean)) ||
		    (expires && !json_object_is_type(expires, json_type_int)) ||
		    (max_uses &&
		     !json_object_is_type(max_uses, json_type_int)) ||
		    count < 0 || count > G_MAXINT32)
			return problem(error,
				       "Invalid personal QR ticket settings.");
		struct line_friend_ticket_request request = {
			.expiration_time = json_object_get_int64(expires),
			.max_use_count = count,
			.refresh = json_object_get_boolean(refresh),
		};

		/* The request ID delivers completion to the UI's callback. */
		return line_friend_ticket(s, &request, error);
	}
	if (!strcmp(method, "liffView"))
		return line_liff_view(s, w->chats, params, error);
	if (!strcmp(method, "openFriend") || !strcmp(method, "renameFriend") ||
	    !strcmp(method, "setFriendFavorite") ||
	    !strcmp(method, "setFriendHidden") ||
	    !strcmp(method, "setFriendBlocked") ||
	    !strcmp(method, "deleteFriend"))
		return friend_action(w, method, params, error);
	if (!strcmp(method, "findAllMessages"))
		return find_all_messages(w, params, error);
	if (!strcmp(method, "createGroup"))
		return change_group(w, NULL, params, error);
	if (!strcmp(method, "groupInvitations"))
		return pending_groups(w, error);
	if (!strcmp(method, "groupFindTicket")) {
		g_autofree char *ticket = NULL;
		J *row = line_group_find_ticket(s, strfield(params, "input"),
						&ticket, error);

		if (row) {
			g_clear_pointer(&w->group_ticket_chat, json_object_put);
			g_clear_pointer(&w->group_ticket_id, g_free);
			w->group_ticket_chat = json_object_get(row);
			w->group_ticket_id = g_steal_pointer(&ticket);
		}
		return row;
	}
	if (!strcmp(method, "groupJoinTicket")) {
		const char *cid = strfield(params, "chat");

		if (!w->group_ticket_chat || !w->group_ticket_id ||
		    g_strcmp0(cid, strfield(w->group_ticket_chat, "id")))
			return problem(error,
				       "Open a group invite link first.");
		gint32 seq = reserve_sequence(w, error);

		if (!seq)
			return NULL;
		g_autoptr(J) ack = line_group_join_ticket(
			s, cid, w->group_ticket_id, seq, error);

		if (!ack)
			return NULL;
		g_hash_table_remove(w->left_groups, cid);
		J *opened = open_conversation(w, w->group_ticket_chat);

		g_clear_pointer(&w->group_ticket_chat, json_object_put);
		g_clear_pointer(&w->group_ticket_id, g_free);
		return opened;
	}
	if (g_str_has_prefix(method, "square"))
		return square_dispatch(w, method, params, error);
	gboolean join = !strcmp(method, "joinGroup");

	if (join || !strcmp(method, "declineGroup"))
		return answer_group(w, params, join, error);
	gboolean stickers = !strcmp(method, "stickerCatalog");

	if (stickers || !strcmp(method, "emojiCatalog")) {
		gint64 offset = json_object_get_int64(field(params, "offset"));
		gint64 limit =
			field(params, "limit") ?
				json_object_get_int64(field(params, "limit")) :
				96;
		const char *query = strfield(params, "query");

		if (offset < 0 || offset > G_MAXUINT || limit < 1 ||
		    limit > 128 ||
		    (query && (strlen(query) > 512 ||
			       !g_utf8_validate(query, -1, NULL))))
			return problem(error, "Invalid catalog page.");
		if (stickers) {
			if (!w->stickers)
				w->stickers = line_stickers_new();
			return line_sticker_catalog(s, w->stickers, offset,
						    limit, query, error);
		}
		return line_emoji_catalog_page(s, (guint)offset, (guint)limit,
					       query, error);
	}
	if (!strcmp(method, "releaseMedia")) {
		const char *lease = strfield(params, "lease");

		if (lease)
			g_hash_table_remove(w->leases, lease);
		return json_object_new_boolean(TRUE);
	}
	const char *chat = strfield(params, "chat"),
		   *id = strfield(params, "id"),
		   *path = strfield(params, "path");
	/*
	 * Call control remains available when the caller has no chat history or
	 * the user has switched conversations while a call is ringing.
	 */
	if (!strcmp(method, "callEnd"))
		return line_voice_hangup(error);
	if (!strcmp(method, "callMute"))
		return line_voice_mute(
			json_object_get_boolean(field(params, "muted")), error);
	if (!strcmp(method, "callShare"))
		return line_call_share(
			json_object_get_boolean(field(params, "enabled")),
			error);
	if (!strcmp(method, "callPreview")) {
		line_call_preview(chat, json_object_get_boolean(
						field(params, "visible")));
		return obj();
	}
	if (!strcmp(method, "callCamera"))
		return line_call_camera(
			json_object_get_boolean(field(params, "enabled")),
			error);
	if (!strcmp(method, "callVideoFocus"))
		return line_call_video_focus(chat, id, error);
	if (!strcmp(method, "callAccept"))
		return line_voice_answer(chat, error);
	if (!strcmp(method, "callDecline"))
		return line_voice_decline(chat, error);
	if (!strcmp(method, "callFrame")) {
		int width = 0, height = 0, rotation = 0;
		struct line_preview_frame info = { 0 };
		gboolean share = json_object_get_boolean(
			json_object_object_get(params, "share"));
		gboolean self = !share &&
				json_object_get_boolean(field(params, "self"));
		gboolean sharing = FALSE;
		g_autoptr(GBytes) frame =
			share ? line_call_take_share_frame(chat, &info,
							   &sharing) :
			self  ? line_call_take_self_frame(chat, &width,
							  &height) :
				line_call_take_frame(chat, &info);

		if (!frame)
			return obj();
		if (!self) {
			width = info.width;
			height = info.height;
			rotation = info.rotation;
		}
		J *video = line_media_lease(w->leases, frame, error);

		if (video) {
			json_object_object_add(video, "width",
					       json_object_new_int(width));
			json_object_object_add(video, "height",
					       json_object_new_int(height));
			json_object_object_add(video, "format",
					       json_object_new_string("i420"));
			json_object_object_add(video, "rotation",
					       json_object_new_int(rotation));
			json_object_object_add(
				video, "cameraEnabled",
				json_object_new_boolean(info.camera_enabled));
			if (!self) {
				json_object_object_add(
					video, "decodedAt",
					json_object_new_int64(info.decoded_at));
				json_object_object_add(
					video, "screen",
					json_object_new_boolean(info.screen));
			}
			if (share)
				json_object_object_add(
					video, "peerSharing",
					json_object_new_boolean(sharing));
		}
		return video;
	}
	if (!chat || !g_hash_table_contains(w->chats, chat))
		return problem(error,
			       "Select a conversation from your chat list.");
	if (!strcmp(method, "groupMembers"))
		return line_group_members(s, chat, error);
	if (!strcmp(method, "mentionCandidates")) {
		const char *q = strfield(params, "query");

		if (line_is_square_chat(chat)) {
			g_autoptr(GError) err = NULL;
			J *rows = line_square_search_mentionables(s, chat, q,
								  &err);

			if (rows && json_object_array_length(rows))
				return rows;
			json_object_put(rows);
			const char *sq =
				w->square_chat_to_square ?
					strfield(w->square_chat_to_square,
						 chat) :
					NULL;
			if (!sq) {
				if (err) {
					g_propagate_error(
						error, g_steal_pointer(&err));
					return NULL;
				}
				return json_object_new_array();
			}
			g_autoptr(J) detail = line_square_members(
				s, sq, NULL, q, FALSE, error);
			if (!detail)
				return NULL;
			const char *my_mid = my_square_member_id(w, chat);
			J *members = field(detail, "members");
			J *out = json_object_new_array();

			for (size_t i = 0;
			     json_object_is_type(members, json_type_array) &&
			     i < json_object_array_length(members);
			     i++) {
				J *m = json_object_array_get_idx(members, i);
				const char *mid = strfield(m, "id");
				const char *name = strfield(m, "name");

				if (!mid || !g_strcmp0(mid, my_mid) || !name)
					continue;
				if (q && *q &&
				    !g_str_match_string(q, name, TRUE))
					continue;
				json_object_array_add(out, json_object_get(m));
				if (json_object_array_length(out) >= 20)
					break;
			}
			return out;
		}
		if (*chat != 'c')
			return json_object_new_array();
		g_autoptr(J) detail = line_group_members(s, chat, error);

		if (!detail)
			return NULL;
		J *members = field(detail, "members");
		J *out = json_object_new_array();

		for (size_t i = 0;
		     json_object_is_type(members, json_type_array) &&
		     i < json_object_array_length(members);
		     i++) {
			J *m = json_object_array_get_idx(members, i);
			const char *mid = strfield(m, "id");
			const char *name = strfield(m, "name");

			if (!mid || !g_strcmp0(mid, s->mid) || !name)
				continue;
			if (q && *q && !g_str_match_string(q, name, TRUE))
				continue;
			json_object_array_add(out, json_object_get(m));
			if (json_object_array_length(out) >= 20)
				break;
		}
		return out;
	}
	if (!strcmp(method, "inviteGroup"))
		return change_group(w, chat, params, error);
	if (!strcmp(method, "groupLink"))
		return line_group_link(s, chat, error);
	if (!strcmp(method, "updateGroupPicture"))
		return line_group_picture_update(s, chat, path, error);
	if (!strcmp(method, "sharedList") || !strcmp(method, "sharedPost") ||
	    !strcmp(method, "sharedPhotos") ||
	    !strcmp(method, "sharedCreateAlbum") ||
	    !strcmp(method, "sharedRenameAlbum") ||
	    !strcmp(method, "sharedDeleteAlbum") ||
	    !strcmp(method, "sharedDeletePhoto") ||
	    !strcmp(method, "sharedUpdateNote") ||
	    !strcmp(method, "sharedDeleteNote") ||
	    !strcmp(method, "sharedToggleLike") ||
	    !strcmp(method, "sharedComments") ||
	    !strcmp(method, "sharedCommentPost") ||
	    !strcmp(method, "sharedCommentDelete"))
		return line_shared_content(s, method, params, error);
	if (!strcmp(method, "sharedPhoto") ||
	    (!strcmp(method, "saveFile") && strfield(params, "album"))) {
		g_autoptr(GBytes) image = line_shared_photo(s, params, error);

		return image ? line_media_lease(w->leases, image, error) : NULL;
	}
	gboolean kick = !strcmp(method, "removeGroupMember");
	gboolean cancel = !strcmp(method, "cancelGroupInvite");
	gboolean rename = !strcmp(method, "renameGroup");
	gboolean leave = !strcmp(method, "leaveGroup");
	gboolean set_link = !strcmp(method, "setGroupLink");
	gboolean renew_link = !strcmp(method, "renewGroupLink");

	if (kick || cancel || rename || leave || set_link || renew_link) {
		J *enabled = field(params, "enabled");

		if (set_link &&
		    !json_object_is_type(enabled, json_type_boolean))
			return problem(
				error,
				"Choose whether the group link is enabled.");
		gint32 seq = reserve_sequence(w, error);

		if (!seq)
			return NULL;
		if (kick || cancel)
			return line_group_remove(s, chat, id, cancel, seq,
						 error);
		if (rename)
			return line_group_rename(
				s, chat, strfield(params, "name"), seq, error);
		if (set_link)
			return line_group_set_link(
				s, chat, json_object_get_boolean(enabled), seq,
				error);
		if (renew_link)
			return line_group_renew_link(s, chat, seq, error);
		J *left = line_group_leave(s, chat, seq, error);

		if (left) {
			g_hash_table_remove(w->chats, chat);
			g_hash_table_add(w->left_groups, g_strdup(chat));
			json_object_object_del(w->opened_chats, chat);
			json_object_object_del(w->unread_heads, chat);
			drop_chat_archive(w, chat);
			clear_global_search(w);
		}
		return left;
	}
	if (!strcmp(method, "findMessages"))
		return find_messages(w, chat, params, error);
	if (!strcmp(method, "callStart")) {
		J *result = line_call_start(
			s, chat,
			json_object_get_boolean(field(params, "video")), error);
		if (result && call_group_id(chat)) {
			json_object_put(w->group_call_members);
			w->group_call_members = json_object_get(
				field(result, "participantMids"));
			json_object_object_del(result, "participantMids");
			if (w->group_call_members)
				json_object_object_add(
					result, "participants",
					group_roster(w, w->group_call_members));
			w->group_roster_due =
				g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
		}
		return result;
	}
	gboolean older = !strcmp(method, "olderHistory");

	if (!strcmp(method, "history") || older)
		return history_page(w, chat, older, error);
	gboolean selected = !g_strcmp0(chat, w->history_chat);
	J *msg = selected && id ? field(w->messages, id) : NULL;

	if (json_object_get_boolean(field(msg, "unsent")))
		msg = NULL;
	if (!strcmp(method, "forwardMessage"))
		return forward_message(w, strfield(params, "destination"), msg,
				       error);
	if (!strcmp(method, "announcements"))
		return announcement_list(w, chat, error);
	if (!strcmp(method, "removeAnnouncement"))
		return remove_announcement(w, chat, id, error);
	if (!strcmp(method, "announceMessage")) {
		if (!msg || json_object_get_int(field(msg, "15")) != 0)
			return problem(error,
				       "Choose a text message to announce.");
		g_autofree char *text = line_message_preview(s, msg, error);
		gint32 seq = text ? reserve_sequence(w, error) : 0;

		if (!seq)
			return NULL;
		if (line_is_square_chat(chat))
			return line_square_announce_message(
				s, chat, id, text, strfield(msg, "1"),
				json_object_get_int64(field(msg, "5")), seq,
				error);
		return line_announce_message(s, chat, id, text, seq, error);
	}
	if (!strcmp(method, "sendSticker")) {
		const char *reply_to = strfield(params, "replyTo");
		const char *pack = strfield(params, "packageId");
		const char *sticker = strfield(params, "stickerId");

		if (!w->stickers || !pack || !sticker)
			return problem(
				error,
				"Select a LINE sticker from your packs.");
		if (reply_to && (!selected || !field(w->messages, reply_to) ||
				 field(field(w->unsent, chat), reply_to)))
			return problem(error, "Select a message to reply to.");
		gint32 seq = reserve_sequence(w, error);

		if (!seq)
			return NULL;
		J *ack;

		if (line_is_square_chat(chat)) {
			const char *my_mid = my_square_member_id(w, chat);

			ack = line_square_send_sticker(s, w->stickers, chat,
						       my_mid, pack, sticker,
						       reply_to, seq, error);
			if (ack) {
				g_hash_table_remove(w->history_cache, chat);
				const char *sent_id = strfield(ack, "id");

				if (w->messages &&
				    !g_strcmp0(chat, w->history_chat) &&
				    sent_id && *sent_id && my_mid &&
				    !field(w->messages, sent_id)) {
					J *stub = obj();

					json_object_object_add(
						stub, "1",
						json_object_new_string(my_mid));
					json_object_object_add(
						stub, "4",
						json_object_new_string(
							sent_id));
					json_object_object_add(w->messages,
							       sent_id, stub);
					w->publish_reads = TRUE;
					w->need_ranges = TRUE;
				}
				if (w->square_poll_due)
					w->square_poll_due =
						g_get_monotonic_time() +
						1 * G_USEC_PER_SEC;
			}
			return ack;
		}
		ack = line_send_sticker(s, w->stickers, chat, pack, sticker,
					reply_to, seq, error);
		if (ack) {
			g_hash_table_remove(w->history_cache, chat);
			const char *sent_id = strfield(ack, "id");

			if (w->messages && !g_strcmp0(chat, w->history_chat) &&
			    sent_id && *sent_id && s->mid &&
			    !field(w->messages, sent_id)) {
				J *stub = obj();

				json_object_object_add(
					stub, "1",
					json_object_new_string(s->mid));
				json_object_object_add(
					stub, "4",
					json_object_new_string(sent_id));
				json_object_object_add(w->messages, sent_id,
						       stub);
				w->publish_reads = TRUE;
			}
		}
		return ack;
	}
	if (!strcmp(method, "markRead")) {
		if (w->offline)
			return problem(
				error,
				"Reconnect to LINE to mark messages as read.");
		if (!msg || !w->latest_history ||
		    g_strcmp0(id, strfield(w->unread_heads, chat)))
			return problem(
				error,
				"Refresh this conversation to update its read receipt.");
		J *last = field(w->read_acks, chat);

		if (last && !g_strcmp0(id, strfield(last, "id")))
			return json_object_get(last);
		J *ack;

		if (line_is_square_chat(chat)) {
			ack = line_square_mark_read(s, chat, id, error);
		} else {
			gint32 seq = reserve_sequence(w, error);

			ack = seq ? line_mark_read(s, chat, id, seq, error) :
				    NULL;
		}
		if (ack)
			json_object_object_add(w->read_acks, chat,
					       json_object_get(ack));
		return ack;
	}
	gboolean open_video = !strcmp(method, "openVideo");
	gboolean video_poster = !strcmp(method, "videoPoster");

	if (!strcmp(method, "openAudio")) {
		if (!msg ||
		    json_object_get_int(field(msg, "15")) != LINE_MEDIA_AUDIO)
			return problem(
				error,
				"Select an audio message from this conversation.");
		g_autoptr(GBytes) bytes =
			line_download_attachment(s, msg, error);

		return bytes ? line_media_lease(w->leases, bytes, error) : NULL;
	}
	if (open_video || video_poster) {
		if (!msg ||
		    json_object_get_int(field(msg, "15")) != LINE_MEDIA_VIDEO)
			return problem(
				error,
				"Select a video from this conversation.");
		/* Older offline Square rows may omit the room in field 2. */
		if (line_is_square_chat(chat) && !strfield(msg, "2"))
			json_object_object_add(msg, "2",
					       json_object_new_string(chat));
		g_autoptr(GBytes) bytes =
			video_poster ?
				line_download_video_preview(s, msg, error) :
				line_download_video(s, msg, error);

		if (!bytes)
			return NULL;
		return video_poster ?
			       line_media_lease(w->leases, bytes, error) :
			       line_media_video_lease(w->leases, bytes, error);
	}
	if (!strcmp(method, "photo") || !strcmp(method, "saveFile")) {
		if (!msg)
			return problem(
				error,
				"Reload this conversation to open the attachment.");
		g_autoptr(GBytes) bytes = NULL;
		const char *hosted = line_hosted_image_url(msg);

		if (json_object_get_int(field(msg, "15")) == 7) {
			g_autofree char *url =
				line_sticker_message_asset(msg, error);

			if (url)
				bytes = line_sticker_fetch(url, error);
		} else if (hosted) {
			/* An official account's picture, not an OBS object. */
			bytes = line_flex_image_download(hosted, error);
			if (!bytes && error && *error &&
			    !g_error_matches(*error, LINE_ERROR,
					     LINE_RATE_LIMITED)) {
				g_clear_error(error);
				problem(error,
					"Could not load this image from the official account.");
			}
		} else if (!strcmp(method, "photo"))
			bytes = line_download_image(s, msg, error);
		else
			bytes = line_download_attachment(s, msg, error);
		if (!bytes)
			return NULL;
		/*
		 * Both photo previews and saveFile return a read-only memfd
		 * lease over SCM_RIGHTS so the Landlock-confined worker never
		 * writes to user-chosen paths in $HOME.
		 */
		return line_media_lease(w->leases, bytes, error);
	}
	gboolean text_send = !strcmp(method, "send"),
		 photo = !strcmp(method, "sendPhoto"),
		 file = !strcmp(method, "sendFile");
	gboolean unsend = !strcmp(method, "unsend"),
		 react = !strcmp(method, "react");
	if (text_send || photo || file || unsend || react) {
		const char *text = strfield(params, "text"),
			   *reply_to = strfield(params, "replyTo"),
			   *reaction = strfield(params, "reaction");
		if (text_send &&
		    (!text || !*text || !g_utf8_validate(text, -1, NULL) ||
		     strlen(text) > 20000))
			return problem(
				error,
				"Enter a message of up to 20,000 UTF-8 bytes.");
		if (reply_to && (!selected || !field(w->messages, reply_to) ||
				 field(field(w->unsent, chat), reply_to)))
			return problem(error, "Select a message to reply to.");
		if ((react || unsend) && !msg)
			return problem(
				error,
				"Select a message in this conversation.");
		const char *owner_id = line_is_square_chat(chat) ?
					       my_square_member_id(w, chat) :
					       s->mid;

		if (unsend && g_strcmp0(strfield(msg, "1"), owner_id))
			return problem(
				error,
				"You can only unsend your own messages.");
		if (react && !reaction)
			return problem(error, "Choose a reaction.");
		if ((photo || file) && (!path || !g_path_is_absolute(path)))
			return problem(error, "Choose an attachment.");
		if (text_send && !line_is_square_chat(chat))
			return submit_send(w, chat, params, error);
		if (w->offline)
			return problem(
				error,
				"Reconnect to LINE to complete this action.");
		if (line_is_square_chat(chat)) {
			const char *my_mid = my_square_member_id(w, chat);
			J *ack;

			if (unsend)
				ack = line_square_unsend(s, chat, id, error);
			else {
				gint32 reserved = reserve_sequence(w, error);

				if (!reserved)
					return NULL;
				if (text_send)
					ack = line_square_send_text(
						s, chat, my_mid, text, reply_to,
						field(params, "emojis"),
						field(params, "mentions"),
						reserved, error);
				else if (photo)
					ack = line_send_square_attachment(
						s, chat, my_mid, path,
						LINE_MEDIA_IMAGE, reserved,
						error);
				else if (file) {
					const char *kind =
						strfield(params, "type");
					int type = !g_strcmp0(kind, "video") ?
							   LINE_MEDIA_VIDEO :
						   !g_strcmp0(kind, "audio") ?
							   LINE_MEDIA_AUDIO :
							   LINE_MEDIA_FILE;

					ack = line_send_square_attachment(
						s, chat, my_mid, path, type,
						reserved, error);
				} else
					ack = line_square_react(s, chat, id,
								reaction,
								reserved,
								error);
			}
			if (ack) {
				if (unsend)
					message_unsent(w, chat, id);
				g_hash_table_remove(w->history_cache, chat);
				if (!unsend && !react && w->messages &&
				    !g_strcmp0(chat, w->history_chat)) {
					const char *sent_id =
						strfield(ack, "id");

					if (sent_id && *sent_id && my_mid &&
					    !field(w->messages, sent_id)) {
						J *stub = obj();

						json_object_object_add(
							stub, "1",
							json_object_new_string(
								my_mid));
						json_object_object_add(
							stub, "4",
							json_object_new_string(
								sent_id));
						json_object_object_add(
							w->messages, sent_id,
							stub);
						w->publish_reads = TRUE;
						w->need_ranges = TRUE;
					}
				}
				if (w->square_poll_due)
					w->square_poll_due =
						g_get_monotonic_time() +
						1 * G_USEC_PER_SEC;
			}
			return ack;
		}
		gint32 reserved = reserve_sequence(w, error);

		if (!reserved)
			return NULL;
		s->seq--;
		J *ack;

		if (photo)
			ack = line_send_image(s, chat, path, error);
		else if (file) {
			const char *kind = strfield(params, "type");
			int type =
				!g_strcmp0(kind, "video") ? LINE_MEDIA_VIDEO :
				!g_strcmp0(kind, "audio") ? LINE_MEDIA_AUDIO :
							    LINE_MEDIA_FILE;
			ack = line_send_attachment(s, chat, path, type, error);
		} else
			ack = line_message_action(
				s, id, react ? reaction : NULL, error);
		s->seq = reserved;
		if (ack) {
			if (unsend)
				message_unsent(w, chat, id);
			g_hash_table_remove(w->history_cache, chat);
			if (!unsend && !react && w->messages &&
			    !g_strcmp0(chat, w->history_chat)) {
				const char *sent_id = strfield(ack, "id");

				if (sent_id && *sent_id && s->mid &&
				    !field(w->messages, sent_id)) {
					J *stub = obj();

					json_object_object_add(
						stub, "1",
						json_object_new_string(s->mid));
					json_object_object_add(
						stub, "4",
						json_object_new_string(
							sent_id));
					json_object_object_add(w->messages,
							       sent_id, stub);
					w->publish_reads = TRUE;
				}
			}
		}
		return ack;
	}
	return problem(error, "Unknown request method.");
}

/*
 * Optional CDN transfers and peer receipt lookups run off the dispatch
 * thread so they cannot hold up text sends. Only the dispatch thread
 * commits token rotations and allocates image leases.
 */
static struct image_job image_stop;

static void image_free(gpointer data)
{
	struct image_job *job = data;

	if (job == &image_stop)
		return;
	g_clear_pointer(&job->bytes, g_bytes_unref);
	g_clear_pointer(&job->ranges, json_object_put);
	g_free(job->read_chat);
	g_free(job->read_self);
	g_free(job->read_sync);
	g_free(job->read_next_sync);
	line_secret_free(job->access);
	line_secret_free(job->next_access);
	g_clear_error(&job->error);
	g_free(job->path);
	g_free(job->product);
	g_free(job->emoji);
	g_free(job);
}

static gpointer fetch_images(gpointer data)
{
	struct line_worker *w = data;

	line_tune_thread(LINE_THREAD_BACKGROUND);
	for (;;) {
		struct image_job *job = g_async_queue_pop(w->image_requests);

		if (job == &image_stop)
			break;
		if (job->read_chat) {
			Line session = { .access = g_strdup(job->access) };

			job->ranges = line_is_square_chat(job->read_chat) ?
					      line_square_peer_ranges(
						      &session, job->read_chat,
						      job->read_self,
						      job->read_sync,
						      &job->read_next_sync,
						      &job->read_changed,
						      &job->error) :
					      line_peer_ranges(&session,
							       job->read_chat,
							       &job->error);
			if (session.token_changed)
				job->next_access =
					g_steal_pointer(&session.access);
			line_secret_free(session.access);
		} else if (job->sticker_sound)
			job->bytes = line_sticker_sound_fetch(job->path,
							      &job->error);
		else if (job->sticker)
			job->bytes = line_sticker_fetch(job->path, &job->error);
		else if (job->flex)
			job->bytes = line_flex_image_download(job->path,
							      &job->error);
		else if (job->product)
			job->bytes = line_emoji_image_variant(job->product,
							      job->emoji,
							      job->animated,
							      &job->error);
		else
			job->bytes =
				line_avatar_download(job->path, &job->error);
		g_async_queue_push(w->image_results, job);
		while (eventfd_write(w->image_wake, 1) < 0 && errno == EINTR)
			;
	}
	line_transport_release();
	return NULL;
}

static gboolean background_ready(struct line_worker *w, GError **error)
{
	if (!w->image_thread)
		w->image_thread =
			g_thread_try_new("line-images", fetch_images, w, error);
	return w->image_thread != NULL;
}

/* Run once after each event batch; the visible history has already been sent. */
static void update_peer_reads(struct line_worker *w)
{
	if (w->offline || !w->publish_reads)
		return;
	w->publish_reads = FALSE;
	Line *s = &w->session;
	const char *chat = w->history_chat;
	gboolean square = line_is_square_chat(chat);
	const char *self_id = square ? my_square_member_id(w, chat) : s->mid;

	if (!chat || !self_id || !strcmp(chat, self_id) || !w->messages)
		return;
	g_autoptr(J) counts = line_peer_read_counts(
		self_id, chat, w->messages, w->peer_ranges, w->peer_notices);
	if (json_object_object_length(counts)) {
		g_autoptr(J) event = obj();
		json_object_object_add(event, "event",
				       json_object_new_string("message_reads"));
		json_object_object_add(event, "chat",
				       json_object_new_string(chat));
		json_object_object_add(event, "counts",
				       json_object_get(counts));
		line_emit(event);
	}
	if (!w->need_ranges || w->ranges_pending || !s->access ||
	    w->image_wake < 0 ||
	    (w->ranges_retry_at && g_get_monotonic_time() < w->ranges_retry_at))
		return;
	gboolean should_fetch = square;

	if (!should_fetch) {
		json_object_object_foreach(w->messages, id, msg) {
			if (g_strcmp0(strfield(msg, "1"), self_id) ||
			    (chat[0] == 'u' &&
			     json_object_get_int(field(counts, id)) > 0))
				continue;
			should_fetch = TRUE;
			break;
		}
	}
	if (!should_fetch)
		return;
	w->need_ranges = FALSE;
	if (!background_ready(w, NULL))
		return;
	struct image_job *job = g_new0(struct image_job, 1);

	job->read_chat = g_strdup(chat);
	if (square) {
		job->read_self = g_strdup(self_id);
		job->read_sync = g_strdup(w->square_read_sync);
	}
	job->access = g_strdup(s->access);
	job->generation = w->generation;
	w->ranges_pending = TRUE;
	/* Jump ahead of avatar/photo downloads so "Read" stays snappy. */
	g_async_queue_push_front(w->image_requests, job);
}

static gboolean queue_image(struct line_worker *w, J *request, J *sticker_msg,
			    const char *method, GError **error)
{
	gboolean emoji = !strcmp(method, "emoji");
	gboolean sticker_sound = !strcmp(method, "stickerSound");
	gboolean sticker = sticker_msg || !strcmp(method, "sticker");
	gboolean flex = !strcmp(method, "flexImage");
	J *params = field(request, "params");
	const char *id = strfield(params, "id");
	const char *path = id ? strfield(w->session.pictures, id) : NULL;
	const char *product = strfield(params, "productId");
	const char *entry = strfield(params, "emojiId");
	g_autofree char *sticker_url = NULL;

	if (!w->ready || w->image_count >= 4 || w->image_wake < 0) {
		problem(error, "Image download is unavailable.");
		return FALSE;
	}
	if (sticker_sound) {
		const char *chat = strfield(params, "chat");
		J *raw = chat && id && !g_strcmp0(chat, w->history_chat) &&
					 g_hash_table_contains(w->chats, chat) ?
				 field(w->messages, id) :
				 NULL;

		if (!raw) {
			problem(error,
				"Reload this conversation to play the sticker sound.");
			return FALSE;
		}
		sticker_url = line_sticker_message_sound(raw, error);
		if (!sticker_url)
			return FALSE;
	} else if (flex) {
		path = strfield(params, "url");
		if (!line_flex_image_allowed(path) ||
		    strlen(path) != (size_t)json_object_get_string_len(
					    field(params, "url"))) {
			problem(error, "Unsupported Flex image source.");
			return FALSE;
		}
	} else if (sticker_msg) {
		sticker_url = line_sticker_message_asset(sticker_msg, error);
		if (!sticker_url)
			return FALSE;
	} else if (sticker) {
		if (!w->stickers) {
			problem(error, "Open a LINE sticker pack first.");
			return FALSE;
		}
		sticker_url = line_sticker_asset(w->stickers,
						 strfield(params, "packageId"),
						 strfield(params, "stickerId"),
						 error);
		if (!sticker_url)
			return FALSE;
	} else if (emoji ? !line_emoji_id_valid(product, entry) : !path) {
		problem(error, emoji ? "LINE emoji is unavailable." :
				       "Profile picture is unavailable.");
		return FALSE;
	}
	if (!background_ready(w, error))
		return FALSE;
	struct image_job *job = g_new0(struct image_job, 1);

	job->path = sticker || sticker_sound ? g_steal_pointer(&sticker_url) :
					       g_strdup(path);
	job->sticker = sticker;
	job->sticker_sound = sticker_sound;
	job->flex = flex;
	job->product = emoji ? g_strdup(product) : NULL;
	job->emoji = emoji ? g_strdup(entry) : NULL;
	job->animated = emoji && !g_strcmp0(strfield(params, "resourceType"),
					    "ANIMATION");
	job->generation = w->generation;
	job->request_id = json_object_get_int64(field(request, "id"));
	w->image_count++;
	g_async_queue_push(w->image_requests, job);
	return TRUE;
}

static void save_rotated_token(struct line_worker *w);

static void finish_peer_ranges(struct line_worker *w, struct image_job *job)
{
	w->ranges_pending = FALSE;
	if (job->generation == w->generation) {
		if (job->next_access &&
		    !g_strcmp0(job->access, w->session.access)) {
			line_secret_set(&w->session.access,
					g_steal_pointer(&job->next_access));
			w->session.token_changed = TRUE;
			save_rotated_token(w);
		}
		if (!g_strcmp0(job->read_chat, w->history_chat)) {
			if (job->ranges) {
				if (job->read_next_sync) {
					g_free(w->square_read_sync);
					w->square_read_sync = g_steal_pointer(
						&job->read_next_sync);
				}
				if (job->read_sync && w->peer_ranges) {
					json_object_object_foreach(
						job->ranges, reader, entry) {
						J *prev = field(w->peer_ranges,
								reader);
						gint64 cur =
							json_object_get_int64(
								field(entry,
								      "2"));

						if (!prev ||
						    cur > json_object_get_int64(
								  field(prev,
									"2")))
							json_object_object_add(
								w->peer_ranges,
								reader,
								json_object_get(
									entry));
					}
				} else {
					g_clear_pointer(&w->peer_ranges,
							json_object_put);
					w->peer_ranges =
						g_steal_pointer(&job->ranges);
				}
				w->ranges_retry_at = 0;
				w->publish_reads = TRUE;
				if (job->read_changed) {
					g_hash_table_remove(w->history_cache,
							    job->read_chat);
					revalidate_history(w, job->read_chat);
				}
			} else {
				w->ranges_retry_at = g_get_monotonic_time() +
						     2 * G_USEC_PER_SEC;
				w->need_ranges = TRUE;
			}
		}
	}
	w->publish_reads |= w->need_ranges;
}

static void finish_images(struct line_worker *w)
{
	eventfd_t tick;

	while (eventfd_read(w->image_wake, &tick) < 0 && errno == EINTR)
		;
	struct image_job *job;

	while ((job = g_async_queue_try_pop(w->image_results))) {
		if (job->read_chat) {
			finish_peer_ranges(w, job);
			image_free(job);
			continue;
		}
		g_autoptr(J) reply = obj();
		J *result = NULL;

		if (w->generation == job->generation && job->bytes)
			result = line_media_lease(w->leases, job->bytes,
						  &job->error);
		json_object_object_add(reply, "id",
				       json_object_new_int64(job->request_id));
		if (result)
			json_object_object_add(reply, "result", result);
		else
			json_object_object_add(
				reply, "error",
				json_object_new_string(
					job->error ? job->error->message :
						     "Image expired."));
		emit_reply(w, reply);
		w->image_count--;
		image_free(job);
	}
}

static void handle(struct line_worker *w, const char *line)
{
	g_autoptr(J) req = json_tokener_parse(line), reply = obj();
	g_autoptr(GError) error = NULL;
	J *id = field(req, "id");
	const char *method = strfield(req, "method");
	J *params = field(req, "params");
	J *sticker_msg = NULL;
	J *value = NULL;

	if (w->ready && !g_strcmp0(method, "photo")) {
		const char *chat = strfield(params, "chat");
		const char *message = strfield(params, "id");

		if (chat && message && !g_strcmp0(chat, w->history_chat) &&
		    g_hash_table_contains(w->chats, chat)) {
			J *raw = field(w->messages, message);

			if (json_object_get_int(field(raw, "15")) == 7)
				sticker_msg = raw;
		}
	}
	if (!json_object_is_type(id, json_type_int))
		problem(&error, "Missing request ID.");
	else if (sticker_msg || !g_strcmp0(method, "avatar") ||
		 !g_strcmp0(method, "flexImage") ||
		 !g_strcmp0(method, "emoji") || !g_strcmp0(method, "sticker") ||
		 !g_strcmp0(method, "stickerSound")) {
		if (queue_image(w, req, sticker_msg, method, &error))
			return;
	} else {
		gint64 started = g_get_monotonic_time();

		value = dispatch(w, method, params, &error);
		if (!g_strcmp0(g_getenv("LINE_DEBUG_TIMING"), "1"))
			g_printerr(
				"line-timing %s %lld ms\n", method,
				(long long)((g_get_monotonic_time() - started) /
					    1000));
	}
	if (g_error_matches(error, LINE_ERROR, LINE_OBS_UNAUTHORIZED)) {
		line_secret_set(&w->session.obs_token, NULL);
		w->session.obs_until = 0;
	}
	if (w->session.token_changed && w->session.access && w->dir >= 0)
		save_rotated_token(w);
	json_object_object_add(reply, "id", json_object_get(id));
	if (value)
		json_object_object_add(reply, "result", value);
	else
		json_object_object_add(
			reply, "error",
			json_object_new_string(error ? error->message :
						       "Request failed."));
	line_reply_retry(reply, error, !g_strcmp0(method, "login"));
	emit_reply(w, reply);
	/* A request LINE answered with an auth refusal ends the session. */
	if (w->ready && w->session.denied) {
		end_session(w);
		return;
	}
	if (w->revalidate) {
		g_autofree char *chat = g_steal_pointer(&w->revalidate);

		revalidate_history(w, chat);
	}
}

/*
 * A signed-out worker blocks indefinitely. A signed-in worker checks the
 * server revision between interactive requests, with a one-shot timerfd.
 */
static gboolean apply_operation(struct line_worker *w, J *op)
{
	Line *s = &w->session;
	const char *chat, *id;

	if (line_unsend_operation(op, &chat, &id)) {
		message_unsent(w, chat, id);
		return TRUE;
	}
	if (json_object_get_int(field(op, "3")) == 30) {
		/* RECEIVE_ANNOUNCEMENT param1 is the chat MID. */
		chat = strfield(op, "10");
		if (chat && !g_strcmp0(chat, w->history_chat))
			line_event("announcements_changed", "chat", chat);
		return TRUE;
	}
	enum peer_notice notice = line_peer_read_notice(s->mid, w->history_chat,
							w->peer_notices, op);

	if (notice == PEER_NOTICE_ADDED)
		w->publish_reads = w->need_ranges = TRUE;
	if (notice != PEER_NOTICE_OTHER)
		return TRUE;
	line_profile_operation(s, op);
	line_voice_operation(s, op);
	return FALSE;
}

static void poll_operation(J *op, gpointer data)
{
	struct line_worker *w = data;
	J *message = field(op, "20");
	const char *chat = strfield(message, "2");

	if (apply_operation(w, op))
		return;
	if (chat && json_object_get_int(field(op, "3")) == 25)
		reconcile_message(w, chat, message);
}

static gboolean poll_call_operations(struct line_worker *w, gboolean *reload)
{
	g_autoptr(J) args = line_talk_sync_request(&w->sync);
	g_autoptr(GError) ignored = NULL;
	g_autoptr(J) result = line_call(&w->session, "/SYNC4", "sync", args,
					w->session.access, 2, &ignored);

	if (!result)
		return FALSE;
	return line_talk_sync_apply(&w->sync, result, poll_operation, w,
				    reload);
}

/*
 * Renew tokens when LINE's schedule says so, before the access token
 * expires. A regularly used refresh token keeps the desktop signed in.
 * A failed scheduled renewal never signs out: the next RPC still takes the
 * explicit expiry path, and only LINE's rejection ends the session there.
 */
static void maintain_tokens(struct line_worker *w)
{
	Line *s = &w->session;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 mono = g_get_monotonic_time();

	if (mono < w->refresh_retry || !line_refresh_due(s, now))
		return;
	int cooldown = line_transport_retry_after();
	if (cooldown) {
		w->refresh_retry = mono + (gint64)cooldown * G_USEC_PER_SEC;
		return;
	}
	g_autoptr(GError) error = NULL;
	gboolean debug = !g_strcmp0(g_getenv("LINE_DEBUG_SESSION"), "1");

	if (line_refresh_session(s, &error)) {
		w->refresh_failures = 0;
		w->refresh_retry = 0;
		/* A successful response can still carry a past issue time. */
		if (line_refresh_due(s, now))
			w->refresh_retry =
				g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
		if (debug)
			g_printerr(
				"line-session renewed; next renewal in %lld s\n",
				(long long)(s->refresh_at - now));
		return;
	}
	w->refresh_failures = MIN(w->refresh_failures + 1, 16u);
	/* 2, 4, 8 ... minutes, then hourly, until LINE answers. */
	gint64 wait = line_retry_delay(w->refresh_failures, 120, 3600);

	w->refresh_retry = mono + wait * G_USEC_PER_SEC;
	if (debug)
		g_printerr(
			"line-session renewal failed (%s); retrying in %lld s\n",
			error ? error->message : "unknown", (long long)wait);
}

static gboolean save_vault_token(struct line_worker *w, GError **error)
{
	const Line *s = &w->session;

	if (!ensure_vault(w, error))
		return FALSE;
	if (avernella_vault_client_rotate_token(
		    w->vault_fd, s->access, s->refresh, s->refresh_at, error))
		return TRUE;
	/* Persisting the same token is idempotent. */
	if (retry_vault(w, error) &&
	    avernella_vault_client_rotate_token(
		    w->vault_fd, s->access, s->refresh, s->refresh_at, error))
		return TRUE;
	if (avernella_vault_disconnected(*error))
		drop_vault(w);
	return FALSE;
}

static void save_rotated_token(struct line_worker *w)
{
	Line *s = &w->session;

	if (s->access && s->token_changed) {
		g_autoptr(GError) storage_error = NULL;
		gboolean saved =
			w->vault_has_keys ?
				save_vault_token(w, &storage_error) :
				line_store_save(w->dir, s, &storage_error);

		if (!saved) {
			w->save_retry =
				g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
			line_event("status", "message", storage_error->message);
		} else {
			s->token_changed = FALSE;
			w->save_retry = 0;
		}
	}
	line_live_sync_set_access(w->live, s->access);
}

static gint64 earlier_deadline(gint64 first, gint64 second)
{
	return !first || (second && second < first) ? second : first;
}

static gint64 maintain_group_roster(struct line_worker *w)
{
	const char *chat = line_call_group_chat(FALSE);

	if (!chat || !w->group_roster_due)
		return 0;
	gint64 now = g_get_monotonic_time();

	if (now < w->group_roster_due)
		return w->group_roster_due;
	if (!line_call_group_chat(TRUE))
		return now + G_USEC_PER_SEC;
	int cooldown = line_transport_retry_after();

	if (cooldown) {
		w->group_roster_due = now + (gint64)cooldown * G_USEC_PER_SEC;
		return w->group_roster_due;
	}
	Line *s = &w->session;
	g_autoptr(GError) error = NULL;
	g_autoptr(J) ids = call_group_members(s, chat, &error);

	if (s->denied) {
		end_session(w);
		return 0;
	}
	if (!line_call_group_chat(TRUE))
		return 0;
	if (g_error_matches(error, LINE_ERROR, LINE_ACCESS_DENIED) ||
	    (error && !g_error_matches(error, LINE_ERROR, LINE_RATE_LIMITED) &&
	     !transient_failure(s, error))) {
		w->group_roster_due = 0;
		return 0;
	}
	if (ids && !json_object_equal(ids, w->group_call_members)) {
		g_autoptr(J) packet = obj();
		J *roster = group_roster(w, ids);

		if (s->denied) {
			json_object_put(roster);
			end_session(w);
			return 0;
		}
		if (!line_call_group_chat(TRUE)) {
			json_object_put(roster);
			return 0;
		}
		json_object_object_add(
			packet, "event",
			json_object_new_string("call_participants"));
		json_object_object_add(packet, "chat",
				       json_object_new_string(chat));
		json_object_object_add(packet, "participants", roster);
		json_object_put(w->group_call_members);
		w->group_call_members = g_steal_pointer(&ids);
		line_emit(packet);
	}
	if (s->token_changed) {
		line_live_sync_set_access(w->live, s->access);
		save_rotated_token(w);
	}
	w->group_roster_due = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	return w->group_roster_due;
}

/*
 * With healthy long polling the eventfd announces changes. Renewals use
 * LINE's absolute Unix time so suspend and wall-clock changes are respected.
 * Invitation expiry and retry intervals use elapsed monotonic time.
 */
/*
 * Gently poll SquareService.fetchMyEvents when the user belongs to at least
 * one OpenChat. Respects Retry-After and slows down when unfocused.
 */
static gint64 maintain_square_sync(struct line_worker *w)
{
	if (!w->ready || w->square_disabled || !w->squares ||
	    !json_object_object_length(w->squares) || !w->square_poll_due)
		return 0;
	gint64 now = g_get_monotonic_time();

	if (now < w->square_poll_due)
		return w->square_poll_due;
	int cooldown = line_transport_retry_after();

	if (cooldown) {
		w->square_poll_due = now + (gint64)cooldown * G_USEC_PER_SEC;
		return w->square_poll_due;
	}
	g_autoptr(GError) error = NULL;
	g_autoptr(J) events = line_square_poll_events(
		&w->session, &w->square_sub_id, &w->square_sync_token,
		w->square_members, &error);
	if (!events) {
		if (g_error_matches(error, LINE_ERROR,
				    LINE_METHOD_UNAVAILABLE)) {
			w->square_disabled = TRUE;
			w->square_poll_due = 0;
			return 0;
		}
		w->square_poll_failures = MIN(w->square_poll_failures + 1, 4u);
		int backoff = MIN(2 << (w->square_poll_failures - 1), 15);

		w->square_poll_due = now + (gint64)backoff * G_USEC_PER_SEC;
		return w->square_poll_due;
	}
	w->square_poll_failures = 0;
	gboolean reload = FALSE;
	g_autofree char *open_chat = NULL;
	g_autoptr(GHashTable) messaged =
		g_hash_table_new(g_str_hash, g_str_equal);

	for (size_t i = 0; i < json_object_array_length(events); i++) {
		J *ev = json_object_array_get_idx(events, i);
		const char *chat = strfield(ev, "chat");

		if (!g_strcmp0(strfield(ev, "kind"), "message") && chat)
			g_hash_table_add(messaged, (gpointer)chat);
	}
	for (size_t i = 0; i < json_object_array_length(events); i++) {
		J *ev = json_object_array_get_idx(events, i);
		const char *kind = strfield(ev, "kind");
		const char *chat = strfield(ev, "chat");

		if (!g_strcmp0(kind, "announcements") && chat) {
			if (!g_strcmp0(chat, w->history_chat))
				line_event("announcements_changed", "chat",
					   chat);
			continue;
		}
		if (!g_strcmp0(kind, "message") && chat) {
			const char *mid = strfield(ev, "id");
			gboolean mine =
				json_object_get_boolean(field(ev, "mine"));
			struct cached_history *cached =
				g_hash_table_lookup(w->history_cache, chat);

			if (cached)
				cached->stale = TRUE;
			if (!g_strcmp0(chat, w->history_chat)) {
				g_free(open_chat);
				open_chat = g_strdup(chat);
			} else {
				g_hash_table_remove(w->history_cache, chat);
			}
			if (mid && *mid) {
				J *previous = field(w->unread_heads, chat);
				J *summary =
					g_hash_table_lookup(w->chats, chat);
				gint64 known =
					MAX(json_object_get_int64(previous),
					    json_object_get_int64(
						    field(summary, "lastId")));
				gint64 incoming =
					g_ascii_strtoll(mid, NULL, 10);

				if (w->unread_heads && (!mine || previous) &&
				    incoming > known)
					json_object_object_add(
						w->unread_heads, chat,
						json_object_new_string(mid));
			}
			g_autoptr(J) pkt = obj();

			json_object_object_add(
				pkt, "event",
				json_object_new_string("chat_message"));
			json_object_object_add(pkt, "chat",
					       json_object_new_string(chat));
			if (mid)
				json_object_object_add(
					pkt, "id", json_object_new_string(mid));
			const char *preview = strfield(ev, "preview");

			if (preview && *preview)
				json_object_object_add(
					pkt, "preview",
					json_object_new_string(preview));
			json_object_object_add(
				pkt, "time",
				json_object_get(field(ev, "time")));
			json_object_object_add(pkt, "mine",
					       json_object_new_boolean(mine));
			line_emit(pkt);
		} else if (chat && g_hash_table_contains(messaged, chat)) {
			/* Chat status update paired with a message we already handled. */
			continue;
		} else {
			if (chat && !g_strcmp0(chat, w->history_chat)) {
				g_free(open_chat);
				open_chat = g_strdup(chat);
			}
			reload = TRUE;
		}
	}
	if (open_chat)
		revalidate_history(w, open_chat);
	if (reload)
		line_event("changed", NULL, NULL);
	if (w->history_chat && line_is_square_chat(w->history_chat) &&
	    !w->ranges_pending) {
		w->need_ranges = TRUE;
		w->publish_reads = TRUE;
	}
	if (w->session.token_changed)
		save_rotated_token(w);
	gint64 delay_us;

	if (json_object_array_length(events) >= 50)
		delay_us = 200 * 1000;
	else if (w->foreground && w->history_chat &&
		 line_is_square_chat(w->history_chat))
		delay_us = 1 * G_USEC_PER_SEC;
	else if (w->foreground)
		delay_us = 2 * G_USEC_PER_SEC;
	else
		delay_us = 5 * G_USEC_PER_SEC;
	w->square_poll_due = g_get_monotonic_time() + delay_us;
	return w->square_poll_due;
}

static gint64 maintain_outbox(struct line_worker *w)
{
	if (!w->ready || w->offline || !w->online_chats || !w->outbox ||
	    !w->outbox_due)
		return 0;
	gint64 now = g_get_monotonic_time();

	if (now < w->outbox_due)
		return w->outbox_due;
	int cooldown = line_transport_retry_after();

	if (cooldown) {
		w->outbox_due = now + (gint64)cooldown * G_USEC_PER_SEC;
		return w->outbox_due;
	}
	for (size_t i = 0; i < json_object_array_length(w->outbox); i++) {
		g_autoptr(J) entry = json_object_get(
			json_object_array_get_idx(w->outbox, i));
		const char *id = strfield(entry, "intentId");
		gboolean queued =
			!g_strcmp0(strfield(entry, "status"), "queued");

		if (!queued &&
		    !g_hash_table_add(w->checked_sends, g_strdup(id)))
			continue;
		w->outbox_due = now + G_USEC_PER_SEC;
		g_autoptr(GError) error = NULL;
		g_autoptr(J) receipt = check_send(w, entry, &error);

		if (!receipt)
			send_outcome(entry, "failed", NULL, error);
		else if (!field(receipt, "pending"))
			send_outcome(entry, "sent", receipt, NULL);
		else if (queued)
			send_outcome(receipt, strfield(receipt, "status"), NULL,
				     NULL);
		if (w->session.denied) {
			end_session(w);
			return 0;
		}
		return w->offline ? 0 : w->outbox_due;
	}
	w->outbox_due = 0;
	return 0;
}

static int maintenance_wait(struct line_worker *w)
{
	Line *s = &w->session;
	gint64 next = line_voice_tick(), renewal = 0;

	next = earlier_deadline(next, maintain_group_roster(w));
	if (!w->offline)
		next = earlier_deadline(next, maintain_square_sync(w));
	next = earlier_deadline(next, maintain_outbox(w));
	if (!w->offline && w->live && !w->live_failing && s->access) {
		gint64 now = g_get_monotonic_time();
		gint64 wall = g_get_real_time() / G_USEC_PER_SEC;

		if (line_refresh_due(s, wall)) {
			maintain_tokens(w);
			next = earlier_deadline(next, w->refresh_retry);
		}
		if (s->refresh && *s->refresh && s->refresh_at > wall)
			renewal = s->refresh_at;
		if (s->token_changed && now >= w->save_retry)
			save_rotated_token(w);
		if (s->token_changed)
			next = earlier_deadline(next, w->save_retry);
	}
	if (renewal != w->armed_renewal) {
		struct itimerspec timer = { .it_value.tv_sec = renewal };
		if (w->renewal_timer >= 0 &&
		    !timerfd_settime(w->renewal_timer, TFD_TIMER_ABSTIME,
				     &timer, NULL))
			w->armed_renewal = renewal;
		else if (renewal)
			next = earlier_deadline(next,
						g_get_monotonic_time() +
							2 * G_USEC_PER_SEC);
	}
	if (!next)
		return -1;
	return CLAMP((next - g_get_monotonic_time() + 999) / 1000, 0, G_MAXINT);
}

/*
 * SEND_MESSAGE (25) and RECEIVE_MESSAGE (26) carry the message itself.
 * Tell the window which chat moved and mark its cached page stale.
 */
static gboolean message_operation(struct line_worker *w, J *op,
				  char **open_chat)
{
	gint64 type = json_object_get_int64(field(op, "3"));
	J *msg = field(op, "20");
	const char *from = strfield(msg, "1"), *to = strfield(msg, "2"),
		   *id = strfield(msg, "4");

	if ((type != 25 && type != 26) || !from || !to || !id)
		return FALSE;
	gboolean mine = !g_strcmp0(from, w->session.mid);
	/* One-to-one chats are named after the other person. */
	const char *chat =
		json_object_get_int(field(msg, "3")) == 0 && !mine ? from : to;

	if (field(field(w->unsent, chat), id))
		return TRUE;
	if (mine && type == 25)
		reconcile_message(w, chat, msg);
	g_autoptr(J) event = obj();

	json_object_object_add(event, "event",
			       json_object_new_string("chat_message"));
	json_object_object_add(event, "chat", json_object_new_string(chat));
	json_object_object_add(event, "id", json_object_new_string(id));
	const char *preview = strfield(msg, "10");

	if (preview && *preview)
		json_object_object_add(event, "preview",
				       json_object_new_string(preview));
	gint64 time = json_object_get_int64(field(msg, "5"));

	if (time <= 0)
		time = json_object_get_int64(field(msg, "6"));
	if (time <= 0)
		time = json_object_get_int64(field(op, "2"));
	json_object_object_add(event, "time", json_object_new_int64(time));
	json_object_object_add(event, "mine", json_object_new_boolean(mine));
	line_emit(event);
	/* Keep the accepted receipt target monotonic across sync replays. */
	J *previous = field(w->unread_heads, chat);
	J *summary = g_hash_table_lookup(w->chats, chat);
	gint64 known = MAX(json_object_get_int64(previous),
			   json_object_get_int64(field(summary, "lastId")));

	if (w->unread_heads && (!mine || previous) &&
	    json_object_get_int64(field(msg, "4")) > known)
		json_object_object_add(w->unread_heads, chat,
				       json_object_new_string(id));
	if (!g_hash_table_contains(w->chats, chat))
		g_hash_table_insert(w->chats, g_strdup(chat), NULL);
	struct cached_history *c = g_hash_table_lookup(w->history_cache, chat);

	if (c)
		c->stale = TRUE;
	if (!g_strcmp0(chat, w->history_chat)) {
		g_free(*open_chat);
		*open_chat = g_strdup(chat);
	} else {
		/* Drop it so the next open fetches fresh rather than old. */
		g_hash_table_remove(w->history_cache, chat);
	}
	return TRUE;
}

/*
 * Friend and profile operations patch the window in place, as Discord
 * updates a single user, instead of reloading every chat. Param 1 names
 * the person. Returns FALSE for operations this does not cover.
 */
static gboolean social_operation(struct line_worker *w, J *op)
{
	Line *s = &w->session;
	const char *who = strfield(op, "10"), *event;

	switch (json_object_get_int64(field(op, "3"))) {
	case 1: /* UPDATE_PROFILE, from this or another device */
		event = "self";
		break;
	case 5: /* NOTIFIED_ADD_CONTACT */
		event = "friend_added";
		break;
	case 88: /* NOTIFIED_FRIEND_REQUEST */
		event = "friend_request";
		break;
	case 2: /* NOTIFIED_UPDATE_PROFILE */
	case 4: /* ADD_CONTACT */
	case 6: /* BLOCK_CONTACT */
	case 7: /* UNBLOCK_CONTACT */
	case 49: /* UPDATE_CONTACT */
	case 63: /* FRIEND_REQUEST_ACCEPTED */
	case 78: /* NOTIFIED_BUDDY_UPDATE_PROFILE */
	case 141: /* NOTIFIED_UPDATE_PROFILE_CONTENT */
		event = "contact";
		break;
	default:
		return FALSE;
	}
	g_autoptr(J) packet = obj();
	g_autoptr(GError) ignored = NULL;
	J *row = NULL;

	json_object_object_add(packet, "event", json_object_new_string(event));
	if (!strcmp(event, "self")) {
		row = line_own_profile(s, &ignored);
		if (row && strfield(row, "name")) {
			g_free(w->name);
			w->name = g_strdup(strfield(row, "name"));
		}
	} else if (who && *who == 'u' && g_strcmp0(who, s->mid)) {
		row = line_friend_card(s, who, &ignored);
		if (row)
			remember_card(w, row);
	}
	if (row)
		json_object_object_add(packet, "row", row);
	else if (strcmp(event, "friend_request"))
		return TRUE;
	line_emit(packet);
	return TRUE;
}

/* A revision probe and the long poll can confirm the same recovery. */
static void reconnected(struct line_worker *w)
{
	w->recovered_at = g_get_monotonic_time();
	w->route_probe = FALSE;
	w->sync_failures = 0;
	w->offline = FALSE;
	g_hash_table_remove_all(w->checked_sends);
	w->outbox_due = g_get_monotonic_time();
	line_event("reconnected", NULL, NULL);
}

/* Operations from the long-poll thread: apply them and refresh at once. */
static void live_operations(struct line_worker *w)
{
	struct line_live_batch batch;
	Line *s = &w->session;

	line_live_sync_take(w->live, &batch);
	if (batch.access && s->access &&
	    !g_strcmp0(batch.access_from, s->access) &&
	    strcmp(batch.access, s->access)) {
		line_secret_set(&s->access, g_steal_pointer(&batch.access));
		s->token_changed = TRUE;
		save_rotated_token(w);
	}
	line_secret_free(batch.access);
	line_secret_free(batch.access_from);
	if (batch.denied) {
		/*
		 * The long poll's token was refused. Confirm once with getProfile,
		 * as LINE's client does: a renewed session restarts the long poll
		 * from the maintenance timer, a refused one signs out.
		 */
		json_object_put(batch.ops);
		g_clear_pointer(&w->live, line_live_sync_stop);
		w->live_failing = FALSE;
		w->live_failing_since = 0;
		g_autoptr(GError) error = NULL;
		g_autoptr(J) profile = checked_profile(s, &error);

		if (!profile && session_rejected(s, error))
			end_session(w);
		else
			wake_after(w, profile			  ? 1 :
				      transient_failure(s, error) ? 15 :
								    300);
		return;
	}
	size_t count = batch.ops ? json_object_array_length(batch.ops) : 0;
	gboolean other = batch.reload;
	g_autofree char *open_chat = NULL;
	guint cards = 0;

	for (size_t i = 0; i < count; i++) {
		J *op = json_object_array_get_idx(batch.ops, i);

		if (apply_operation(w, op))
			continue;
		if (message_operation(w, op, &open_chat))
			continue;
		if (cards < LIVE_CARD_LIMIT && social_operation(w, op))
			cards++;
		else
			other = TRUE;
	}
	json_object_put(batch.ops);
	if (batch.reload)
		w->publish_reads = w->need_ranges = TRUE;
	if (batch.reload && s->profiles)
		g_hash_table_remove_all(s->profiles);
	w->sync.revision = MAX(w->sync.revision, batch.revision);
	if (count || batch.reload || batch.recovered_since)
		w->displayed_revision =
			MAX(w->displayed_revision, batch.revision);
	/*
	 * New messages update the chat list in the window directly and the
	 * open chat refetches its page; anything else reloads both.
	 */
	if (batch.recovered_since > w->recovered_at)
		reconnected(w);
	else {
		if (open_chat)
			revalidate_history(w, open_chat);
		if (other)
			line_event("changed", NULL, NULL);
	}
	if (batch.failing && !w->live_failing) {
		w->live_failing_since = g_get_monotonic_time();
		wake_after(w, 1);
	}
	if (!batch.failing)
		w->live_failing_since = 0;
	w->live_failing = batch.failing;
}

/*
 * Seconds until the next fallback revision check. A short long-poll outage
 * is covered at once; a long one is checked less often, so a client whose
 * long poll keeps failing never keeps up a one-second probe for hours.
 */
static int fallback_interval(const struct line_worker *w, gboolean catch_up)
{
	gint64 since = w->live_failing_since;

	if (catch_up)
		return 1;
	if (since && g_get_monotonic_time() - since >= 120 * G_USEC_PER_SEC)
		return 15;
	return w->foreground ? 1 : 3;
}

static void sync_revision(struct line_worker *w)
{
	guint64 tick;

	if (!w->ready || read(w->timer, &tick, sizeof(tick)) != sizeof(tick))
		return;
	if (w->live && !w->offline && !w->live_failing && !w->route_probe)
		return;
	Line *s = &w->session;
	int wait = line_transport_retry_after();

	if (wait) {
		wake_after(w, wait);
		return;
	}

	g_autoptr(GError) error = NULL;
	g_autoptr(J) revision =
		line_rpc(s, "/S4", "getLastOpRevision", NULL, &error);
	if (revision && w->verify_on_reconnect) {
		g_autoptr(J) profile = checked_profile(s, &error);

		if (profile) {
			if (g_strcmp0(s->mid, strfield(profile, "1"))) {
				end_session(w);
				return;
			}
			w->verify_on_reconnect = FALSE;
		} else
			g_clear_pointer(&revision, json_object_put);
	}
	if (json_object_is_type(revision, json_type_int) &&
	    json_object_get_int64(revision) >= 0) {
		gint64 current = json_object_get_int64(revision);
		gboolean reload = FALSE, catch_up = FALSE;

		if (w->sync.revision < 0) {
			/*
			 * Establish the start cursor once. Account history before sign-in
			 * must never create an incoming call on this desktop.
			 */
			w->sync.revision = current;
		} else if ((!w->live || w->live_failing) &&
			   (w->sync.more || current > w->sync.revision)) {
			/* A revision probe reports activity, never an acknowledged page. */
			catch_up = poll_call_operations(w, &reload);
			if (reload)
				w->publish_reads = w->need_ranges = TRUE;
			if (reload && s->profiles)
				g_hash_table_remove_all(s->profiles);
		}
		current = MAX(current, w->sync.revision);
		if (!w->live && s->access)
			w->live =
				line_live_sync_start(&w->sync, s->access, NULL);
		if (w->offline || w->route_probe || w->sync_failures)
			reconnected(w);
		else if (reload || (w->displayed_revision >= 0 &&
				    current > w->displayed_revision))
			line_event("changed", NULL, NULL);
		w->displayed_revision = MAX(w->displayed_revision, current);
		if (!w->live || w->live_failing)
			wake_after(w, fallback_interval(w, catch_up));
	} else if (session_rejected(s, error)) {
		end_session(w);
	} else {
		if (error && transient_failure(s, error))
			went_offline(w);
		if (w->displayed_revision != -2)
			line_event(
				"sync_unavailable", "message",
				"Automatic chat refresh is unavailable. Use Refresh to try again.");
		w->displayed_revision = -2;
		w->sync_failures = MIN(w->sync_failures + 1, 5u);
		wake_after(w, MIN(3 << (w->sync_failures - 1), 30));
	}
	if (w->ready && !w->offline && s->access)
		maintain_tokens(w);
	save_rotated_token(w);
}

static void handle_wire_line(char *text, int fd, gpointer data)
{
	struct line_worker *w = data;

	if (fd >= 0)
		line_media_stage_fd(fd);
	handle(w, text);
	line_media_clear_staged_fd();
	g_free(text);
}

static int worker(int credentials_fd)
{
	/*
	 * Also release the TLS connection during a pending QR approval if the
	 * compositor kills the UI. The second check closes the parent-death race.
	 */
	pid_t parent = getppid();

	if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent ||
	    parent == 1)
		return 1;
	int frame_fd = LINE_CREDENTIAL_FD + (credentials_fd >= 0);
	int socket_type;
	socklen_t size = sizeof(socket_type);

	if (!getsockopt(frame_fd, SOL_SOCKET, SO_TYPE, &socket_type, &size) &&
	    socket_type == SOCK_SEQPACKET) {
		fcntl(frame_fd, F_SETFD, FD_CLOEXEC);
		line_call_frame_socket(frame_fd);
	} else {
		frame_fd = -1;
	}
	/* Strip any stray inherited FDs before spawning Vault & sandboxing. */
	int last_fd = MAX(frame_fd, MAX(credentials_fd, STDERR_FILENO));
	int err = avernella_close_extra_fds(last_fd);

	if (err < 0) {
		g_printerr(
			"Could not close inherited worker descriptors (%s).\n",
			g_strerror(-err));
		return 1;
	}
	/* The keyring helper exits before the worker and Vault are confined. */
	guint8 key[LINE_STATE_KEY_SIZE];
	gboolean have_key = FALSE;

	if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0)) {
		g_printerr("Avernella could not disable worker dumps: %s\n",
			   g_strerror(errno));
		return 1;
	}
#if LINE_HAVE_LIBSECRET
	have_key = line_keyring_key(key);
#endif
	gboolean store_ready = line_store_start(have_key ? key : NULL);

	explicit_bzero(key, sizeof(key));
	if (!store_ready)
		return 1;
	/*
	 * Spawn the isolated Key Vault broker BEFORE installing
	 * the worker's Landlock/seccomp sandbox and before any thread starts.
	 */
	struct avernella_worker_proc broker = { .pid = -1,
						.pidfd = -1,
						.ctrl_fd = -1 };

	{
		g_autofree char *state_dir = g_build_filename(
			g_get_user_state_dir(), LINE_STATE_DIR, NULL);

		(void)avernella_vault_broker_start(&broker, state_dir);
	}
	/* Before any thread starts, so every later thread inherits it. */
	line_sandbox_worker();
	struct line_worker w = {
		.dir = -1,
		.credentials_fd = credentials_fd,
		.vault_broker = broker,
		.vault_fd = -1,
		.sync.revision = -1,
		.displayed_revision = -1,
		.renewal_timer = timerfd_create(CLOCK_REALTIME,
						TFD_CLOEXEC | TFD_NONBLOCK),
		.timer = timerfd_create(CLOCK_MONOTONIC,
					TFD_CLOEXEC | TFD_NONBLOCK)
	};
	w.image_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	w.image_requests = g_async_queue_new_full(image_free);
	w.image_results = g_async_queue_new_full(image_free);
	w.leases = line_leases_new();
	w.chats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	w.history_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						cached_history_free);
	w.checked_sends =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_queue_init(&w.history_lru);
	w.left_groups =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	w.square_history_tokens =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	struct line_wire_reader wire_in;

	line_wire_reader_init(&wire_in);
	struct pollfd fds[] = { { .fd = STDIN_FILENO, .events = POLLIN },
				{ .fd = w.timer, .events = POLLIN },
				{ .fd = w.image_wake, .events = POLLIN },
				{ .fd = -1, .events = POLLIN },
				{ .fd = w.renewal_timer, .events = POLLIN },
				{ .fd = line_route_open(), .events = POLLIN } };
	for (;;) {
		int timeout = maintenance_wait(&w);

		/* Dispatch any read-receipt fetches scheduled by maintenance
		 * before blocking in poll(). */
		update_peer_reads(&w);
		fds[3].fd = line_live_sync_fd(w.live);
		if (poll(fds, G_N_ELEMENTS(fds), timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[0].revents) {
			static char block[64 * 1024];
			ssize_t n = line_wire_read(&wire_in, STDIN_FILENO,
						   block, sizeof(block),
						   handle_wire_line, &w);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			if (wire_in.input && wire_in.input->len > 1024 * 1024)
				break;
		}
		if (fds[2].revents & POLLIN)
			finish_images(&w);
		if ((fds[5].revents & (POLLIN | POLLERR)) &&
		    line_route_restored(fds[5].fd, &w.route_lost) && w.ready &&
		    !w.route_probe) {
			/* A long poll can survive a brief link loss. */
			w.route_probe = TRUE;
			wake_after(&w, 1);
		}
		if (fds[3].revents & POLLIN && w.live)
			live_operations(&w);
		if (fds[0].revents || fds[3].revents)
			line_live_sync_set_access(w.live, w.session.access);
		if (fds[1].revents & POLLIN)
			sync_revision(&w);
		if (fds[4].revents & POLLIN) {
			guint64 tick;
			if (read(w.renewal_timer, &tick, sizeof(tick)) ==
			    sizeof(tick))
				w.armed_renewal = 0;
		}
		update_peer_reads(&w);
	}
	if (fds[5].fd >= 0)
		close(fds[5].fd);
	line_wire_reader_clear(&wire_in);
	line_media_clear_staged_fd();
	line_voice_close();
	g_clear_pointer(&w.live, line_live_sync_stop);
	if (w.image_thread) {
		g_async_queue_push_front(w.image_requests, &image_stop);
		g_thread_join(w.image_thread);
	}
	g_async_queue_unref(w.image_requests);
	g_async_queue_unref(w.image_results);
	if (w.image_wake >= 0)
		close(w.image_wake);
	clear_view(&w);
	line_sealing_set_vault(NULL, NULL);
	drop_vault(&w);
	avernella_worker_close(&w.vault_broker);
	if (frame_fd >= 0)
		close(frame_fd);
	g_hash_table_unref(w.leases);
	if (w.dir >= 0) {
		line_cooldown_use_store(-1);
		close(w.dir);
	}
	if (w.credentials_fd >= 0)
		close(w.credentials_fd);
	if (w.timer >= 0)
		close(w.timer);
	if (w.renewal_timer >= 0)
		close(w.renewal_timer);
	line_session_clear(&w.session);
	line_store_stop();
	g_hash_table_unref(w.chats);
	g_hash_table_unref(w.checked_sends);
	g_hash_table_unref(w.left_groups);
	g_hash_table_unref(w.square_history_tokens);
	g_free(w.name);
	line_obs_http_close();
	line_transport_release();
	return 0;
}

static gboolean launch_url_arg(const char *arg)
{
	return arg &&
	       (!g_ascii_strncasecmp(arg, "line://", 7) ||
		!g_ascii_strncasecmp(arg, "lineb://", 8) ||
		!g_ascii_strncasecmp(arg, "https://line.me/", 16) ||
		!g_ascii_strncasecmp(arg, "https://line.naver.jp/", 22) ||
		!g_ascii_strncasecmp(arg, "https://liff.line.me/", 21) ||
		!g_ascii_strncasecmp(arg, "https://miniapp.line.me/", 24) ||
		!g_ascii_strncasecmp(arg, "https://openchat.line.me/", 25));
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	umask(0077);
	line_tune_process();
	if (argc == 2 && !strcmp(argv[1], "--worker"))
		return worker(-1);
	if (argc == 2 && !strcmp(argv[1], "--worker-password"))
		return worker(LINE_CREDENTIAL_FD);
	if (argc >= 2 && !strcmp(argv[1], "--pick-share-monitor"))
		return ui_pick_share_monitor(argc - 2, argv + 2);
	if (argc == 2 &&
	    (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		puts("avernella 0.1 — independent Linux client compatible with LINE\nUsage: avernella [LINE_URL]");
		return 0;
	}
	if (argc > 2 || (argc == 2 && !launch_url_arg(argv[1]))) {
		fputs("Usage: avernella [--help | LINE_URL]\n", stderr);
		return 2;
	}
	return line_ui(argc, argv);
}
