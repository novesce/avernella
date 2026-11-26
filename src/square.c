// SPDX-License-Identifier: GPL-3.0-or-later
#include "square.h"
#include "emoji.h"
#include "messaging.h"
#include "obs_http.h"
#include "profile_image.h"
#include "rich_message.h"
#include <string.h>

enum {
	SQUARE_PAGE_LIMIT = 100,
	SQUARE_HISTORY_LIMIT = 50,
	SQUARE_MEMBER_LIMIT = 50,
	SQUARE_FLEX_JSON_LIMIT = 64 * 1024,
	SQUARE_FLEX_ALT_LIMIT = 8 * 1024,
	SQUARE_FLEX_DEPTH = 24
};

/* SquareMemberRole (com.linecorp.square.protocol.thrift.common). */
enum {
	SQ_ROLE_ADMIN = 1,
	SQ_ROLE_CO_ADMIN = 2,
	SQ_ROLE_MEMBER = 10
};

/* SquareMembershipState. */
enum {
	SQ_STATE_JOIN_REQUESTED = 1,
	SQ_STATE_JOINED = 2,
	SQ_STATE_KICK_OUT = 5,
	SQ_STATE_BANNED = 6
};

/* SquareJoinMethodType. */
enum {
	SQ_JOIN_NONE = 0,
	SQ_JOIN_APPROVAL = 1,
	SQ_JOIN_CODE = 2
};

/* SquareAttribute. */
enum {
	SQ_ATTR_NAME = 1,
	SQ_ATTR_WELCOME = 2,
	SQ_ATTR_PROFILE_IMAGE = 3,
	SQ_ATTR_DESC = 4,
	SQ_ATTR_SEARCHABLE = 6,
	SQ_ATTR_CATEGORY = 7,
	SQ_ATTR_ABLE_TO_USE_INVITE = 9,
	SQ_ATTR_JOIN_METHOD = 12
};

/* SquareChatAttribute. */
enum {
	SQ_CHAT_ATTR_NAME = 2
};

/* SquareMemberAttribute. */
enum {
	SQ_MEMBER_ATTR_DISPLAY_NAME = 1,
	SQ_MEMBER_ATTR_PROFILE_IMAGE = 2,
	SQ_MEMBER_ATTR_MEMBERSHIP_STATE = 5,
	SQ_MEMBER_ATTR_ROLE = 6,
	SQ_MEMBER_ATTR_SELF_INTRO = 8
};

/* SquareEventType (classes.dex SquareEventType). */
enum {
	SQ_EVT_RECEIVE_MESSAGE = 0,
	SQ_EVT_SEND_MESSAGE = 1,
	SQ_EVT_JOIN_CHAT = 2,
	SQ_EVT_INVITE_CHAT = 3,
	SQ_EVT_LEAVE_CHAT = 4,
	SQ_EVT_DESTROY_MESSAGE = 5,
	SQ_EVT_MARK_AS_READ = 6,
	SQ_EVT_UPDATE_MEMBER_PROFILE = 7,
	SQ_EVT_UPDATE_SQUARE = 8,
	SQ_EVT_UPDATE_SQUARE_STATUS = 9,
	SQ_EVT_UPDATE_AUTHORITY = 10,
	SQ_EVT_UPDATE_MEMBER = 11,
	SQ_EVT_UPDATE_CHAT = 12,
	SQ_EVT_UPDATE_CHAT_STATUS = 13,
	SQ_EVT_UPDATE_CHAT_MEMBER = 14,
	SQ_EVT_SHUTDOWN_SQUARE = 18,
	SQ_EVT_KICKOUT = 19,
	SQ_EVT_DELETE_CHAT = 20,
	SQ_EVT_NOTIF_JOIN_REQUEST = 21,
	SQ_EVT_NOTIF_JOINED = 22,
	SQ_EVT_NOTIF_KICKED_OUT = 26,
	SQ_EVT_NOTIF_SQUARE_DELETE = 27,
	SQ_EVT_NOTIF_CHAT_DELETE = 28,
	SQ_EVT_NOTIF_MESSAGE = 29,
	SQ_EVT_UPDATE_CHAT_NAME = 30,
	SQ_EVT_UPDATE_CHAT_IMAGE = 31,
	SQ_EVT_UPDATE_ANNOUNCEMENT = 37,
	SQ_EVT_MUTATE_MESSAGE = 41,
	SQ_EVT_NOTIF_NEW_MEMBER = 42,
	SQ_EVT_NOTIF_REACTION = 47,
	SQ_EVT_SYSTEM_MESSAGE = 49
};

static J *array(void)
{
	return json_object_new_array();
}

static J *string(const char *s)
{
	return json_object_new_string(s);
}

static void put(J *o, const char *k, J *v)
{
	json_object_object_add(o, k, v);
}

static void put_str(J *o, const char *k, const char *v)
{
	put(o, k, string(v));
}

static void put_num(J *o, const char *k, gint64 n)
{
	put(o, k, json_object_new_int64(n));
}

static void put_bool(J *o, const char *k, gboolean b)
{
	put(o, k, json_object_new_boolean(b));
}

static size_t length(J *v)
{
	return json_object_is_type(v, json_type_array) ?
		       json_object_array_length(v) :
		       0;
}

static J *at(J *v, size_t n)
{
	return json_object_array_get_idx(v, n);
}

static const char *nonempty(J *v, const char *k)
{
	const char *s = strfield(v, k);

	return s && *s && g_utf8_validate(s, -1, NULL) ? s : NULL;
}

static gpointer problem(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return NULL;
}

gboolean line_square_id(const char *id, char type)
{
	if (!id || id[0] != type || strlen(id) != 33)
		return FALSE;
	for (int i = 1; i < 33; i++)
		if (!g_ascii_isxdigit(id[i]))
			return FALSE;
	return TRUE;
}

gboolean line_is_square_chat(const char *id)
{
	return line_square_id(id, 'm');
}

static gboolean decimal_id(const char *id)
{
	guint64 numeric;

	return id && *id >= '1' && *id <= '9' &&
	       g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &numeric,
					  NULL);
}

static gboolean valid_obs_hash(const char *hash)
{
	if (!hash || !*hash || strlen(hash) > 512)
		return FALSE;
	for (const char *p = hash; *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			return FALSE;
	return TRUE;
}

static char *obs_avatar_path(const char *hash)
{
	return valid_obs_hash(hash) ? g_strconcat("/", hash, NULL) : NULL;
}

static void remember_square_avatar(Line *s, const char *id, const char *hash)
{
	if (!s || !s->pictures || !id || !*id)
		return;
	g_autofree char *path = obs_avatar_path(hash);

	if (path)
		put_str(s->pictures, id, path);
	else
		json_object_object_del(s->pictures, id);
}

/*
 * SquareService uses its own SquareException namespace on /SQ1 (errorCode at
 * field 1, reason at field 3). Map errors clearly and reset s->service_code so
 * Square codes are never mistaken for TalkException codes.
 */
static J *square_rpc(Line *s, const char *method, J *request, GError **error)
{
	g_autoptr(J) args = obj();

	put(args, "1:obj", request ? json_object_get(request) : obj());
	g_autoptr(GError) call_err = NULL;
	J *reply = line_rpc(s, "/SQ1", method, args, &call_err);

	if (reply)
		return reply;
	int code = s->service_code;
	J *exc = line_service_exception();
	const char *reason = nonempty(exc, "3");

	s->service_code = -1;
	if (g_error_matches(call_err, LINE_ERROR, LINE_METHOD_UNAVAILABLE) ||
	    g_error_matches(call_err, LINE_ERROR, LINE_RATE_LIMITED) ||
	    g_error_matches(call_err, LINE_ERROR, LINE_SESSION_EXPIRED) ||
	    g_error_matches(call_err, LINE_ERROR, LINE_ACCESS_DENIED) ||
	    g_error_matches(call_err, LINE_ERROR, LINE_TIMEOUT)) {
		g_propagate_error(error, g_steal_pointer(&call_err));
		return NULL;
	}
	if (code == 503) {
		g_set_error_literal(
			error, LINE_ERROR, LINE_RATE_LIMITED,
			"LINE OpenChat is busy right now. Try again shortly.");
		return NULL;
	}
	if (code == 403) {
		problem(error,
			reason ?
				reason :
				"You do not have permission for this OpenChat action.");
		return NULL;
	}
	if (code == 404) {
		problem(error,
			reason ? reason : "This OpenChat was not found.");
		return NULL;
	}
	if (code == 409) {
		problem(error, "This OpenChat changed. Refresh and try again.");
		return NULL;
	}
	if (code == 410) {
		problem(error,
			reason ?
				reason :
				"This OpenChat action is no longer available.");
		return NULL;
	}
	if (code == 400) {
		problem(error,
			reason ? reason :
				 "Check your OpenChat details and try again.");
		return NULL;
	}
	if (code == 505) {
		problem(error,
			"LINE OpenChat is temporarily under maintenance.");
		return NULL;
	}
	if (reason) {
		problem(error, reason);
		return NULL;
	}
	g_propagate_error(error, g_steal_pointer(&call_err));
	return NULL;
}

static gboolean role_allows(int my_role, int required_role)
{
	return my_role > 0 && required_role > 0 && my_role <= required_role;
}

static const char *role_name(int role)
{
	switch (role) {
	case SQ_ROLE_ADMIN:
		return "admin";
	case SQ_ROLE_CO_ADMIN:
		return "coadmin";
	default:
		return "member";
	}
}

static char *clean_text_field(const char *text, gsize max_units,
			      gboolean allow_empty, gboolean allow_newline,
			      const char *err_msg, GError **error)
{
	if (!text || !g_utf8_validate(text, -1, NULL))
		return allow_empty ? g_strdup("") : problem(error, err_msg);
	g_autofree char *clean = g_strdup(text);

	g_strstrip(clean);
	gsize units = 0;

	for (const char *p = clean; *p; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);

		if (g_unichar_iscntrl(ch) &&
		    (!allow_newline || (ch != '\n' && ch != '\r')))
			return problem(error, err_msg);
		units += ch > 0xffff ? 2 : 1;
	}
	if ((!allow_empty && !units) || units > max_units)
		return problem(error, err_msg);
	return g_steal_pointer(&clean);
}

static int chat_order(const void *a, const void *b)
{
	J *lhs = *(J *const *)a, *rhs = *(J *const *)b;
	gint64 lt = json_object_get_int64(field(lhs, "time"));
	gint64 rt = json_object_get_int64(field(rhs, "time"));

	return lt == rt ? strcmp(strfield(lhs, "id"), strfield(rhs, "id")) :
			  (lt > rt ? -1 : 1);
}

static J *make_chat_row(Line *s, J *chat, J *status, J *square, J *member)
{
	const char *id = nonempty(chat, "1");
	const char *sq_mid = nonempty(chat, "2");

	if (!line_square_id(id, 'm') || !line_square_id(sq_mid, 's'))
		return NULL;
	const char *name = nonempty(chat, "4");

	if (!name && square)
		name = nonempty(square, "2");
	const char *hash = nonempty(chat, "5");

	if (!hash && square)
		hash = nonempty(square, "4");
	remember_square_avatar(s, id, hash);
	remember_square_avatar(s, sq_mid,
			       square ? nonempty(square, "4") : NULL);

	J *last_sq_msg = field(status, "3");
	J *last_msg = field(last_sq_msg, "1");
	J *other = field(status, "5");
	gint64 unread = json_object_get_int64(field(other, "2"));
	gint64 member_count = json_object_get_int64(field(other, "1"));
	const char *last_id = nonempty(last_msg, "4");
	const char *seen_id = nonempty(other, "3");
	gint64 last_time = json_object_get_int64(field(last_msg, "5"));
	const char *my_mid = member ? nonempty(member, "1") : NULL;
	int my_role = member ? json_object_get_int(field(member, "8")) : 0;

	J *row = obj();

	put_str(row, "id", id);
	put_str(row, "name", name ? name : id);
	put_bool(row, "square", TRUE);
	put_str(row, "squareMid", sq_mid);
	put_num(row, "unread", MAX(0, unread));
	put_num(row, "memberCount", MAX(0, member_count));
	put_num(row, "time", last_time);
	if (last_id)
		put_str(row, "lastId", last_id);
	if (seen_id)
		put_str(row, "lastSeenId", seen_id);
	if (my_mid)
		put_str(row, "myMemberId", my_mid);
	if (my_role)
		put_str(row, "myRole", role_name(my_role));
	g_autofree char *avatar = obs_avatar_path(hash);

	if (avatar)
		put_str(row, "avatarKey", avatar);
	return row;
}

J *line_square_chats(Line *s, J **squares_out, J **members_out,
		     J **authorities_out, J **chat_to_square_out,
		     GError **error)
{
	g_autoptr(J) squares = obj(), members = obj(), authorities = obj();
	g_autoptr(J) chat_to_sq = obj();
	g_autoptr(J) rows = array();
	g_autofree char *sq_token = NULL;

	/* 1. Fetch all joined Squares, my memberships, and authorities. */
	for (int page = 0; page < 20; page++) {
		g_autoptr(J) req = obj();

		if (sq_token)
			put_str(req, "2:str", sq_token);
		put_num(req, "3:i32", SQUARE_PAGE_LIMIT);
		g_autoptr(J) reply =
			square_rpc(s, "getJoinedSquares", req, error);
		if (!reply)
			return NULL;
		J *list = field(reply, "1");
		J *mem_map = field(reply, "2");
		J *auth_map = field(reply, "3");

		for (size_t i = 0; i < length(list); i++) {
			J *sq = at(list, i);
			const char *mid = nonempty(sq, "1");

			if (line_square_id(mid, 's')) {
				put(squares, mid, json_object_get(sq));
				remember_square_avatar(s, mid,
						       nonempty(sq, "4"));
			}
		}
		if (json_object_is_type(mem_map, json_type_object)) {
			json_object_object_foreach(mem_map, k, v) {
				if (line_square_id(k, 's')) {
					put(members, k, json_object_get(v));
					remember_square_avatar(
						s, nonempty(v, "1"),
						nonempty(v, "4"));
				}
			}
		}
		if (json_object_is_type(auth_map, json_type_object)) {
			json_object_object_foreach(auth_map, k, v) {
				if (line_square_id(k, 's'))
					put(authorities, k, json_object_get(v));
			}
		}
		const char *next = nonempty(reply, "5");

		if (!next || (sq_token && !strcmp(next, sq_token)))
			break;
		g_free(sq_token);
		sq_token = g_strdup(next);
	}

	/* 2. Fetch all joined SquareChats and statuses. */
	g_autofree char *chat_token = NULL;

	g_autoptr(GHashTable) seen =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (int page = 0; page < 20; page++) {
		g_autoptr(J) req = obj();

		if (chat_token)
			put_str(req, "2:str", chat_token);
		put_num(req, "3:i32", SQUARE_PAGE_LIMIT);
		g_autoptr(J) reply =
			square_rpc(s, "getJoinedSquareChats", req, error);
		if (!reply)
			return NULL;
		J *chats = field(reply, "1");
		J *statuses = field(reply, "3");

		for (size_t i = 0; i < length(chats); i++) {
			J *chat = at(chats, i);
			const char *cid = nonempty(chat, "1");
			const char *smid = nonempty(chat, "2");

			if (!line_square_id(cid, 'm') ||
			    !line_square_id(smid, 's') ||
			    !g_hash_table_add(seen, g_strdup(cid)))
				continue;
			/* Skip deleted or suspended chats. */
			if (json_object_get_int(field(chat, "8")) != 0)
				continue;
			J *sq = field(squares, smid);
			J *mem = field(members, smid);
			J *st = field(statuses, cid);
			J *row = make_chat_row(s, chat, st, sq, mem);

			if (row) {
				json_object_array_add(rows, row);
				put_str(chat_to_sq, cid, smid);
			}
		}
		const char *next = nonempty(reply, "4");

		if (!next || (chat_token && !strcmp(next, chat_token)))
			break;
		g_free(chat_token);
		chat_token = g_strdup(next);
	}
	json_object_array_sort(rows, chat_order);
	if (squares_out)
		*squares_out = g_steal_pointer(&squares);
	if (members_out)
		*members_out = g_steal_pointer(&members);
	if (authorities_out)
		*authorities_out = g_steal_pointer(&authorities);
	if (chat_to_square_out)
		*chat_to_square_out = g_steal_pointer(&chat_to_sq);
	return g_steal_pointer(&rows);
}

static const char *content_label(gint64 type)
{
	switch (type) {
	case 1:
		return "[Photo]";
	case 2:
		return "[Video]";
	case 3:
		return "[Audio]";
	case 7:
		return "[Sticker]";
	case 13:
		return "[Contact]";
	case 14:
		return "[File]";
	case 15:
		return "[Location]";
	case 17:
		return "[Rich message]";
	case 22:
		return "[Card]";
	default:
		return "[Unsupported message]";
	}
}

static J *parse_metadata_json(J *meta, const char *key, size_t limit, int depth)
{
	const char *raw = nonempty(meta, key);
	size_t n = raw ? strlen(raw) : 0;

	if (!n || n > limit)
		return NULL;
	struct json_tokener *tok = json_tokener_new_ex(depth);

	if (!tok)
		return NULL;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT |
					    JSON_TOKENER_VALIDATE_UTF8);
	J *val = json_tokener_parse_ex(tok, raw, (int)n);

	if (json_tokener_get_error(tok) != json_tokener_success ||
	    json_tokener_get_parse_end(tok) != n ||
	    !json_object_is_type(val, json_type_object)) {
		json_object_put(val);
		val = NULL;
	}
	json_tokener_free(tok);
	return val;
}

static J *extract_square_reactions(J *reaction_status)
{
	J *by_type = field(reaction_status, "2");

	if (!json_object_is_type(by_type, json_type_object) ||
	    !json_object_object_length(by_type))
		return NULL;
	static const char *const names[] = { NULL,  NULL,      "nice", "love",
					     "fun", "amazing", "sad",  "omg" };
	J *counts = obj();

	json_object_object_foreach(by_type, key, val) {
		guint64 kind;

		if (g_ascii_string_to_unsigned(key, 10, 2, 7, &kind, NULL)) {
			gint64 c = json_object_get_int64(val);

			if (c > 0 && kind < G_N_ELEMENTS(names) && names[kind])
				put_num(counts, names[kind], c);
		}
	}
	if (!json_object_object_length(counts)) {
		json_object_put(counts);
		return NULL;
	}
	return counts;
}

static int message_order(const void *a, const void *b)
{
	J *lhs = *(J *const *)a, *rhs = *(J *const *)b;
	gint64 lt = json_object_get_int64(field(lhs, "time"));
	gint64 rt = json_object_get_int64(field(rhs, "time"));

	if (lt != rt)
		return lt < rt ? -1 : 1;
	return strcmp(strfield(lhs, "id"), strfield(rhs, "id"));
}

static gboolean utf16_step(const char **cursor, guint64 *pos, guint64 target)
{
	while (*pos < target) {
		if (!**cursor)
			return FALSE;
		gunichar ch = g_utf8_get_char(*cursor);
		guint64 step = ch > 0xffff ? 2 : 1;

		if (*pos + step > target)
			return FALSE;
		*cursor = g_utf8_next_char(*cursor);
		*pos += step;
	}
	return *pos == target;
}

static J *parse_sticon_emojis(const char *text, J *meta)
{
	g_autoptr(J) replace =
		parse_metadata_json(meta, "REPLACE", 256 * 1024, 16);
	J *resources = field(field(replace, "sticon"), "resources");

	if (!text || !length(resources) || length(resources) > 1024)
		return NULL;
	g_autoptr(J) out = array();
	const char *cursor = text;
	guint64 pos = 0;

	for (size_t i = 0; i < length(resources); i++) {
		J *it = at(resources, i);
		const char *pack = nonempty(it, "productId");
		const char *id = nonempty(it, "sticonId");
		const char *kind = nonempty(it, "resourceType");
		gint64 s = json_object_get_int64(field(it, "S"));
		gint64 e = json_object_get_int64(field(it, "E"));
		gint64 ver =
			field(it, "version") ?
				json_object_get_int64(field(it, "version")) :
				1;

		if (!kind)
			kind = "STATIC";
		if (!line_emoji_id_valid(pack, id) || s < 0 || e <= s ||
		    (strcmp(kind, "STATIC") && strcmp(kind, "ANIMATION")) ||
		    !utf16_step(&cursor, &pos, (guint64)s) ||
		    !utf16_step(&cursor, &pos, (guint64)e))
			return NULL;
		J *r = obj();

		put_num(r, "start", s);
		put_num(r, "length", e - s);
		put_str(r, "productId", pack);
		put_str(r, "emojiId", id);
		put_num(r, "version", ver);
		put_str(r, "resourceType", kind);
		json_object_array_add(out, r);
	}
	return g_steal_pointer(&out);
}

static J *format_square_message(Line *s, J *sq_msg, const char *sender_name,
				J *reaction_status, const char *my_member_id,
				J **raw_msg_out)
{
	J *msg = field(sq_msg, "1");

	if (!json_object_is_type(msg, json_type_object))
		return NULL;
	int state = json_object_get_int(field(sq_msg, "5"));

	if (state == 2 || state == 4) /* DELETED or UNSENT */
		return NULL;
	const char *id = nonempty(msg, "4");
	const char *sender = nonempty(msg, "1");
	gint64 type = json_object_get_int64(field(msg, "15"));
	const char *text = nonempty(msg, "10");
	J *meta = field(msg, "18");
	gboolean own = sender && my_member_id && !strcmp(sender, my_member_id);
	gboolean notice = type == 18;

	J *row = obj();

	if (type == 0) {
		J *emojis = parse_sticon_emojis(text, meta);

		if (emojis)
			put(row, "emojis", emojis);
	}
	static const char *const kinds[] = {
		"text",	    "photo",	     "video",	"audio",      "html",
		"pdf",	    "call",	     "sticker", "presence",   "gift",
		"board",    "app",	     "link",	"contact",    "file",
		"location", "post",	     "rich",	"chat_event", "music",
		"payment",  "externalImage", "flex"
	};
	g_autofree char *alt = NULL;

	if (type == 22) {
		g_autoptr(J) flex = parse_metadata_json(meta, "FLEX_JSON",
							SQUARE_FLEX_JSON_LIMIT,
							SQUARE_FLEX_DEPTH);
		J *contents = flex;
		const char *ft = nonempty(contents, "type");
		const char *a = nonempty(meta, "ALT_TEXT");

		if (!g_strcmp0(ft, "flex")) {
			if (!a)
				a = nonempty(flex, "altText");
			contents = field(flex, "contents");
			ft = nonempty(contents, "type");
		}
		if (!g_strcmp0(ft, "bubble") ||
		    (!g_strcmp0(ft, "carousel") &&
		     length(field(contents, "contents"))))
			put(row, "flex", json_object_get(contents));
		if (a && strlen(a) <= SQUARE_FLEX_ALT_LIMIT)
			alt = g_strdup(a);
	} else if (type == 17) {
		J *rich = line_rich_parse(meta, &alt);

		if (rich)
			put(row, "rich", rich);
	}
	if (alt)
		text = alt;
	const char *filename = nonempty(meta, "FILE_NAME");

	if (filename && !line_received_filename(filename))
		filename = NULL;
	if (type == 2) {
		guint64 duration;
		const char *ms = nonempty(meta, "DURATION");

		if (ms && g_ascii_string_to_unsigned(ms, 10, 1, G_MAXINT32,
						     &duration, NULL))
			put_num(row, "durationMs", (gint64)duration);
		if (!filename)
			filename = "video.mp4";
	}
	if (filename)
		put_str(row, "filename", filename);
	const char *related = nonempty(msg, "21");

	if (related && json_object_get_int(field(msg, "22")) == 3)
		put_str(row, "replyTo", related);
	if (!notice && sender && line_square_id(sender, 'p')) {
		put_str(row, "senderId", sender);
		const char *pic = strfield(s->pictures, sender);

		put_str(row, "avatarKey", pic ? pic : "");
	}
	put_str(row, "type",
		notice						? "chat_event" :
		type >= 0 && type < (gint64)G_N_ELEMENTS(kinds) ? kinds[type] :
								  "other");
	J *reactions = !notice ? extract_square_reactions(reaction_status) :
				 NULL;
	if (reactions)
		put(row, "reactions", reactions);
	if (type == 15 && !text)
		text = nonempty(field(msg, "11"), "2");
	put_str(row, "id", id ? id : "");
	put_str(row, "from",
		own	    ? "You" :
		sender_name ? sender_name :
		sender	    ? sender :
			      "OpenChat");
	put_str(row, "text", text ? text : content_label(type));
	put_bool(row, "own", own && !notice);
	put_bool(row, "square", TRUE);
	put_num(row, "time", json_object_get_int64(field(msg, "5")));
	if (raw_msg_out)
		*raw_msg_out = msg;
	return row;
}

static J *format_square_system_event(gint64 created_time, const char *id,
				     const char *text)
{
	J *row = obj();

	put_str(row, "id", id ? id : "");
	put_str(row, "from", "OpenChat");
	put_str(row, "type", "chat_event");
	put_str(row, "text", text);
	put_bool(row, "own", FALSE);
	put_bool(row, "square", TRUE);
	put_num(row, "time", created_time);
	return row;
}

J *line_square_history(Line *s, const char *chat, const char *my_member_id,
		       const char *continuation, J **raw_out,
		       char **next_token_out, gboolean *more_out,
		       GError **error)
{
	if (!line_square_id(chat, 'm'))
		return problem(error, "Choose an OpenChat conversation.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat);
	put_num(req, "4:i32", SQUARE_HISTORY_LIMIT);
	put_num(req, "5:i32", 2); /* FetchDirection.BACKWARD */
	put_num(req, "6:i32", 0); /* inclusive = false */
	if (continuation && *continuation)
		put_str(req, "7:str", continuation);
	put_num(req, "8:i32", 1); /* FetchType.DEFAULT */
	g_autoptr(J) reply = square_rpc(s, "fetchSquareChatEvents", req, error);
	if (!reply)
		return NULL;
	J *events = field(reply, "2");

	if (events && !json_object_is_type(events, json_type_array))
		return problem(error,
			       "LINE returned an unreadable OpenChat history.");
	g_autoptr(J) rows = array(), raw = obj();
	g_autoptr(GHashTable) destroyed =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	/* First pass: collect destroyed message IDs so we hide them. */
	for (size_t i = 0; i < length(events); i++) {
		J *ev = at(events, i);
		int etype = json_object_get_int(field(ev, "3"));
		J *payload = field(ev, "4");

		if (etype == SQ_EVT_DESTROY_MESSAGE) {
			const char *mid = nonempty(field(payload, "6"), "3");

			if (mid)
				g_hash_table_add(destroyed, g_strdup(mid));
		}
	}
	for (size_t i = 0; i < length(events); i++) {
		J *ev = at(events, i);
		gint64 etime = json_object_get_int64(field(ev, "2"));
		int etype = json_object_get_int(field(ev, "3"));
		J *payload = field(ev, "4");
		J *row = NULL;
		J *raw_msg = NULL;

		if (etype == SQ_EVT_RECEIVE_MESSAGE) {
			J *p = field(payload, "1");

			row = format_square_message(s, field(p, "2"),
						    nonempty(p, "3"),
						    field(p, "4"), my_member_id,
						    &raw_msg);
		} else if (etype == SQ_EVT_SEND_MESSAGE) {
			J *p = field(payload, "2");

			row = format_square_message(s, field(p, "2"),
						    nonempty(p, "4"),
						    field(p, "5"), my_member_id,
						    &raw_msg);
		} else if (etype == SQ_EVT_JOIN_CHAT) {
			J *m = field(field(payload, "3"), "2");
			const char *who = nonempty(m, "3");

			remember_square_avatar(s, nonempty(m, "1"),
					       nonempty(m, "4"));
			g_autofree char *txt =
				g_strdup_printf("%s joined the OpenChat",
						who ? who : "A member");
			row = format_square_system_event(etime, "", txt);
		} else if (etype == SQ_EVT_LEAVE_CHAT) {
			J *p = field(payload, "5");
			const char *who = nonempty(field(p, "4"), "3");
			g_autofree char *txt = g_strdup_printf(
				"%s left the OpenChat", who ? who : "A member");

			row = format_square_system_event(etime, "", txt);
		} else if (etype == SQ_EVT_SYSTEM_MESSAGE) {
			const char *txt = nonempty(field(payload, "47"), "2");

			if (txt)
				row = format_square_system_event(etime, "",
								 txt);
		} else if (etype == SQ_EVT_UPDATE_CHAT_NAME) {
			J *p = field(payload, "31");
			const char *who = nonempty(field(p, "2"), "3");
			const char *updated = nonempty(p, "3");
			g_autofree char *txt =
				updated ?
					g_strdup_printf(
						"%s renamed the OpenChat to %s",
						who ? who : "An admin",
						updated) :
					g_strdup("The OpenChat was renamed");

			row = format_square_system_event(etime, "", txt);
		}
		if (!row)
			continue;
		const char *mid = strfield(row, "id");

		if (mid && *mid && g_hash_table_contains(destroyed, mid)) {
			json_object_put(row);
			continue;
		}
		if (raw_msg && mid && *mid)
			put(raw, mid, json_object_get(raw_msg));
		json_object_array_add(rows, row);
	}
	json_object_array_sort(rows, message_order);
	const char *next = nonempty(reply, "4");

	if (next_token_out)
		*next_token_out = next ? g_strdup(next) : NULL;
	if (more_out)
		*more_out = next != NULL && *next != '\0';
	if (raw_out)
		*raw_out = g_steal_pointer(&raw);
	return g_steal_pointer(&rows);
}

static J *build_emoji_replace(const char *text, J *emojis)
{
	if (!length(emojis) || length(emojis) > 1024)
		return NULL;
	g_autoptr(J) resources = array(), packs = array(), seen = obj();
	const char *cursor = text;
	guint64 pos = 0;

	for (size_t i = 0; i < length(emojis); i++) {
		J *it = at(emojis, i);
		const char *pack = nonempty(it, "productId");
		const char *id = nonempty(it, "emojiId");
		const char *kind = nonempty(it, "resourceType");
		gint64 start = json_object_get_int64(field(it, "start"));
		gint64 len = json_object_get_int64(field(it, "length"));
		gint64 ver =
			field(it, "version") ?
				json_object_get_int64(field(it, "version")) :
				1;

		if (!kind)
			kind = "STATIC";
		if (!line_emoji_id_valid(pack, id) || start < 0 || len <= 0 ||
		    !utf16_step(&cursor, &pos, (guint64)start) ||
		    !utf16_step(&cursor, &pos, (guint64)(start + len)))
			return NULL;
		J *r = obj();

		put_num(r, "S", start);
		put_num(r, "E", start + len);
		put_str(r, "productId", pack);
		put_str(r, "sticonId", id);
		put_num(r, "version", ver);
		put_str(r, "resourceType", kind);
		json_object_array_add(resources, r);
		if (!field(seen, pack)) {
			put_bool(seen, pack, TRUE);
			json_object_array_add(packs, string(pack));
		}
	}
	J *replace = obj(), *sticon = obj(), *own = obj();

	put(sticon, "resources", g_steal_pointer(&resources));
	put(replace, "sticon", sticon);
	put(own, "productIds", g_steal_pointer(&packs));
	put(replace, "sticonOwnership", own);
	return replace;
}

static J *build_mentions_meta(const char *text, J *mentions)
{
	if (!length(mentions) || length(mentions) > 20)
		return NULL;
	g_autoptr(J) list = array();
	const char *cursor = text;
	guint64 pos = 0;

	for (size_t i = 0; i < length(mentions); i++) {
		J *it = at(mentions, i);
		const char *mid = nonempty(it, "id");
		gint64 start = json_object_get_int64(field(it, "start"));
		gint64 len = json_object_get_int64(field(it, "length"));

		if (!line_square_id(mid, 'p') || start < 0 || len <= 0 ||
		    !utf16_step(&cursor, &pos, (guint64)start) ||
		    !utf16_step(&cursor, &pos, (guint64)(start + len)))
			return NULL;
		g_autofree char *s_str =
			g_strdup_printf("%" G_GINT64_FORMAT, start);
		g_autofree char *l_str =
			g_strdup_printf("%" G_GINT64_FORMAT, len);
		J *m = obj();

		put_str(m, "S", s_str);
		put_str(m, "L", l_str);
		put_str(m, "M", mid);
		json_object_array_add(list, m);
	}
	J *out = obj();

	put(out, "MENTIONEES", g_steal_pointer(&list));
	return out;
}

static J *send_square_message(Line *s, const char *chat,
			      const char *my_member_id, J *msg, gint32 seq,
			      GError **error)
{
	if (!line_square_id(chat, 'm') || seq <= 0)
		return problem(error, "Select an OpenChat conversation.");
	g_autoptr(J) req = obj();
	J *sq_msg = obj();

	if (my_member_id && line_square_id(my_member_id, 'p'))
		put_str(msg, "1:str", my_member_id);
	put_str(msg, "2:str", chat);
	put_num(msg, "3:i32", 4); /* MIDType.SQUARE_CHAT */
	put_num(msg, "5:i64", 0);
	put_num(msg, "6:i64", 0);
	put_bool(msg, "14:bool", FALSE);
	put_num(msg, "19:byte", 0);

	put(sq_msg, "1:obj", json_object_get(msg));
	put_num(sq_msg, "3:i32", 5); /* MIDType.SQUARE_MEMBER */

	put_num(req, "1:i32", seq);
	put_str(req, "2:str", chat);
	put(req, "3:obj", sq_msg);

	g_autoptr(J) reply = square_rpc(s, "sendMessage", req, error);
	if (!reply)
		return NULL;
	J *created = field(field(reply, "1"), "1");
	const char *id = nonempty(created, "4");

	if (!id)
		return problem(
			error,
			"LINE returned no message ID. Check the OpenChat before resending.");
	J *receipt = obj();

	put_str(receipt, "id", id);
	J *sent_at = field(created, "5");

	if (json_object_is_type(sent_at, json_type_int))
		put(receipt, "sentAt", json_object_get(sent_at));
	return receipt;
}

J *line_square_send_text(Line *s, const char *chat, const char *my_member_id,
			 const char *text, const char *reply_to, J *emojis,
			 J *mentions, gint32 seq, GError **error)
{
	if (!text || !*text || !g_utf8_validate(text, -1, NULL) ||
	    strlen(text) > 20000)
		return problem(error,
			       "Enter a message of up to 20,000 UTF-8 bytes.");
	if (reply_to && !decimal_id(reply_to))
		return problem(error, "Select a message to reply to.");
	g_autoptr(J) msg = obj();
	g_autoptr(J) meta = obj();

	put_str(msg, "10:str", text);
	put_num(msg, "15:i32", 0);
	if (length(emojis)) {
		g_autoptr(J) rep = build_emoji_replace(text, emojis);

		if (rep)
			put_str(meta, "REPLACE",
				json_object_to_json_string_ext(
					rep, JSON_C_TO_STRING_PLAIN));
	}
	if (length(mentions)) {
		g_autoptr(J) ment = build_mentions_meta(text, mentions);

		if (ment)
			put_str(meta, "MENTION",
				json_object_to_json_string_ext(
					ment, JSON_C_TO_STRING_PLAIN));
	}
	if (json_object_object_length(meta))
		put(msg, "18:map:str:str", json_object_get(meta));
	if (reply_to) {
		put_str(msg, "21:str", reply_to);
		put_num(msg, "22:i32", 3); /* REPLY */
		put_num(msg, "24:i32", 2); /* ServiceCode.SQUARE */
	}
	return send_square_message(s, chat, my_member_id, msg, seq, error);
}

J *line_square_send_sticker(Line *s, LineStickers *stickers, const char *chat,
			    const char *my_member_id, const char *package_id,
			    const char *sticker_id, const char *reply_to,
			    gint32 seq, GError **error)
{
	if (reply_to && !decimal_id(reply_to))
		return problem(error, "Select a message to reply to.");
	g_autoptr(J) meta = line_sticker_metadata(s, stickers, package_id,
						  sticker_id, error);
	if (!meta)
		return NULL;
	g_autoptr(J) msg = obj();

	put_num(msg, "15:i32", 7); /* STICKER */
	put(msg, "18:map:str:str", json_object_get(meta));
	if (reply_to) {
		put_str(msg, "21:str", reply_to);
		put_num(msg, "22:i32", 3);
		put_num(msg, "24:i32", 2);
	}
	return send_square_message(s, chat, my_member_id, msg, seq, error);
}

J *line_square_send_media_stub(Line *s, const char *chat,
			       const char *my_member_id, int content_type,
			       J *metadata, gint32 seq, GError **error)
{
	g_autoptr(J) msg = obj();

	put_num(msg, "15:i32", content_type);
	if (metadata && json_object_object_length(metadata))
		put(msg, "18:map:str:str", json_object_get(metadata));
	return send_square_message(s, chat, my_member_id, msg, seq, error);
}

J *line_square_react(Line *s, const char *chat, const char *message_id,
		     const char *reaction, gint32 seq, GError **error)
{
	if (!line_square_id(chat, 'm') || !decimal_id(message_id) || seq <= 0 ||
	    !reaction)
		return problem(error, "Choose a message and reaction.");
	static const char *const names[] = { "none",	"nice", "love", "fun",
					     "amazing", "sad",	"omg" };
	guint i = 0;

	while (i < G_N_ELEMENTS(names) && strcmp(reaction, names[i]))
		i++;
	if (i == G_N_ELEMENTS(names))
		return problem(error, "Unknown reaction.");
	g_autoptr(J) req = obj();

	put_num(req, "1:i32", seq);
	put_str(req, "2:str", chat);
	put_str(req, "3:str", message_id);
	put_num(req, "4:i32", (gint64)(i + 1)); /* UNDO=1, NICE=2 .. OMG=7 */
	g_autoptr(J) ack = square_rpc(s, "reactToMessage", req, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put_str(out, "id", message_id);
	return out;
}

J *line_square_unsend(Line *s, const char *chat, const char *message_id,
		      GError **error)
{
	if (!line_square_id(chat, 'm') || !decimal_id(message_id))
		return problem(error, "Select a message to unsend.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat);
	put_str(req, "3:str", message_id);
	g_autoptr(J) ack = square_rpc(s, "unsendMessage", req, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put_str(out, "id", message_id);
	return out;
}

J *line_square_destroy_message(Line *s, const char *chat,
			       const char *message_id, GError **error)
{
	if (!line_square_id(chat, 'm') || !decimal_id(message_id))
		return problem(error, "Select a message to remove.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat);
	put_str(req, "4:str", message_id);
	g_autoptr(J) ack = square_rpc(s, "destroyMessage", req, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put_str(out, "id", message_id);
	return out;
}

J *line_square_mark_read(Line *s, const char *chat, const char *message_id,
			 GError **error)
{
	if (!line_square_id(chat, 'm') || !decimal_id(message_id))
		return problem(error, "Invalid OpenChat read receipt.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat);
	put_str(req, "4:str", message_id);
	g_autoptr(J) ack = square_rpc(s, "markAsRead", req, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put_str(out, "chat", chat);
	put_str(out, "id", message_id);
	return out;
}

static J *square_announcement_row(J *entry, gboolean can_manage)
{
	J *seq = field(entry, "1");

	if (!json_object_is_type(seq, json_type_int) ||
	    json_object_get_int64(seq) <= 0)
		return NULL;
	J *txt_contents = field(field(entry, "3"), "1");
	const char *text = nonempty(txt_contents, "2");
	J *row = obj();

	put_str(row, "id", json_object_get_string(seq));
	put_str(row, "text", text ? text : "OpenChat announcement");
	put_bool(row, "canRemove", can_manage);
	return row;
}

J *line_square_announcements(Line *s, const char *chat, gboolean can_manage,
			     GError **error)
{
	if (!line_square_id(chat, 'm'))
		return problem(error, "Choose an OpenChat conversation.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat);
	g_autoptr(J) reply =
		square_rpc(s, "getSquareChatAnnouncements", req, error);
	if (!reply)
		return NULL;
	J *list = field(reply, "1");
	J *rows = array();

	for (size_t i = 0; i < MIN(length(list), 50); i++) {
		J *row = square_announcement_row(at(list, i), can_manage);

		if (row)
			json_object_array_add(rows, row);
	}
	return rows;
}

J *line_square_announce_message(Line *s, const char *chat,
				const char *message_id, const char *text,
				const char *sender_member_id, gint64 created_at,
				gint32 seq, GError **error)
{
	if (!line_square_id(chat, 'm') || !decimal_id(message_id) || !text ||
	    !*text || seq <= 0)
		return problem(error, "Choose a text message to announce.");
	g_autoptr(J) req = obj();
	J *ann = obj(), *contents = obj(), *txt = obj();

	put_str(txt, "1:str", message_id);
	put_str(txt, "2:str", text);
	if (sender_member_id && line_square_id(sender_member_id, 'p'))
		put_str(txt, "3:str", sender_member_id);
	put_num(txt, "4:i64", created_at);
	put(contents, "1:obj", txt);
	put_num(ann, "2:i32", 0); /* TEXT_MESSAGE */
	put(ann, "3:obj", contents);
	put_num(req, "1:i32", seq);
	put_str(req, "2:str", chat);
	put(req, "3:obj", ann);
	g_autoptr(J) reply =
		square_rpc(s, "createSquareChatAnnouncement", req, error);
	if (!reply)
		return NULL;
	J *row = square_announcement_row(field(reply, "1"), TRUE);

	return row ? row :
		     problem(error,
			     "Check the announcements list before retrying.");
}

J *line_square_remove_announcement(Line *s, const char *chat, const char *id,
				   GError **error)
{
	guint64 numeric;

	if (!line_square_id(chat, 'm') || !id ||
	    !g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &numeric, NULL))
		return problem(error, "Select an announcement to remove.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat);
	put_num(req, "3:i64", (gint64)numeric);
	return square_rpc(s, "deleteSquareChatAnnouncement", req, error);
}

J *line_square_categories(Line *s, GError **error)
{
	g_autoptr(J) reply = square_rpc(s, "getSquareCategories", NULL, error);
	if (!reply)
		return NULL;
	J *list = field(reply, "1");
	J *out = array();

	for (size_t i = 0; i < length(list); i++) {
		J *cat = at(list, i);
		gint64 id = json_object_get_int64(field(cat, "1"));
		const char *name = nonempty(cat, "2");

		if (id > 0 && name) {
			J *row = obj();

			put_num(row, "id", id);
			put_str(row, "name", name);
			json_object_array_add(out, row);
		}
	}
	return out;
}

static const char *join_method_name(int type)
{
	switch (type) {
	case SQ_JOIN_APPROVAL:
		return "approval";
	case SQ_JOIN_CODE:
		return "code";
	default:
		return "open";
	}
}

static J *square_summary_object(Line *s, J *sq, J *member, J *auth, J *status,
				J *chat)
{
	const char *mid = nonempty(sq, "1");

	if (!line_square_id(mid, 's'))
		return NULL;
	J *out = obj();
	const char *hash = nonempty(sq, "4");

	remember_square_avatar(s, mid, hash);
	put_str(out, "squareMid", mid);
	put_str(out, "name", nonempty(sq, "2") ? nonempty(sq, "2") : mid);
	put_str(out, "description", nonempty(sq, "5") ? nonempty(sq, "5") : "");
	put_str(out, "welcome", nonempty(sq, "3") ? nonempty(sq, "3") : "");
	put_bool(out, "searchable", json_object_get_boolean(field(sq, "6")));
	put_num(out, "categoryId", json_object_get_int64(field(sq, "8")));
	put_bool(out, "inviteEnabled",
		 json_object_get_boolean(field(sq, "11")));
	const char *invite_url = nonempty(sq, "9");

	if (invite_url)
		put_str(out, "invitationUrl", invite_url);
	g_autofree char *avatar = obs_avatar_path(hash);

	if (avatar)
		put_str(out, "avatarKey", avatar);

	J *jm = field(sq, "14");
	int jm_type = json_object_get_int(field(jm, "1"));

	put_str(out, "joinMethod", join_method_name(jm_type));
	const char *question = nonempty(field(field(jm, "2"), "1"), "1");

	if (question)
		put_str(out, "approvalQuestion", question);

	if (status) {
		put_num(out, "memberCount",
			json_object_get_int64(field(status, "1")));
		put_num(out, "joinRequestCount",
			json_object_get_int64(field(status, "2")));
		put_num(out, "openChatCount",
			json_object_get_int64(field(status, "4")));
	}
	if (chat && line_square_id(nonempty(chat, "1"), 'm')) {
		put_str(out, "chatMid", nonempty(chat, "1"));
		put_str(out, "chatName",
			nonempty(chat, "4") ? nonempty(chat, "4") :
					      nonempty(sq, "2"));
	}
	int my_role = 0;

	if (member && line_square_id(nonempty(member, "1"), 'p')) {
		const char *mmid = nonempty(member, "1");
		const char *mhash = nonempty(member, "4");

		remember_square_avatar(s, mmid, mhash);
		my_role = json_object_get_int(field(member, "8"));
		put_bool(out, "joined",
			 json_object_get_int(field(member, "7")) ==
				 SQ_STATE_JOINED);
		put_str(out, "myMemberId", mmid);
		put_str(out, "myName",
			nonempty(member, "3") ? nonempty(member, "3") : "");
		put_str(out, "myBio",
			nonempty(member, "13") ? nonempty(member, "13") : "");
		put_str(out, "myRole", role_name(my_role));
		g_autofree char *mavatar = obs_avatar_path(mhash);

		if (mavatar)
			put_str(out, "myAvatarKey", mavatar);
	}
	if (auth && my_role) {
		put_bool(out, "canEdit",
			 role_allows(my_role,
				     json_object_get_int(field(auth, "2"))));
		put_bool(out, "canApprove",
			 role_allows(my_role,
				     json_object_get_int(field(auth, "4"))));
		put_bool(out, "canCreateChat",
			 role_allows(my_role,
				     json_object_get_int(field(auth, "6"))));
		put_bool(out, "canRemoveMember",
			 role_allows(my_role,
				     json_object_get_int(field(auth, "8"))));
		put_bool(out, "canGrantRole",
			 role_allows(my_role,
				     json_object_get_int(field(auth, "9"))));
		put_bool(out, "canAnnounce",
			 role_allows(my_role,
				     json_object_get_int(field(auth, "12"))));
		put_bool(out, "canDeleteMessage",
			 my_role == SQ_ROLE_ADMIN ||
				 my_role == SQ_ROLE_CO_ADMIN);
	}
	return out;
}

J *line_square_find_ticket(Line *s, const char *input, GError **error)
{
	g_autofree char *ticket = line_square_parse_ticket(input);

	if (!ticket)
		return problem(
			error,
			"Paste a LINE OpenChat invite link (line.me/ti/g2/…) or ticket.");
	g_autoptr(J) req = obj();

	put_str(req, "1:str", ticket);
	g_autoptr(GError) v2_err = NULL;
	g_autoptr(J) reply =
		square_rpc(s, "findSquareByInvitationTicketV2", req, &v2_err);
	if (!reply) {
		if (!g_error_matches(v2_err, LINE_ERROR,
				     LINE_METHOD_UNAVAILABLE)) {
			g_propagate_error(error, g_steal_pointer(&v2_err));
			return NULL;
		}
		g_autoptr(J) v1_req = obj();

		put_str(v1_req, "2:str", ticket);
		reply = square_rpc(s, "findSquareByInvitationTicket", v1_req,
				   error);
		if (!reply)
			return NULL;
	}
	J *summary = square_summary_object(s, field(reply, "1"),
					   field(reply, "2"), field(reply, "3"),
					   field(reply, "4"),
					   field(reply, "7"));
	if (!summary)
		return problem(error, "LINE did not return OpenChat details.");
	return summary;
}

J *line_square_join(Line *s, const char *square_mid, const char *chat_mid,
		    const char *display_name, const char *join_code,
		    const char *approval_message, GError **error)
{
	if (!line_square_id(square_mid, 's'))
		return problem(error, "Choose an OpenChat to join.");
	if (chat_mid && *chat_mid && !line_square_id(chat_mid, 'm'))
		return problem(error, "Invalid OpenChat room.");
	g_autofree char *clean_name = clean_text_field(
		display_name, 20, FALSE, FALSE,
		"Enter an OpenChat nickname of up to 20 characters.", error);
	if (!clean_name)
		return NULL;
	if (join_code && *join_code) {
		g_autoptr(J) code_req = obj();

		put_str(code_req, "2:str", square_mid);
		put_str(code_req, "3:str", join_code);
		g_autoptr(J) code_ack =
			square_rpc(s, "checkJoinCode", code_req, error);
		if (!code_ack)
			return NULL;
	}
	g_autoptr(J) req = obj();
	J *member = obj();

	put_str(member, "2:str", square_mid);
	put_str(member, "3:str", clean_name);
	put_bool(member, "5:bool", TRUE); /* ableToReceiveMessage */
	put_str(req, "2:str", square_mid);
	put(req, "3:obj", member);
	if (chat_mid && *chat_mid)
		put_str(req, "4:str", chat_mid);
	if ((join_code && *join_code) ||
	    (approval_message && *approval_message)) {
		J *jv = obj();

		if (join_code && *join_code) {
			J *cv = obj();

			put_str(cv, "1:str", join_code);
			put(jv, "2:obj", cv);
		} else {
			J *av = obj();

			put_str(av, "1:str", approval_message);
			put(jv, "1:obj", av);
		}
		put(req, "5:obj", jv);
	}
	g_autoptr(J) reply = square_rpc(s, "joinSquare", req, error);
	if (!reply)
		return NULL;
	J *sq = field(reply, "1");
	J *mem = field(reply, "4");
	J *chat = field(reply, "7");
	J *st = field(reply, "8");
	J *out = obj();

	put_bool(out, "pending",
		 json_object_get_int(field(mem, "7")) ==
			 SQ_STATE_JOIN_REQUESTED);
	if (chat && line_square_id(nonempty(chat, "1"), 'm')) {
		J *row = make_chat_row(s, chat, st, sq, mem);

		if (row)
			put(out, "chat", row);
	}
	if (sq)
		put(out, "rawSquare", json_object_get(sq));
	if (mem)
		put(out, "rawMember", json_object_get(mem));
	if (field(reply, "2"))
		put(out, "rawAuthority", json_object_get(field(reply, "2")));
	return out;
}

J *line_square_join_chat(Line *s, const char *chat_mid, GError **error)
{
	if (!line_square_id(chat_mid, 'm'))
		return problem(error, "Choose an OpenChat room to join.");
	g_autoptr(J) req = obj();

	put_str(req, "1:str", chat_mid);
	g_autoptr(J) reply = square_rpc(s, "joinSquareChat", req, error);
	if (!reply)
		return NULL;
	J *chat = field(reply, "1");
	J *st = field(reply, "2");
	J *row = make_chat_row(s, chat, st, NULL, NULL);

	return row ? row : problem(error, "LINE did not confirm joining.");
}

J *line_square_leave(Line *s, const char *square_mid, GError **error)
{
	if (!line_square_id(square_mid, 's'))
		return problem(error, "Choose an OpenChat to leave.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", square_mid);
	g_autoptr(J) ack = square_rpc(s, "leaveSquare", req, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put_str(out, "squareMid", square_mid);
	return out;
}

J *line_square_leave_chat(Line *s, const char *chat_mid, gint64 member_rev,
			  GError **error)
{
	if (!line_square_id(chat_mid, 'm'))
		return problem(error, "Choose an OpenChat room to leave.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", chat_mid);
	put_bool(req, "3:bool", TRUE);
	put_num(req, "4:i64", member_rev);
	g_autoptr(J) ack = square_rpc(s, "leaveSquareChat", req, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put_str(out, "chat", chat_mid);
	return out;
}

J *line_square_create(Line *s, const char *name, const char *description,
		      const char *welcome, const char *display_name,
		      int category_id, gboolean searchable, int join_method,
		      const char *join_code, const char *approval_question,
		      gint32 seq, GError **error)
{
	if (seq <= 0)
		return problem(error,
			       "Pair a new session to create OpenChats.");
	g_autofree char *clean_name = clean_text_field(
		name, 50, FALSE, FALSE,
		"Enter an OpenChat name of up to 50 characters.", error);
	if (!clean_name)
		return NULL;
	g_autofree char *clean_desc = clean_text_field(
		description, 500, TRUE, TRUE,
		"Enter a description of up to 500 characters.", error);
	if (!clean_desc)
		return NULL;
	g_autofree char *clean_welcome = clean_text_field(
		welcome, 500, TRUE, TRUE,
		"Enter a welcome message of up to 500 characters.", error);
	if (!clean_welcome)
		return NULL;
	g_autofree char *clean_nick = clean_text_field(
		display_name, 20, FALSE, FALSE,
		"Enter your OpenChat nickname (up to 20 characters).", error);
	if (!clean_nick)
		return NULL;
	if (join_method == SQ_JOIN_CODE &&
	    (!join_code || strlen(join_code) < 4 || strlen(join_code) > 32))
		return problem(error,
			       "Enter a join code of 4 to 32 characters.");
	g_autoptr(J) req = obj();
	J *sq = obj(), *creator = obj(), *jm = obj();

	put_str(sq, "2:str", clean_name);
	if (*clean_welcome)
		put_str(sq, "3:str", clean_welcome);
	if (*clean_desc)
		put_str(sq, "5:str", clean_desc);
	put_bool(sq, "6:bool", searchable);
	put_num(sq, "7:i32", 1); /* SquareType.OPEN */
	put_num(sq, "8:i32", category_id > 0 ? category_id : 1);
	put_bool(sq, "11:bool", TRUE); /* ableToUseInvitationTicket */
	put_num(jm, "1:i32",
		join_method >= 0 && join_method <= 2 ? join_method : 0);
	if (join_method == SQ_JOIN_CODE) {
		J *jv = obj(), *cv = obj();

		put_str(cv, "1:str", join_code);
		put(jv, "2:obj", cv);
		put(jm, "2:obj", jv);
	} else if (join_method == SQ_JOIN_APPROVAL) {
		J *jv = obj(), *av = obj();

		put_str(av, "1:str",
			approval_question && *approval_question ?
				approval_question :
				"Why would you like to join?");
		put(jv, "1:obj", av);
		put(jm, "2:obj", jv);
	}
	put(sq, "14:obj", jm);

	put_str(creator, "3:str", clean_nick);
	put_bool(creator, "5:bool", TRUE);

	put_num(req, "1:i32", seq);
	put(req, "2:obj", sq);
	put(req, "3:obj", creator);
	g_autoptr(J) reply = square_rpc(s, "createSquare", req, error);
	if (!reply)
		return NULL;
	J *created_sq = field(reply, "1");
	J *created_mem = field(reply, "2");
	J *created_chat = field(reply, "7");
	J *chat_st = field(reply, "8");
	J *row = make_chat_row(s, created_chat, chat_st, created_sq,
			       created_mem);
	if (!row)
		return problem(
			error,
			"LINE did not confirm the new OpenChat. Refresh Chats before trying again.");
	J *out = obj();

	put(out, "chat", row);
	put(out, "rawSquare", json_object_get(created_sq));
	put(out, "rawMember", json_object_get(created_mem));
	if (field(reply, "3"))
		put(out, "rawAuthority", json_object_get(field(reply, "3")));
	return out;
}

J *line_square_create_chat(Line *s, const char *square_mid, const char *name,
			   gint32 seq, GError **error)
{
	if (!line_square_id(square_mid, 's') || seq <= 0)
		return problem(error, "Choose an OpenChat first.");
	g_autofree char *clean = clean_text_field(
		name, 50, FALSE, FALSE,
		"Enter a chat room name of up to 50 characters.", error);
	if (!clean)
		return NULL;
	g_autoptr(J) req = obj();
	J *chat = obj();

	put_str(chat, "2:str", square_mid);
	put_num(chat, "3:i32", 1); /* SquareChatType.OPEN */
	put_str(chat, "4:str", clean);
	put_num(req, "1:i32", seq);
	put(req, "2:obj", chat);
	put(req, "3:list:str", array());
	g_autoptr(J) reply = square_rpc(s, "createSquareChat", req, error);
	if (!reply)
		return NULL;
	J *row = make_chat_row(s, field(reply, "1"), field(reply, "2"), NULL,
			       NULL);
	return row ? row : problem(error, "LINE did not confirm the new room.");
}

J *line_square_detail(Line *s, const char *square_mid, const char *chat_mid,
		      GError **error)
{
	if (!line_square_id(square_mid, 's'))
		return problem(error, "Choose an OpenChat first.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", square_mid);
	g_autoptr(J) reply = square_rpc(s, "getSquare", req, error);
	if (!reply)
		return NULL;
	J *sq = field(reply, "1");
	J *mem = field(reply, "2");
	J *auth = field(reply, "3");
	J *st = field(reply, "4");
	J *chat = NULL;

	g_autoptr(J) chat_reply = NULL;

	if (chat_mid && line_square_id(chat_mid, 'm')) {
		g_autoptr(J) creq = obj();

		put_str(creq, "1:str", chat_mid);
		g_autoptr(GError) ignored = NULL;
		chat_reply = square_rpc(s, "getSquareChat", creq, &ignored);
		chat = field(chat_reply, "1");
	}
	J *out = square_summary_object(s, sq, mem, auth, st, chat);

	if (!out)
		return problem(error, "LINE did not return OpenChat details.");
	/* Also list joinable sub-chats inside this OpenChat. */
	g_autoptr(J) jreq = obj();

	put_str(jreq, "1:str", square_mid);
	put_num(jreq, "11:i32", 50);
	g_autoptr(GError) ignored = NULL;
	g_autoptr(J) jreply =
		square_rpc(s, "getJoinableSquareChats", jreq, &ignored);
	J *subchats = array();
	J *jlist = field(jreply, "1");
	J *jstatuses = field(jreply, "4");

	for (size_t i = 0; i < length(jlist); i++) {
		J *sc = at(jlist, i);
		const char *cid = nonempty(sc, "1");

		if (!line_square_id(cid, 'm'))
			continue;
		J *row = make_chat_row(s, sc, field(jstatuses, cid), sq, mem);

		if (row)
			json_object_array_add(subchats, row);
	}
	put(out, "subchats", subchats);
	if (sq)
		put(out, "rawSquare", json_object_get(sq));
	if (mem)
		put(out, "rawMember", json_object_get(mem));
	if (auth)
		put(out, "rawAuthority", json_object_get(auth));
	if (chat)
		put(out, "rawChat", json_object_get(chat));
	return out;
}

J *line_square_invite_link(Line *s, const char *mid, GError **error)
{
	if (!line_square_id(mid, 's') && !line_square_id(mid, 'm'))
		return problem(error, "Choose an OpenChat first.");
	g_autoptr(J) req = obj();

	put_str(req, "2:str", mid);
	g_autoptr(J) reply =
		square_rpc(s, "getInvitationTicketUrl", req, error);
	if (!reply)
		return NULL;
	const char *url = nonempty(reply, "1");

	if (!url || (!g_str_has_prefix(url, "https://") &&
		     !g_str_has_prefix(url, "http://")))
		return problem(
			error,
			"Invite links are not available for this OpenChat.");
	J *out = obj();

	put_str(out, "url", url);
	return out;
}

J *line_square_update(Line *s, J *current_square, J *params, GError **error)
{
	const char *mid = nonempty(current_square, "1");

	if (!line_square_id(mid, 's'))
		return problem(error, "Refresh the OpenChat before editing.");
	g_autoptr(J) req = obj();
	J *attrs = array(), *sq = obj();

	put_str(sq, "1:str", mid);
	put_num(sq, "10:i64",
		json_object_get_int64(field(current_square, "10")));
	if (field(params, "name")) {
		g_autofree char *clean = clean_text_field(
			strfield(params, "name"), 50, FALSE, FALSE,
			"Enter an OpenChat name of up to 50 characters.",
			error);
		if (!clean)
			return NULL;
		put_str(sq, "2:str", clean);
		json_object_array_add(attrs, json_object_new_int(SQ_ATTR_NAME));
	}
	if (field(params, "description")) {
		g_autofree char *clean = clean_text_field(
			strfield(params, "description"), 500, TRUE, TRUE,
			"Enter a description of up to 500 characters.", error);
		if (!clean)
			return NULL;
		put_str(sq, "5:str", clean);
		json_object_array_add(attrs, json_object_new_int(SQ_ATTR_DESC));
	}
	if (field(params, "welcome")) {
		g_autofree char *clean = clean_text_field(
			strfield(params, "welcome"), 500, TRUE, TRUE,
			"Enter a welcome message of up to 500 characters.",
			error);
		if (!clean)
			return NULL;
		put_str(sq, "3:str", clean);
		json_object_array_add(attrs,
				      json_object_new_int(SQ_ATTR_WELCOME));
	}
	if (json_object_is_type(field(params, "searchable"),
				json_type_boolean)) {
		put_bool(sq, "6:bool",
			 json_object_get_boolean(field(params, "searchable")));
		json_object_array_add(attrs,
				      json_object_new_int(SQ_ATTR_SEARCHABLE));
	}
	if (json_object_is_type(field(params, "inviteEnabled"),
				json_type_boolean)) {
		put_bool(sq, "11:bool",
			 json_object_get_boolean(
				 field(params, "inviteEnabled")));
		json_object_array_add(
			attrs, json_object_new_int(SQ_ATTR_ABLE_TO_USE_INVITE));
	}
	if (!length(attrs)) {
		json_object_put(attrs);
		json_object_put(sq);
		return problem(error, "No OpenChat changes to save.");
	}
	put(req, "2:set:i32", attrs);
	put(req, "3:obj", sq);
	g_autoptr(J) reply = square_rpc(s, "updateSquare", req, error);
	if (!reply)
		return NULL;
	J *updated = field(reply, "2");
	J *out = obj();

	if (updated) {
		put(out, "rawSquare", json_object_get(updated));
		put_str(out, "name",
			nonempty(updated, "2") ? nonempty(updated, "2") : mid);
	}
	return out;
}

J *line_square_update_chat(Line *s, J *current_chat, const char *name,
			   GError **error)
{
	const char *cid = nonempty(current_chat, "1");
	const char *smid = nonempty(current_chat, "2");

	if (!line_square_id(cid, 'm') || !line_square_id(smid, 's'))
		return problem(error, "Refresh the OpenChat before renaming.");
	g_autofree char *clean = clean_text_field(
		name, 50, FALSE, FALSE,
		"Enter a room name of up to 50 characters.", error);
	if (!clean)
		return NULL;
	g_autoptr(J) req = obj();
	J *attrs = array(), *chat = obj();

	json_object_array_add(attrs, json_object_new_int(SQ_CHAT_ATTR_NAME));
	put_str(chat, "1:str", cid);
	put_str(chat, "2:str", smid);
	put_str(chat, "4:str", clean);
	put_num(chat, "6:i64", json_object_get_int64(field(current_chat, "6")));
	put(req, "2:set:i32", attrs);
	put(req, "3:obj", chat);
	g_autoptr(J) reply = square_rpc(s, "updateSquareChat", req, error);
	if (!reply)
		return NULL;
	J *out = obj();

	put_str(out, "id", cid);
	put_str(out, "name", clean);
	return out;
}

J *line_square_update_my_profile(Line *s, J *my_member,
				 const char *display_name, const char *bio,
				 const char *obs_hash, GError **error)
{
	const char *mmid = nonempty(my_member, "1");
	const char *smid = nonempty(my_member, "2");

	if (!line_square_id(mmid, 'p') || !line_square_id(smid, 's'))
		return problem(error,
			       "Refresh this OpenChat to edit your profile.");
	g_autoptr(J) req = obj();
	J *attrs = array(), *mem = obj();

	put_str(mem, "1:str", mmid);
	put_str(mem, "2:str", smid);
	put_num(mem, "9:i64", json_object_get_int64(field(my_member, "9")));
	if (display_name) {
		g_autofree char *clean = clean_text_field(
			display_name, 20, FALSE, FALSE,
			"Enter an OpenChat nickname of up to 20 characters.",
			error);
		if (!clean)
			return NULL;
		put_str(mem, "3:str", clean);
		json_object_array_add(
			attrs,
			json_object_new_int(SQ_MEMBER_ATTR_DISPLAY_NAME));
	}
	if (bio) {
		g_autofree char *clean = clean_text_field(
			bio, 200, TRUE, TRUE,
			"Enter a bio of up to 200 characters.", error);
		if (!clean)
			return NULL;
		put_str(mem, "13:str", clean);
		json_object_array_add(
			attrs, json_object_new_int(SQ_MEMBER_ATTR_SELF_INTRO));
	}
	if (obs_hash && valid_obs_hash(obs_hash)) {
		put_str(mem, "4:str", obs_hash);
		json_object_array_add(
			attrs,
			json_object_new_int(SQ_MEMBER_ATTR_PROFILE_IMAGE));
	}
	if (!length(attrs)) {
		json_object_put(attrs);
		json_object_put(mem);
		return problem(error, "No profile changes to save.");
	}
	put(req, "2:set:i32", attrs);
	put(req, "3:set:i32", array());
	put(req, "4:obj", mem);
	g_autoptr(J) reply = square_rpc(s, "updateSquareMember", req, error);
	if (!reply)
		return NULL;
	J *updated = field(reply, "2");
	J *out = obj();

	if (updated) {
		remember_square_avatar(s, mmid, nonempty(updated, "4"));
		put(out, "rawMember", json_object_get(updated));
		put_str(out, "myName",
			nonempty(updated, "3") ? nonempty(updated, "3") : "");
		put_str(out, "myBio",
			nonempty(updated, "13") ? nonempty(updated, "13") : "");
		g_autofree char *av = obs_avatar_path(nonempty(updated, "4"));

		if (av)
			put_str(out, "myAvatarKey", av);
	}
	return out;
}

J *line_square_update_picture(Line *s, J *my_member, const char *path,
			      GError **error)
{
	const char *mmid = nonempty(my_member, "1");

	if (!line_square_id(mmid, 'p'))
		return problem(error,
			       "Refresh this OpenChat to change your photo.");
	if (!path || !g_path_is_absolute(path))
		return problem(error, "Choose a JPEG, PNG or WebP picture.");
	g_autoptr(GBytes) jpeg = line_profile_image_prepare(path, error);

	if (!jpeg)
		return NULL;
	g_autoptr(LineSecret) token = line_obs_token(s, error);

	if (!token)
		return NULL;
	g_autofree char *hash = line_obs_upload_square_profile(
		token, "member", mmid, jpeg, error);
	if (!hash)
		return NULL;
	return line_square_update_my_profile(s, my_member, NULL, NULL, hash,
					     error);
}

static J *member_row(Line *s, J *m)
{
	const char *id = nonempty(m, "1");

	if (!line_square_id(id, 'p'))
		return NULL;
	const char *name = nonempty(m, "3");
	const char *hash = nonempty(m, "4");

	remember_square_avatar(s, id, hash);
	J *row = obj();

	put_str(row, "id", id);
	put_str(row, "name", name ? name : id);
	put_str(row, "role", role_name(json_object_get_int(field(m, "8"))));
	put_num(row, "state", json_object_get_int64(field(m, "7")));
	put_num(row, "revision", json_object_get_int64(field(m, "9")));
	const char *join_msg = nonempty(m, "11");

	if (join_msg)
		put_str(row, "joinMessage", join_msg);
	const char *bio = nonempty(m, "13");

	if (bio)
		put_str(row, "bio", bio);
	g_autofree char *av = obs_avatar_path(hash);

	if (av)
		put_str(row, "avatarKey", av);
	return row;
}

J *line_square_members(Line *s, const char *square_mid,
		       const char *continuation, const char *query,
		       gboolean pending_only, GError **error)
{
	if (!line_square_id(square_mid, 's'))
		return problem(error, "Choose an OpenChat first.");
	g_autoptr(J) req = obj();
	J *opt = obj();

	put_num(opt, "1:i32",
		pending_only ? SQ_STATE_JOIN_REQUESTED : SQ_STATE_JOINED);
	if (query && *query && g_utf8_validate(query, -1, NULL))
		put_str(opt, "3:str", query);
	put_bool(opt, "7:bool", !pending_only); /* includingMe */
	put_str(req, "2:str", square_mid);
	put(req, "3:obj", opt);
	if (continuation && *continuation)
		put_str(req, "4:str", continuation);
	put_num(req, "5:i32", SQUARE_MEMBER_LIMIT);
	g_autoptr(J) reply = square_rpc(s, "searchSquareMembers", req, error);
	if (!reply)
		return NULL;
	J *list = field(reply, "1");
	J *rows = array();

	for (size_t i = 0; i < length(list); i++) {
		J *row = member_row(s, at(list, i));

		if (row)
			json_object_array_add(rows, row);
	}
	J *out = obj();

	put(out, "members", rows);
	put_num(out, "total", json_object_get_int64(field(reply, "4")));
	const char *next = nonempty(reply, "3");

	if (next)
		put_str(out, "cursor", next);
	return out;
}

J *line_square_moderate_member(Line *s, const char *square_mid,
			       const char *member_mid, const char *action,
			       GError **error)
{
	if (!line_square_id(square_mid, 's') ||
	    !line_square_id(member_mid, 'p') || !action)
		return problem(error, "Choose an OpenChat member.");
	/* Fetch current member revision first so LINE accepts the update. */
	g_autoptr(J) get_req = obj();

	put_str(get_req, "2:str", member_mid);
	g_autoptr(J) got = square_rpc(s, "getSquareMember", get_req, error);
	if (!got)
		return NULL;
	J *cur = field(got, "1");
	gint64 rev = json_object_get_int64(field(cur, "9"));

	g_autoptr(J) req = obj();
	J *attrs = array(), *mem = obj();

	put_str(mem, "1:str", member_mid);
	put_str(mem, "2:str", square_mid);
	put_num(mem, "9:i64", rev);
	if (!strcmp(action, "promote")) {
		json_object_array_add(attrs,
				      json_object_new_int(SQ_MEMBER_ATTR_ROLE));
		put_num(mem, "8:i32", SQ_ROLE_CO_ADMIN);
	} else if (!strcmp(action, "demote")) {
		json_object_array_add(attrs,
				      json_object_new_int(SQ_MEMBER_ATTR_ROLE));
		put_num(mem, "8:i32", SQ_ROLE_MEMBER);
	} else if (!strcmp(action, "kick")) {
		json_object_array_add(
			attrs,
			json_object_new_int(SQ_MEMBER_ATTR_MEMBERSHIP_STATE));
		put_num(mem, "7:i32", SQ_STATE_KICK_OUT);
	} else if (!strcmp(action, "ban")) {
		json_object_array_add(
			attrs,
			json_object_new_int(SQ_MEMBER_ATTR_MEMBERSHIP_STATE));
		put_num(mem, "7:i32", SQ_STATE_BANNED);
	} else {
		json_object_put(attrs);
		json_object_put(mem);
		return problem(error, "Unknown OpenChat moderation action.");
	}
	put(req, "2:set:i32", attrs);
	put(req, "3:set:i32", array());
	put(req, "4:obj", mem);
	g_autoptr(J) reply = square_rpc(s, "updateSquareMember", req, error);
	if (!reply)
		return NULL;
	J *out = obj();

	put_str(out, "id", member_mid);
	put_str(out, "action", action);
	return out;
}

J *line_square_answer_join_request(Line *s, const char *square_mid,
				   const char *member_mid, gboolean approve,
				   GError **error)
{
	if (!line_square_id(square_mid, 's') ||
	    !line_square_id(member_mid, 'p'))
		return problem(error, "Choose a pending join request.");
	g_autoptr(J) req = obj();
	J *mids = array();

	json_object_array_add(mids, string(member_mid));
	put_str(req, "2:str", square_mid);
	put(req, "3:list:str", mids);
	g_autoptr(J) reply = square_rpc(
		s, approve ? "approveSquareMembers" : "rejectSquareMembers",
		req, error);
	if (!reply)
		return NULL;
	J *out = obj();

	put_str(out, "id", member_mid);
	put_bool(out, "approved", approve);
	return out;
}

J *line_square_poll_events(Line *s, gint64 *subscription_id, char **sync_token,
			   J *my_members_by_square, GError **error)
{
	g_autoptr(J) req = obj();

	if (subscription_id && *subscription_id > 0)
		put_num(req, "1:i64", *subscription_id);
	if (sync_token && *sync_token && **sync_token)
		put_str(req, "2:str", *sync_token);
	put_num(req, "3:i32", SQUARE_HISTORY_LIMIT);
	g_autoptr(J) reply = square_rpc(s, "fetchMyEvents", req, error);
	if (!reply)
		return NULL;
	J *sub = field(reply, "1");
	gint64 sid = json_object_get_int64(field(sub, "1"));

	if (subscription_id && sid > 0)
		*subscription_id = sid;
	const char *next_sync = nonempty(reply, "3");

	if (sync_token && next_sync) {
		g_free(*sync_token);
		*sync_token = g_strdup(next_sync);
	}
	J *events = field(reply, "2");
	J *out = array();

	for (size_t i = 0; i < length(events); i++) {
		J *ev = at(events, i);
		int type = json_object_get_int(field(ev, "3"));
		J *payload = field(ev, "4");
		J *item = obj();

		if (type == SQ_EVT_NOTIF_MESSAGE) {
			J *p = field(payload, "30");
			const char *cid = nonempty(p, "1");
			J *msg = field(field(p, "2"), "1");
			const char *mid = nonempty(msg, "4");
			const char *sender = nonempty(msg, "1");
			gint64 t = json_object_get_int64(field(msg, "5"));

			if (!line_square_id(cid, 'm')) {
				json_object_put(item);
				continue;
			}
			gboolean mine = FALSE;

			if (sender && my_members_by_square) {
				json_object_object_foreach(my_members_by_square,
							   k, v) {
					(void)k;
					if (!g_strcmp0(nonempty(v, "1"),
						       sender))
						mine = TRUE;
				}
			}
			put_str(item, "kind", "message");
			put_str(item, "chat", cid);
			if (mid)
				put_str(item, "id", mid);
			put_num(item, "time", t);
			put_bool(item, "mine", mine);
		} else if (type == SQ_EVT_UPDATE_CHAT_STATUS) {
			const char *cid = nonempty(field(payload, "15"), "1");

			put_str(item, "kind", "reload");
			if (cid)
				put_str(item, "chat", cid);
		} else if (type == SQ_EVT_UPDATE_SQUARE ||
			   type == SQ_EVT_UPDATE_CHAT ||
			   type == SQ_EVT_UPDATE_MEMBER ||
			   type == SQ_EVT_UPDATE_CHAT_MEMBER ||
			   type == SQ_EVT_UPDATE_AUTHORITY ||
			   type == SQ_EVT_SHUTDOWN_SQUARE ||
			   type == SQ_EVT_KICKOUT ||
			   type == SQ_EVT_DELETE_CHAT ||
			   type == SQ_EVT_NOTIF_JOINED ||
			   type == SQ_EVT_NOTIF_KICKED_OUT ||
			   type == SQ_EVT_NOTIF_SQUARE_DELETE ||
			   type == SQ_EVT_NOTIF_CHAT_DELETE ||
			   type == SQ_EVT_NOTIF_JOIN_REQUEST ||
			   type == SQ_EVT_UPDATE_ANNOUNCEMENT ||
			   type == SQ_EVT_NOTIF_REACTION ||
			   type == SQ_EVT_MUTATE_MESSAGE ||
			   type == SQ_EVT_DESTROY_MESSAGE) {
			put_str(item, "kind", "reload");
		} else {
			json_object_put(item);
			continue;
		}
		json_object_array_add(out, item);
	}
	return out;
}
