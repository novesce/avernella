/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include "stickers.h"

/*
 * LINE OpenChat (SquareService on /SQ1).
 *
 * Square MID prefixes (from LINE Android 26.16.0 iu8.md MIDType):
 *   's' = Square (community)
 *   'm' = SquareChat (conversation room)
 *   'p' = SquareMember (per-OpenChat identity)
 */
gboolean line_square_id(const char *id, char type);
gboolean line_is_square_chat(const char *id);
/* Extracts the invitation ticket from a line.me/ti/g2/... URL or raw ticket. */
char *line_square_parse_ticket(const char *input);

/*
 * Lists joined OpenChats and their chats. On success, returns an array of
 * chat-list rows matching line_chats(), and populates optional output objects
 * keyed by squareMid (squares, members, authorities) and chatMid (chat_to_sq).
 */
J *line_square_chats(Line *s, J **squares_out, J **members_out,
		     J **authorities_out, J **chat_to_square_out,
		     GError **error);

/*
 * Fetches the newest (continuation == NULL) or an earlier page of messages in
 * a SquareChat via fetchSquareChatEvents (BACKWARD). Returns formatted history
 * rows matching messaging.c, stores raw Message objects in *raw_out, and writes
 * the continuation token for olderHistory into *next_token_out.
 */
J *line_square_history(Line *s, const char *chat, const char *my_member_id,
		       const char *continuation, J **raw_out,
		       char **next_token_out, gboolean *more_out,
		       GError **error);

/* Sending and message actions on SquareService. */
J *line_square_send_text(Line *s, const char *chat, const char *my_member_id,
			 const char *text, const char *reply_to, J *emojis,
			 J *mentions, gint32 seq, GError **error);
J *line_square_send_sticker(Line *s, LineStickers *stickers, const char *chat,
			    const char *my_member_id, const char *package_id,
			    const char *sticker_id, const char *reply_to,
			    gint32 seq, GError **error);
/*
 * Sends a plaintext media placeholder message (IMAGE=1, VIDEO=2, AUDIO=3,
 * FILE=14) and returns the created message receipt {id, sentAt} so the caller
 * can upload the payload to OBS /r/g2/m/<id>.
 */
J *line_square_send_media_stub(Line *s, const char *chat,
			       const char *my_member_id, int content_type,
			       J *metadata, gint32 seq, GError **error);
J *line_square_forward_sticker(Line *s, const char *chat,
			       const char *my_member_id, J *message, gint32 seq,
			       GError **error);
J *line_square_forward_location(Line *s, const char *chat,
				const char *my_member_id, J *message,
				gint32 seq, GError **error);
J *line_square_forward_contact(Line *s, const char *chat,
			       const char *my_member_id, J *message, gint32 seq,
			       GError **error);
J *line_square_react(Line *s, const char *chat, const char *message_id,
		     const char *reaction, gint32 seq, GError **error);
J *line_square_unsend(Line *s, const char *chat, const char *message_id,
		      GError **error);
J *line_square_destroy_message(Line *s, const char *chat,
			       const char *message_id, GError **error);
J *line_square_mark_read(Line *s, const char *chat, const char *message_id,
			 GError **error);

/* Announcements on SquareService. */
J *line_square_announcements(Line *s, const char *chat, gboolean can_manage,
			     GError **error);
J *line_square_announce_message(Line *s, const char *chat,
				const char *message_id, const char *text,
				const char *sender_member_id, gint64 created_at,
				gint32 seq, GError **error);
J *line_square_remove_announcement(Line *s, const char *chat, const char *id,
				   GError **error);

/* Discovery, joining, leaving, and creation. */
J *line_square_categories(Line *s, GError **error);
J *line_square_popular_keywords(Line *s, GError **error);
J *line_square_search(Line *s, const char *query, const char *continuation,
		      GError **error);
J *line_square_search_mentionables(Line *s, const char *chat_mid,
				   const char *query, GError **error);
J *line_square_find_ticket(Line *s, const char *input, GError **error);
J *line_square_join(Line *s, const char *square_mid, const char *chat_mid,
		    const char *display_name, const char *join_code,
		    const char *approval_message, GError **error);
J *line_square_join_chat(Line *s, const char *chat_mid, GError **error);
J *line_square_leave(Line *s, const char *square_mid, GError **error);
J *line_square_leave_chat(Line *s, const char *chat_mid, gint64 member_rev,
			  GError **error);
J *line_square_create(Line *s, const char *name, const char *description,
		      const char *welcome, const char *display_name,
		      int category_id, gboolean searchable, int join_method,
		      const char *join_code, const char *approval_question,
		      gint32 seq, GError **error);
J *line_square_create_chat(Line *s, const char *square_mid, const char *name,
			   gint32 seq, GError **error);

/* Details, settings, profiles, and moderation. */
J *line_square_detail(Line *s, const char *square_mid, const char *chat_mid,
		      GError **error);
J *line_square_invite_link(Line *s, const char *mid, GError **error);
J *line_square_update(Line *s, J *current_square, J *params, GError **error);
J *line_square_update_chat(Line *s, J *current_chat, const char *name,
			   GError **error);
J *line_square_update_my_profile(Line *s, J *my_member,
				 const char *display_name, const char *bio,
				 const char *obs_hash, GError **error);
J *line_square_update_picture(Line *s, J *my_member, const char *path,
			      GError **error);
J *line_square_members(Line *s, const char *square_mid,
		       const char *continuation, const char *query,
		       gboolean pending_only, GError **error);
J *line_square_moderate_member(Line *s, const char *square_mid,
			       const char *member_mid, const char *action,
			       GError **error);
J *line_square_answer_join_request(Line *s, const char *square_mid,
				   const char *member_mid, gboolean approve,
				   GError **error);

/*
 * Live sync via fetchMyEvents. Updates *sync_token and *subscription_id in
 * place. Returns an array of normalized event objects ({kind, chat, square,
 * id, time, mine, reload}) or NULL on error.
 */
J *line_square_poll_events(Line *s, gint64 *subscription_id, char **sync_token,
			   J *my_members_by_square, GError **error);
