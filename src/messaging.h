/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * Results are owned by the caller. History is the newest 50 messages, in
 * chronological order. The caller selects a chat from this session's list.
 */
/*
 * A file name to send: valid UTF-8, at most 255 bytes, not "." or "..",
 * with no slash, backslash, control character or bidirectional override
 * (which can disguise an extension).
 */
gboolean line_safe_filename(const char *name);
/* A name from someone else, which also may not start with a dot. */
gboolean line_received_filename(const char *name);
J *line_chats(Line *s, GError **error);
J *line_mark_read(Line *s, const char *chat, const char *id,
		  gint32 reserved_seq, GError **error);
J *line_peer_ranges(Line *s, const char *chat, GError **error);
enum peer_notice {
	PEER_NOTICE_OTHER,
	PEER_NOTICE_UNCHANGED,
	PEER_NOTICE_ADDED
};
enum peer_notice line_peer_read_notice(const char *self, const char *chat,
				       J *notices, J *op);
J *line_peer_read_counts(const char *self, const char *chat, J *messages,
			 J *ranges, J *notices);
J *line_history(Line *s, const char *chat, GError **error);
J *line_send(Line *s, const char *chat, const char *text, GError **error);
J *line_forward_text(Line *s, const char *chat, J *message, GError **error);
J *line_forward_sticker(Line *s, const char *chat, J *message, GError **error);
J *line_forward_location(Line *s, const char *chat, J *message, GError **error);
J *line_forward_contact(Line *s, const char *chat, J *message, GError **error);
char *line_message_preview(Line *s, J *message, GError **error);

/* Encoded metadata and chunks are borrowed. Increments seq exactly once. */
J *line_send_sealed(Line *s, const char *chat, int type, J *meta, J *chunks,
		    const char *reply_to, GError **error);
/* Emoji ranges start at a UTF-16 offset and span a positive UTF-16 length. */
J *line_send_emoji(Line *s, const char *chat, const char *text,
		   const char *reply_to, J *emojis, GError **error);
J *line_send_decorated(Line *s, const char *chat, const char *text,
		       const char *reply_to, J *emojis, J *mentions,
		       GError **error);
J *line_send_reply(Line *s, const char *chat, const char *text,
		   const char *reply_to, GError **error);

J *line_history_raw(Line *s, const char *chat, J **messages, GError **error);
J *line_history_with_summary(Line *s, const char *chat, J *summary,
			     J **messages, GError **error);
J *line_history_cursor(J *index);
J *line_history_older(Line *s, const char *chat, J *boundary, J **messages,
		      gboolean *more, GError **error);
J *line_history_older_since(Line *s, const char *chat, J *boundary,
			    guint64 cleared_through, J **messages,
			    gboolean *more, GError **error);
J *line_message_action(Line *s, const char *id, const char *reaction,
		       GError **error);

void line_profile_operation(Line *s, J *op);

/* Resolve MID keys with empty string values in-place, using the session profile cache. */
void line_resolve_names(Line *s, J *names, char type);

J *line_announcements(Line *s, const char *chat, GError **error);
J *line_announce_message(Line *s, const char *chat, const char *id,
			 const char *text, gint32 seq, GError **error);
J *line_remove_announcement(Line *s, const char *chat, const char *id,
			    gint32 seq, GError **error);
