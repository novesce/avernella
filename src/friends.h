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
