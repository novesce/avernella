/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * One service page, excluding hidden and blocked friends. A present "next"
 * remains opaque to the UI. The worker owns paging and its selected result.
 */
J *line_friends_page(Line *s, const char *cursor, GError **error);
J *line_friend_lookup(Line *s, const char *kind, const char *value,
		      GError **error);
/*
 * trusted_result, kind and value belong to the worker's original lookup.
 * reserved_seq has already been allocated and persisted by the worker.
 */
J *line_friend_add(Line *s, J *trusted_result, const char *kind,
		   const char *value, gint32 reserved_seq, GError **error);
/* Zero values use LINE's personal QR defaults. */
struct line_friend_ticket_request {
	gint64 expiration_time;
	gint32 max_use_count;
	gboolean refresh;
};

/* The session's worker owns s. Reply via the calling request's IPC ID. */
J *line_friend_ticket(Line *s, const struct line_friend_ticket_request *request,
		      GError **error);
J *line_friend_invite(Line *s, GError **error);

J *line_friend_favorite(Line *s, J *trusted_friend, gboolean favorite,
			gint32 reserved_seq, GError **error);
J *line_friend_rename(Line *s, J *trusted_friend, const char *name,
		      gint32 reserved_seq, GError **error);

J *line_managed_friends_page(Line *s, const char *view, const char *cursor,
			     GError **error);
J *line_friend_visibility(Line *s, J *trusted_row, gboolean hidden,
			  gint32 reserved_seq, GError **error);
J *line_friend_block(Line *s, J *trusted_row, gboolean block,
		     gint32 reserved_seq, GError **error);
J *line_friend_delete(Line *s, J *trusted_row, gint32 reserved_seq,
		      GError **error);
J *line_account_settings(Line *s, GError **error);
J *line_account_settings_update(Line *s, const char *key, gboolean value,
				gint32 reserved_seq, GError **error);

J *line_own_profile(Line *s, GError **error);
J *line_own_profile_update(Line *s, J *changes, gint32 reserved_seq,
			   GError **error);
J *line_add_recommendation(Line *s, const char *id, gint32 reserved_seq,
			   GError **error);

/* A single profile card, friend or not, with its status message. */
J *line_friend_card(Line *s, const char *id, GError **error);
J *line_friend_requests(Line *s, GError **error);
J *line_friend_request_decline(Line *s, J *trusted_request, GError **error);

enum line_friend_source {
	LINE_FROM_GROUP = 5,
	LINE_FROM_STRANGER = 13,
	LINE_FROM_REQUEST = 21
};
J *line_friend_add_from(Line *s, J *trusted_row, enum line_friend_source source,
			gint32 reserved_seq, GError **error);

/*
 * Client-side pacing for friend searches and adds. Many lookups of LINE
 * IDs or phone numbers in a short time is what LINE treats as account
 * enumeration or spam, so the worker spaces them out, caps bursts, and
 * stops for a cooldown once LINE itself reports a temporary limit
 * (RelationService reason 4, returned as LINE_RATE_LIMITED).
 */
enum {
	LINE_PACE_SLOTS = 10,
};
struct line_pace {
	gint64 recent[LINE_PACE_SLOTS]; /* monotonic µs, 0 when unused */
	guint next;
	gint64 blocked_until;
};
struct line_pace_rule {
	gint64 gap_us; /* minimum time between two actions */
	guint burst; /* at most this many actions ... */
	gint64 window_us; /* ... within this window */
	gint64 cooldown_us; /* pause after LINE reports a limit */
};
extern const struct line_pace_rule line_lookup_pace, line_add_pace;
/* 0 when an action may start at now_us, otherwise whole seconds to wait. */
gint64 line_pace_wait(const struct line_pace *pace,
		      const struct line_pace_rule *rule, gint64 now_us);
/* Records an attempted action; limited means LINE refused it as too many. */
void line_pace_record(struct line_pace *pace, const struct line_pace_rule *rule,
		      gint64 now_us, gboolean limited);
