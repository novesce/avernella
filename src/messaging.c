// SPDX-License-Identifier: GPL-3.0-or-later
#include "messaging.h"
#include "sealing.h"
#include "emoji.h"
#include "rich_message.h"
#include <string.h>

enum {
	CONTACT_BATCH = 100,
	HISTORY_LIMIT = 50,
	FLEX_JSON_LIMIT = 64 * 1024,
	FLEX_ALT_TEXT_LIMIT = 8 * 1024,
	FLEX_DEPTH = 24
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

static void number(J *o, const char *k, gint64 n)
{
	put(o, k, json_object_new_int64(n));
}

static void boolean(J *o, const char *k, gboolean b)
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

static gboolean talk_id(const char *id)
{
	if (!id || strlen(id) != 33 || !strchr("ucr", id[0]))
		return FALSE;
	for (int i = 1; i < 33; i++)
		if (!g_ascii_isxdigit(id[i]))
			return FALSE;
	return TRUE;
}

enum {
	PROFILE_LIMIT = 512,
	PROFILE_TTL_SECONDS = 120
};

struct cached_profile_name {
	gint64 expires;
	char name[];
};

void line_profile_operation(Line *s, J *op)
{
	if (!s->profiles)
		return;
	/*
	 * A rename or avatar change may be presented by either the actor or the
	 * subject MID, depending on the operation. Refresh all affected metadata
	 * next time it is needed; ordinary message operations retain the cache.
	 */
	switch (json_object_get_int(field(op, "3"))) {
	case 1:
	case 2:
	case 10:
	case 11:
	case 49:
	case 78:
	case 121:
	case 122:
	case 141:
	case 150:
		g_hash_table_remove_all(s->profiles);
		break;
	}
}

static void cache_profile(Line *s, const char *id, const char *name)
{
	gsize n = strlen(name);

	if (n > 1024)
		return;
	if (!s->profiles)
		s->profiles = g_hash_table_new_full(g_str_hash, g_str_equal,
						    g_free, g_free);
	if (g_hash_table_size(s->profiles) >= PROFILE_LIMIT &&
	    !g_hash_table_contains(s->profiles, id)) {
		GHashTableIter it;

		g_hash_table_iter_init(&it, s->profiles);
		if (g_hash_table_iter_next(&it, NULL, NULL))
			g_hash_table_iter_remove(&it);
	}
	struct cached_profile_name *entry = g_malloc(sizeof(*entry) + n + 1);

	entry->expires = g_get_monotonic_time() +
			 (gint64)PROFILE_TTL_SECONDS * G_USEC_PER_SEC;
	memcpy(entry->name, name, n + 1);
	g_hash_table_replace(s->profiles, g_strdup(id), entry);
}

/*
 * A display name is optional: people who left, and accounts that cannot be
 * looked up, can still have readable history. Store only names for requested IDs.
 */
static void remember_profile(Line *s, J *names, const char *id,
			     const char *name, const char *path)
{
	if (!id || !field(names, id))
		return;
	if (name) {
		put(names, id, string(name));
		cache_profile(s, id, name);
	}
	if (s->pictures) {
		if (path && *path == '/' && strlen(path) <= 1024)
			put(s->pictures, id, string(path));
		else
			json_object_object_del(s->pictures, id);
	}
}

static void resolve_users(Line *s, J *mids, J *names)
{
	g_autoptr(J) args = obj();
	J *req = obj(), *targets = array();

	put(args, "1:obj", req);
	put(req, "1:list:obj", targets);
	number(req, "2:i32", 5); /* AUTO_REPAIR */
	for (size_t i = 0; i < length(mids); i++) {
		J *target = obj();

		put(target, "1:str", json_object_get(at(mids, i)));
		json_object_array_add(targets, target);
	}
	g_autoptr(GError) error = NULL;
	g_autoptr(J) reply = line_rpc(s, "/RE4", "getContactsV3", args, &error);
	if (reply) {
		J *users = field(reply, "1");

		for (size_t i = 0; i < length(users); i++) {
			J *u = at(users, i);
			const char *name =
				nonempty(field(field(u, "4"), "1"), "3");
			if (!name)
				name = nonempty(field(u, "3"), "2");
			remember_profile(s, names, nonempty(u, "1"), name,
					 nonempty(field(u, "3"), "3"));
		}
		return;
	}
	/* Some companion profiles only implement the older contacts service. */
	if (!g_error_matches(error, LINE_ERROR, LINE_METHOD_UNAVAILABLE))
		return;
	g_clear_error(&error);
	g_autoptr(J) v2 = obj();
	J *request = obj();

	put(v2, "1:obj", request);
	put(request, "1:list:str", json_object_get(mids));
	reply = line_rpc(s, "/S4", "getContactsV2", v2, &error);
	J *entries = field(reply, "1");

	if (!json_object_is_type(entries, json_type_object))
		return;
	json_object_object_foreach(entries, id, entry) {
		J *contact = field(entry, "3");
		const char *label = nonempty(contact, "27");

		if (!label)
			label = nonempty(contact, "22");
		remember_profile(s, names, id, label, nonempty(contact, "37"));
	}
}

static void resolve_groups(Line *s, J *mids, J *names)
{
	g_autoptr(J) args = obj();
	J *req = obj();

	put(args, "1:obj", req);
	put(req, "1:list:str", json_object_get(mids));
	boolean(req, "2:bool", FALSE);
	boolean(req, "3:bool", FALSE);
	number(args, "2:i32", 7); /* INTERNAL */
	g_autoptr(GError) error = NULL;
	g_autoptr(J) reply = line_rpc(s, "/S4", "getChats", args, &error);
	J *groups = field(reply, "1");

	for (size_t i = 0; i < length(groups); i++) {
		J *group = at(groups, i);

		remember_profile(s, names, nonempty(group, "2"),
				 nonempty(group, "6"), nonempty(group, "7"));
	}
}

void line_resolve_names(Line *s, J *names, char type)
{
	/* Snapshot IDs first: looking up names replaces values in names. */
	g_autoptr(J) ids = array();
	const gint64 now = g_get_monotonic_time();

	json_object_object_foreach(names, id, name) {
		if (id[0] != type || json_object_get_string_len(name))
			continue;
		const struct cached_profile_name *cached =
			s->profiles ? g_hash_table_lookup(s->profiles, id) :
				      NULL;
		if (cached && cached->expires > now)
			put(names, id, string(cached->name));
		else
			json_object_array_add(ids, string(id));
	}
	for (size_t start = 0; start < length(ids); start += CONTACT_BATCH) {
		g_autoptr(J) batch = array();
		for (size_t i = start;
		     i < MIN(length(ids), start + CONTACT_BATCH); i++)
			json_object_array_add(batch,
					      json_object_get(at(ids, i)));
		if (type == 'u')
			resolve_users(s, batch, names);
		else
			resolve_groups(s, batch, names);
	}
}

static int chat_order(const void *a, const void *b)
{
	J *lhs = *(J *const *)a, *rhs = *(J *const *)b;
	gint64 lt = json_object_get_int64(field(lhs, "time"));
	gint64 rt = json_object_get_int64(field(rhs, "time"));
	return lt == rt ? strcmp(strfield(lhs, "id"), strfield(rhs, "id")) :
			  (lt > rt ? -1 : 1);
}

J *line_chats(Line *s, GError **error)
{
	g_autoptr(J) summaries = obj(), names = obj();
	g_autoptr(GHashTable) cursors =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	const char *cursor = NULL;

	for (;;) {
		g_autoptr(J) args = obj();
		J *req = obj();

		put(args, "2:obj", req);
		number(args, "3:i32", 7);
		if (cursor)
			put(req, "1:str", string(cursor));
		boolean(req, "3:bool", TRUE); /* activeOnly */
		number(req, "4:i32", 100);
		boolean(req, "5:bool", TRUE); /* withUnreadCount */
		number(req, "6:i32", 0);
		boolean(req, "7:bool", FALSE);
		g_autoptr(J) reply =
			line_rpc(s, "/S4", "getMessageBoxes", args, error);
		if (!reply)
			return NULL;
		J *boxes = field(reply, "1");

		if (!json_object_is_type(boxes, json_type_array)) {
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"LINE returned an unreadable chat list.");
			return NULL;
		}
		for (size_t i = 0; i < length(boxes); i++) {
			J *box = at(boxes, i), *latest = field(box, "4");
			const char *id = nonempty(box, "1");
			gint64 last = json_object_get_int64(field(latest, "2"));
			gint64 hidden = json_object_get_int64(field(box, "10"));
			gint64 removed = json_object_get_int64(field(box, "8"));
			gint64 cleared = MAX(hidden, removed);

			if (!talk_id(id) || (cleared > 0 && last <= cleared))
				continue;
			gint64 time = json_object_get_int64(field(latest, "1"));
			J *prior = field(summaries, id);

			if (!prior ||
			    time > json_object_get_int64(
					   field(prior, "time")) ||
			    (time == json_object_get_int64(
					     field(prior, "time")) &&
			     last > json_object_get_int64(
					    field(prior, "lastId")))) {
				J *row = obj();

				put(row, "id", string(id));
				number(row, "time", time);
				if (cleared > 0)
					number(row, "clearThrough", cleared);
				number(row, "unread",
				       MAX((gint64)0,
					   json_object_get_int64(
						   field(box, "6"))));
				if (last > 0)
					put(row, "lastId",
					    string(json_object_get_string(
						    field(latest, "2"))));
				if (json_object_get_int64(field(box, "5")) > 0)
					put(row, "lastSeenId",
					    string(json_object_get_string(
						    field(box, "5"))));
				put(summaries, id, row);
			}
			put(names, id,
			    string(g_strcmp0(id, s->mid) == 0 ? "Keep Memo" :
								""));
		}
		if (!json_object_get_boolean(field(reply, "2")))
			break;
		const char *next =
			length(boxes) ?
				nonempty(at(boxes, length(boxes) - 1), "1") :
				NULL;
		if (!next || g_hash_table_contains(cursors, next)) {
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"LINE did not advance the chat list.");
			return NULL;
		}
		cursor = g_strdup(next);
		g_hash_table_add(cursors, (gpointer)cursor);
	}
	line_resolve_names(s, names, 'u');
	line_resolve_names(s, names, 'c');
	J *rows = array();

	json_object_object_foreach(summaries, id, row) {
		const char *name = nonempty(names, id);

		put(row, "name", string(name ? name : id));
		put(row, "avatarKey",
		    string(strfield(s->pictures, id) ?
				   strfield(s->pictures, id) :
				   ""));
		json_object_array_add(rows, json_object_get(row));
	}
	json_object_array_sort(rows, chat_order);
	return rows;
}

J *line_mark_read(Line *s, const char *chat, const char *id,
		  gint32 reserved_seq, GError **error)
{
	guint64 value;

	if (!talk_id(chat) || !g_strcmp0(chat, s->mid) || reserved_seq < 1 ||
	    !id || *id < '1' || *id > '9' ||
	    !g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &value, NULL)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Select the latest message in this conversation.");
		return NULL;
	}
	g_autoptr(J) args = obj();
	number(args, "1:i32", reserved_seq);
	put(args, "2:str", string(chat));
	put(args, "3:str", string(id));
	/* The desktop read operation omits the optional sessionId. */
	g_autoptr(J) ack = line_rpc(s, "/S4", "sendChatChecked", args, error);
	if (!ack)
		return NULL;
	if (!json_object_is_type(ack, json_type_object)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned an unreadable read receipt.");
		return NULL;
	}
	J *result = obj();

	put(result, "chat", string(chat));
	put(result, "id", string(id));
	return result;
}

J *line_peer_ranges(Line *s, const char *chat, GError **error)
{
	g_autoptr(J) args = obj();
	J *ids = array();

	json_object_array_add(ids, string(chat));
	put(args, "2:list:str", ids);
	number(args, "3:i32", 7);
	g_autoptr(J) reply =
		line_rpc(s, "/S4", "getMessageReadRange", args, error);
	for (size_t i = 0; i < length(reply); i++) {
		J *entry = at(reply, i), *ranges = field(entry, "2");

		if (!g_strcmp0(chat, strfield(entry, "1")) &&
		    json_object_is_type(ranges, json_type_object))
			return json_object_get(ranges);
	}
	return NULL;
}

static gboolean peer_reader(const char *self, const char *chat, const char *mid)
{
	return talk_id(mid) && mid[0] == 'u' && strcmp(mid, self) &&
	       (chat[0] != 'u' || !strcmp(mid, chat));
}

/* Remember new read notifications in the open chat, once per reader and ID. */
enum peer_notice line_peer_read_notice(const char *self, const char *chat,
				       J *notices, J *op)
{
	const char *reader = strfield(op, "11"), *id = strfield(op, "12");
	const char *target = strfield(op, "10");
	guint64 value;

	if (json_object_get_int(field(op, "3")) != 55 || !self ||
	    !talk_id(target) || !peer_reader(self, target, reader) || !id ||
	    !g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &value, NULL))
		return PEER_NOTICE_OTHER;
	if (!notices || g_strcmp0(target, chat))
		return PEER_NOTICE_UNCHANGED;
	/* A receipt can beat the message's arrival in the next history reply. */
	J *readers = field(notices, id);

	if (field(readers, reader))
		return PEER_NOTICE_UNCHANGED;

	if (!readers) {
		if (json_object_object_length(notices) >= 2 * HISTORY_LIMIT) {
			struct json_object_iterator oldest =
				json_object_iter_begin(notices);

			json_object_object_del(
				notices, json_object_iter_peek_name(&oldest));
		}
		readers = obj();
		put(notices, id, readers);
	}
	boolean(readers, reader, TRUE);
	return PEER_NOTICE_ADDED;
}

static gboolean range_covers(J *intervals, guint64 id)
{
	/* Some service versions encode a single entry directly. */
	gboolean single = json_object_is_type(intervals, json_type_object);
	size_t count = single ? 1 : length(intervals);

	for (size_t i = 0; i < count; i++) {
		J *range = single ? intervals : at(intervals, i);
		J *first = field(range, "1"), *last = field(range, "2");
		gint64 a = json_object_get_int64(first),
		       b = json_object_get_int64(last);

		if (json_object_is_type(first, json_type_int) &&
		    json_object_is_type(last, json_type_int) && a >= 0 &&
		    b >= 0 && id >= (guint64)MIN(a, b) &&
		    id <= (guint64)MAX(a, b))
			return TRUE;
	}
	return FALSE;
}

J *line_peer_read_counts(const char *self, const char *chat, J *messages,
			 J *ranges, J *notices)
{
	J *counts = obj();

	if (!self || !chat || !strcmp(self, chat) ||
	    !json_object_is_type(messages, json_type_object))
		return counts;
	json_object_object_foreach(messages, key, msg) {
		guint64 id;

		if (g_strcmp0(strfield(msg, "1"), self) ||
		    !g_ascii_string_to_unsigned(key, 10, 1, G_MAXINT64, &id,
						NULL))
			continue;
		J *seen = field(notices, key);
		int count = seen ? json_object_object_length(seen) : 0;

		if (ranges) {
			json_object_object_foreach(ranges, mid, intervals) {
				if (!field(seen, mid) &&
				    peer_reader(self, chat, mid) &&
				    range_covers(intervals, id))
					count++;
			}
		}
		J *explicit = field(msg, "23");

		if (json_object_is_type(explicit, json_type_int))
			count = MAX(count, json_object_get_int(explicit));
		if (count || ranges ||
		    (json_object_is_type(explicit, json_type_int) &&
		     json_object_get_int(explicit) >= 0))
			number(counts, key, count);
	}
	return counts;
}

static const char *content_label(gint64 n)
{
	static const char *const labels[] = { "[Empty message]",
					      "[Photo]",
					      "[Video]",
					      "[Audio]",
					      "[HTML]",
					      "[PDF]",
					      "Call",
					      "[Sticker]",
					      "[Presence]",
					      "[Gift]",
					      "[Group post]",
					      "[App link]",
					      "[Link]",
					      "[Contact]",
					      "[File]",
					      "[Location]",
					      "[Post]",
					      "[Rich message]",
					      "Chat activity",
					      "[Music]",
					      "[Payment]",
					      "[Photo]",
					      "[Flex message]" };
	return n >= 0 && n < (gint64)G_N_ELEMENTS(labels) ?
		       labels[n] :
		       "[Unsupported message]";
}

static int message_order(const void *a, const void *b)
{
	J *lhs = *(J *const *)a, *rhs = *(J *const *)b;
	gint64 lt = json_object_get_int64(field(lhs, "time"));
	gint64 rt = json_object_get_int64(field(rhs, "time"));

	if (lt != rt)
		return lt < rt ? -1 : 1;
	guint64 li = g_ascii_strtoull(strfield(lhs, "id"), NULL, 10);
	guint64 ri = g_ascii_strtoull(strfield(rhs, "id"), NULL, 10);

	return (li > ri) - (li < ri);
}

/* Embeddings, overrides, isolates and marks that reorder displayed text. */
static gboolean bidi_control(gunichar c)
{
	return (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069) ||
	       c == 0x200e || c == 0x200f || c == 0x061c;
}

gboolean line_safe_filename(const char *name)
{
	if (!name || !*name || !g_utf8_validate(name, -1, NULL) ||
	    strlen(name) > 255 || !strcmp(name, ".") || !strcmp(name, "..") ||
	    strpbrk(name, "/\\"))
		return FALSE;
	for (const char *p = name; *p; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_iscntrl(c) || bidi_control(c))
			return FALSE;
	}
	return TRUE;
}

/*
 * A hidden name like .bash_profile, saved into the home folder, could run
 * at the next login.
 */
gboolean line_received_filename(const char *name)
{
	return line_safe_filename(name) && *name != '.';
}

/*
 * LINE's sticon ranges refer to UTF-16 code units in the visible fallback
 * text. Advance once through the UTF-8 body for an ordered run of ranges.
 */
static gboolean emoji_boundary(const char **cursor, guint64 *position,
			       guint64 target)
{
	if (target < *position)
		return FALSE;
	while (*position < target && **cursor) {
		*position += g_utf8_get_char(*cursor) > 0xffff ? 2u : 1u;
		*cursor = g_utf8_next_char(*cursor);
	}
	return target == *position;
}

static gboolean emoji_integer(J *item, const char *field_name, guint64 *out)
{
	J *v = field(item, field_name);

	if (!json_object_is_type(v, json_type_int) ||
	    json_object_get_int64(v) < 0 ||
	    json_object_get_uint64(v) > G_MAXINT32)
		return FALSE;
	*out = json_object_get_uint64(v);
	return TRUE;
}

static const char *exact_text(J *item, const char *key)
{
	const char *p = strfield(item, key);

	return p && strlen(p) == (size_t)json_object_get_string_len(
					 field(item, key)) ?
		       p :
		       NULL;
}

enum chat_action {
	CHAT_UNKNOWN,
	CHAT_JOIN,
	CHAT_LEAVE,
	CHAT_INVITE,
	CHAT_CANCEL_INVITE,
	CHAT_REMOVE,
	CHAT_CREATE,
	CHAT_RENAME,
	CHAT_PHOTO,
	CHAT_LINK_ON,
	CHAT_LINK_OFF,
	CHAT_ANNOUNCE,
	CHAT_ALBUM_RENAME,
	CHAT_ALBUM_REMOVE,
	CHAT_ALBUM_PHOTOS_REMOVE,
	CHAT_INVITE_LIMIT,
	CHAT_INVITE_ENABLED,
	CHAT_TRANSFER_FAILED,
};

/* LINE Android 26.18.2, km8.s.d and rr8.f. */
static enum chat_action chat_action(J *metadata)
{
	const char *key = exact_text(metadata, "LOC_KEY");
	static const struct {
		const char *key;
		enum chat_action action;
	} formats[] = {
		{ "C_MJ", CHAT_JOIN },
		{ "A_MJ", CHAT_JOIN },
		{ "C_ML", CHAT_LEAVE },
		{ "A_ML", CHAT_LEAVE },
		{ "C_MI", CHAT_INVITE },
		{ "C_GI", CHAT_INVITE },
		{ "A_MI", CHAT_INVITE },
		{ "C_IC", CHAT_CANCEL_INVITE },
		{ "A_IC", CHAT_CANCEL_INVITE },
		{ "C_MR", CHAT_REMOVE },
		{ "A_MR", CHAT_REMOVE },
		{ "A_MC", CHAT_CREATE },
		{ "C_PN", CHAT_RENAME },
		{ "C_PI", CHAT_PHOTO },
		{ "C_SN", CHAT_LINK_ON },
		{ "C_SP", CHAT_LINK_OFF },
		{ "C_MA", CHAT_ANNOUNCE },
		{ "BB", CHAT_ALBUM_RENAME },
		{ "BD", CHAT_ALBUM_REMOVE },
		{ "BO", CHAT_ALBUM_PHOTOS_REMOVE },
		{ "C_OL", CHAT_INVITE_LIMIT },
		{ "C_BG", CHAT_INVITE_ENABLED },
		{ "S_NC", CHAT_TRANSFER_FAILED },
	};

	for (size_t i = 0; key && i < G_N_ELEMENTS(formats); i++)
		if (!strcmp(key, formats[i].key))
			return formats[i].action;
	return CHAT_UNKNOWN;
}

static char **chat_action_args(J *metadata)
{
	const char *text = exact_text(metadata, "LOC_ARGS");

	return text && strlen(text) <= 4096 && g_utf8_validate(text, -1, NULL) ?
		       g_strsplit(text, "\x1e", 6) :
		       g_new0(char *, 1);
}

static void add_notice_names(Line *s, J *names, const char *ids)
{
	if (!ids)
		return;
	g_auto(GStrv) members = g_strsplit(ids, "\x1f", 6);

	for (size_t i = 0; members[i]; i++)
		if (talk_id(members[i]) && *members[i] == 'u' &&
		    g_strcmp0(members[i], s->mid))
			put(names, members[i], string(""));
}

static char *notice_names(Line *s, J *names, const char *ids,
			  const char *fallback)
{
	g_auto(GStrv) members = ids ? g_strsplit(ids, "\x1f", 6) : NULL;
	g_autoptr(GString) text = g_string_new(NULL);

	for (size_t i = 0; members && members[i]; i++) {
		const char *id = members[i];

		if (!talk_id(id) || *id != 'u')
			continue;
		const char *name = !g_strcmp0(s->mid, id) ? "You" :
							    nonempty(names, id);

		if (text->len)
			g_string_append(text, ", ");
		g_string_append(text, name ? name : "A member");
	}
	return text->len ? g_string_free(g_steal_pointer(&text), FALSE) :
			   g_strdup(fallback);
}

static gboolean has_target_list(enum chat_action action)
{
	return action == CHAT_INVITE || action == CHAT_CANCEL_INVITE ||
	       action == CHAT_REMOVE;
}

static char *chat_notice(Line *s, J *names, J *meta, const char *sender,
			 const char *fallback)
{
	enum chat_action action = chat_action(meta);
	g_auto(GStrv) args = chat_action_args(meta);
	const char *second = args[0] ? args[1] : NULL;
	g_autofree char *actor = notice_names(s, names, args[0], sender);
	g_autofree char *targets =
		has_target_list(action) ?
			notice_names(s, names, second, "a member") :
			NULL;

	switch (action) {
	case CHAT_JOIN:
		return g_strdup_printf("%s joined the group", actor);
	case CHAT_LEAVE:
		return g_strdup_printf("%s left the group", actor);
	case CHAT_INVITE:
		return g_strdup_printf("%s invited %s to the group", actor,
				       targets);
	case CHAT_CANCEL_INVITE:
		return g_strdup_printf("%s canceled the invitation for %s",
				       actor, targets);
	case CHAT_REMOVE:
		return g_strdup_printf("%s removed %s from the group", actor,
				       targets);
	case CHAT_CREATE:
		return g_strdup_printf("%s created the group", actor);
	case CHAT_RENAME:
		return second && *second ?
			       g_strdup_printf("%s renamed the group to %s",
					       actor, second) :
			       g_strdup_printf("%s renamed the group", actor);
	case CHAT_PHOTO:
		return g_strdup_printf("%s changed the group photo", actor);
	case CHAT_LINK_ON:
		return g_strdup_printf("%s enabled the group invite link",
				       actor);
	case CHAT_LINK_OFF:
		return g_strdup_printf("%s disabled the group invite link",
				       actor);
	case CHAT_ANNOUNCE:
		return g_strdup_printf("%s posted an announcement", actor);
	case CHAT_ALBUM_RENAME:
		return args[0] && *args[0] && second && *second ?
			       g_strdup_printf("%s renamed the album %s to %s",
					       sender, args[0], second) :
			       g_strdup_printf("%s renamed an album", sender);
	case CHAT_ALBUM_REMOVE:
		return args[0] && *args[0] ?
			       g_strdup_printf("%s removed the album %s",
					       sender, args[0]) :
			       g_strdup_printf("%s removed an album", sender);
	case CHAT_ALBUM_PHOTOS_REMOVE:
		return g_strdup_printf("%s removed photos from an album",
				       sender);
	case CHAT_INVITE_LIMIT:
		return g_strdup("The group invitation limit was reached");
	case CHAT_INVITE_ENABLED:
		return g_strdup("Group invitations were enabled");
	case CHAT_TRANSFER_FAILED:
		return g_strdup("LINE could not transfer this message");
	default:
		return g_strdup(fallback ? fallback : "Chat activity");
	}
}

static const char *call_field(J *meta, const char *key, const char *legacy)
{
	const char *value = exact_text(meta, key);

	return value ? value : exact_text(meta, legacy);
}

static char *call_record(J *meta, gboolean own, gboolean *notice,
			 const char *fallback)
{
	const char *type = call_field(meta, "TYPE", "voipType");

	if (!g_strcmp0(type, "G")) {
		const char *event =
			call_field(meta, "GC_EVT_TYPE", "voipGcEventType");
		const char *media =
			call_field(meta, "GC_MEDIA_TYPE", "voipGcMediaType");
		const char *group =
			!g_strcmp0(media, "VIDEO") ? "Group video call" :
			!g_strcmp0(media, "LIVE")  ? "Group live call" :
						     "Group voice call";

		*notice = TRUE;
		if (!g_strcmp0(event, "S"))
			return g_strdup_printf("%s started", group);
		if (!g_strcmp0(event, "E"))
			return g_strdup_printf("%s ended", group);
		if (!g_strcmp0(event, "I"))
			return g_strdup_printf("%s invitation", group);
		return g_strdup(fallback ? fallback : group);
	}
	const char *kind = !g_strcmp0(type, "V") ? "Video call" :
			   !g_strcmp0(type, "A") ? "Voice call" :
			   !g_strcmp0(type, "P") ? "Phone call" :
						   "Call";
	const char *result = call_field(meta, "RESULT", "voipResult");

	if (!g_strcmp0(result, "NORMAL")) {
		const char *duration =
			call_field(meta, "DURATION", "voipDuration");
		guint64 ms;

		if (duration &&
		    g_ascii_string_to_unsigned(duration, 10, 1, G_MAXINT32, &ms,
					       NULL)) {
			unsigned int seconds = ms / 1000;

			if (seconds >= 3600)
				return g_strdup_printf("%s · %u:%02u:%02u",
						       kind, seconds / 3600,
						       seconds / 60 % 60,
						       seconds % 60);
			return g_strdup_printf("%s · %u:%02u", kind,
					       seconds / 60, seconds % 60);
		}
		return g_strdup(kind);
	}
	const char *status = !g_strcmp0(result, "NO_RESPONSE") ?
				     (own ? "No answer" : "Missed") :
			     !g_strcmp0(result, "CANCELED") ? "Canceled" :
			     !g_strcmp0(result, "REJECTED") ? "Declined" :
			     !g_strcmp0(result, "BUSY")	    ? "Busy" :
			     !g_strcmp0(result, "FAIL")	    ? "Failed" :
							      NULL;

	if (status)
		return g_strdup_printf("%s · %s", kind, status);
	return g_strdup(fallback ? fallback : kind);
}

static J *emoji_ranges(const char *text, J *resources, gboolean sending,
		       J *ownership)
{
	if (!length(resources) || length(resources) > 1024 || !text ||
	    !g_utf8_validate(text, -1, NULL))
		return NULL;
	g_autoptr(J) converted = array(), packs = obj();
	const char *cursor = text;
	guint64 position = 0;

	for (size_t i = 0; i < length(resources); i++) {
		J *item = at(resources, i);
		guint64 start, end, version = 1;
		const char *pack = exact_text(item, "productId");
		const char *id =
			exact_text(item, sending ? "emojiId" : "sticonId");
		const char *kind = field(item, "resourceType") ?
					   exact_text(item, "resourceType") :
					   "STATIC";
		if (!line_emoji_id_valid(pack, id) || !kind ||
		    (strcmp(kind, "STATIC") && strcmp(kind, "ANIMATION")) ||
		    (field(item, "version") &&
		     (!emoji_integer(item, "version", &version) ||
		      (sending && !version))) ||
		    !emoji_integer(item, sending ? "start" : "S", &start) ||
		    !emoji_integer(item, sending ? "length" : "E", &end))
			return NULL;
		if (sending)
			end += start;
		if (end <= start ||
		    !emoji_boundary(&cursor, &position, start) ||
		    !emoji_boundary(&cursor, &position, end))
			return NULL;
		J *range = obj();

		number(range, sending ? "S" : "start", start);
		number(range, sending ? "E" : "length",
		       sending ? end : end - start);
		put(range, "productId", string(pack));
		put(range, sending ? "sticonId" : "emojiId", string(id));
		number(range, "version", version);
		put(range, "resourceType", string(kind));
		json_object_array_add(converted, range);
		if (ownership && !field(packs, pack)) {
			boolean(packs, pack, TRUE);
			json_object_array_add(ownership, string(pack));
		}
	}
	return g_steal_pointer(&converted);
}

static J *metadata_object(J *metadata, const char *key, size_t limit, int depth)
{
	const char *encoded = exact_text(metadata, key);
	size_t size = encoded ? strlen(encoded) : 0;

	if (!size || size > limit)
		return NULL;
	struct json_tokener *reader = json_tokener_new_ex(depth);

	if (!reader)
		return NULL;
	json_tokener_set_flags(reader, JSON_TOKENER_STRICT |
					       JSON_TOKENER_VALIDATE_UTF8);
	J *value = json_tokener_parse_ex(reader, encoded, (int)size);

	if (json_tokener_get_error(reader) != json_tokener_success ||
	    json_tokener_get_parse_end(reader) != size ||
	    !json_object_is_type(value, json_type_object)) {
		json_object_put(value);
		value = NULL;
	}
	json_tokener_free(reader);
	return value;
}

static J *plain_replacement(J *metadata)
{
	return metadata_object(metadata, "REPLACE", 256 * 1024,
			       JSON_TOKENER_DEFAULT_DEPTH);
}

static const char *flex_alt_text(J *value, const char *key)
{
	const char *alt = exact_text(value, key);

	return alt && *alt && strlen(alt) <= FLEX_ALT_TEXT_LIMIT &&
			       g_utf8_validate(alt, -1, NULL) ?
		       alt :
		       NULL;
}

static char *add_flex(J *row, J *metadata)
{
	g_autoptr(J) payload = metadata_object(metadata, "FLEX_JSON",
					       FLEX_JSON_LIMIT, FLEX_DEPTH);
	J *contents = payload;
	const char *type = exact_text(contents, "type");
	const char *alt = flex_alt_text(metadata, "ALT_TEXT");

	if (!g_strcmp0(type, "flex")) {
		if (!alt)
			alt = flex_alt_text(payload, "altText");
		contents = field(payload, "contents");
		type = exact_text(contents, "type");
	}
	if (!g_strcmp0(type, "bubble") || (!g_strcmp0(type, "carousel") &&
					   length(field(contents, "contents"))))
		put(row, "flex", json_object_get(contents));
	return g_strdup(alt);
}

static gboolean sealed_text(J *message)
{
	return length(field(message, "20")) ||
	       (field(field(message, "18"), "e2eeVersion") &&
		!nonempty(message, "10"));
}

static J *text_payload(Line *s, J *message, GError **error)
{
	if (sealed_text(message))
		return line_open_payload(s, message, 0, error);
	J *payload = obj();
	const char *text = exact_text(message, "10");

	if (text)
		put(payload, "text", string(text));
	J *replacements = plain_replacement(field(message, "18"));

	if (replacements)
		put(payload, "REPLACE", replacements);
	return payload;
}

J *line_forward_text(Line *s, const char *chat, J *message, GError **error)
{
	g_autoptr(J) payload = text_payload(s, message, error);

	if (!payload)
		return NULL;
	const char *text = exact_text(payload, "text");

	if (!text || !*text) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This message has no text to forward.");
		return NULL;
	}
	J *resources =
		field(field(field(payload, "REPLACE"), "sticon"), "resources");
	g_autoptr(J) emojis = emoji_ranges(text, resources, FALSE, NULL);

	return line_send_emoji(s, chat, text, NULL, emojis, error);
}

static J *send_plain_message(Line *s, const char *chat, int type, J *meta,
			     J *location, GError **error)
{
	if (!talk_id(chat) || s->seq < 0 || s->seq >= G_MAXINT32) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Unable to forward this message.");
		return NULL;
	}
	g_autoptr(J) args = obj();
	J *msg = obj();

	number(args, "1:i32", ++s->seq);
	put(args, "2:obj", msg);
	put(msg, "2:str", string(chat));
	number(msg, "5:i64", 0);
	number(msg, "6:i64", 0);
	boolean(msg, "14:bool", FALSE);
	number(msg, "15:i32", type);
	if (location)
		put(msg, "11:obj", json_object_get(location));
	if (meta && json_object_object_length(meta))
		put(msg, "18:map:str:str", json_object_get(meta));
	number(msg, "19:byte", 0);
	g_autoptr(J) reply = line_rpc(s, "/S4", "sendMessage", args, error);
	if (!reply)
		return NULL;
	const char *id = nonempty(reply, "4");

	if (!id) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned no message ID. Check the chat before resending.");
		return NULL;
	}
	J *receipt = obj();

	put(receipt, "id", string(id));
	return receipt;
}

J *line_forward_sticker(Line *s, const char *chat, J *message, GError **error)
{
	J *src = field(message, "18");
	const char *stkid = exact_text(src, "STKID"),
		   *pkg = exact_text(src, "STKPKGID"),
		   *ver = exact_text(src, "STKVER");

	if (json_object_get_int(field(message, "15")) != 7 || !stkid || !pkg ||
	    !ver) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This sticker cannot be forwarded.");
		return NULL;
	}
	g_autoptr(J) meta = obj();
	static const char *const keys[] = { "STKID",  "STKPKGID", "STKVER",
					    "STKTXT", "STKOPT",	  "STKHASH" };

	for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
		const char *v = exact_text(src, keys[i]);

		if (v)
			put(meta, keys[i], string(v));
	}
	return send_plain_message(s, chat, 7, meta, NULL, error);
}

J *line_forward_location(Line *s, const char *chat, J *message, GError **error)
{
	J *src = field(message, "11");

	if (json_object_get_int(field(message, "15")) != 15 ||
	    !json_object_is_type(src, json_type_object)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This location cannot be forwarded.");
		return NULL;
	}
	g_autoptr(J) loc = obj();
	const char *title = nonempty(src, "1"), *addr = nonempty(src, "2"),
		   *phone = nonempty(src, "5");
	J *lat = field(src, "3"), *lon = field(src, "4");

	if (title)
		put(loc, "1:str", string(title));
	if (addr)
		put(loc, "2:str", string(addr));
	if (lat)
		put(loc, "3:double",
		    json_object_new_double(json_object_get_double(lat)));
	if (lon)
		put(loc, "4:double",
		    json_object_new_double(json_object_get_double(lon)));
	if (phone)
		put(loc, "5:str", string(phone));
	return send_plain_message(s, chat, 15, NULL, loc, error);
}

J *line_forward_contact(Line *s, const char *chat, J *message, GError **error)
{
	J *src = field(message, "18");
	const char *mid = exact_text(src, "mid"),
		   *name = exact_text(src, "displayName");

	if (json_object_get_int(field(message, "15")) != 13 || !mid ||
	    !talk_id(mid) || mid[0] != 'u') {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This contact card cannot be forwarded.");
		return NULL;
	}
	g_autoptr(J) meta = obj();

	put(meta, "mid", string(mid));
	if (name && *name)
		put(meta, "displayName", string(name));
	return send_plain_message(s, chat, 13, meta, NULL, error);
}

char *line_message_preview(Line *s, J *message, GError **error)
{
	g_autoptr(J) payload = text_payload(s, message, error);

	if (!payload)
		return NULL;
	const char *text = exact_text(payload, "text");

	if (!text || !*text) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "This message has no text to announce.");
		return NULL;
	}
	const char *end = text;

	while (*end && end - text < 400)
		end = g_utf8_next_char(end);
	return g_strndup(text, end - text);
}

static J *history_rows(Line *s, J *reply, J **messages, GError **error)
{
	g_autoptr(J) names = obj();
	if (!json_object_is_type(reply, json_type_array)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned an unreadable message list.");
		return NULL;
	}
	for (size_t i = 0; i < length(reply); i++) {
		J *message = at(reply, i);
		const char *mid = nonempty(message, "1");

		if (talk_id(mid) && mid[0] == 'u' &&
		    g_strcmp0(mid, s->mid) != 0)
			put(names, mid, string(""));
		J *meta = field(message, "18");

		if (json_object_get_int(field(message, "15")) == 18) {
			enum chat_action action = chat_action(meta);

			if (action >= CHAT_JOIN && action <= CHAT_ANNOUNCE) {
				g_auto(GStrv) args = chat_action_args(meta);

				add_notice_names(s, names, args[0]);
				if (args[0] && has_target_list(action))
					add_notice_names(s, names, args[1]);
			}
		}
	}
	line_resolve_names(s, names, 'u');
	J *rows = array();

	for (size_t i = 0; i < length(reply); i++) {
		J *msg = at(reply, i), *row = obj();
		const char *id = nonempty(msg, "4"),
			   *sender = nonempty(msg, "1");
		const char *name = sender ? nonempty(names, sender) : NULL;
		gboolean own = sender && g_strcmp0(sender, s->mid) == 0;
		gint64 type = json_object_get_int64(field(msg, "15"));
		const char *text = nonempty(msg, "10");
		gboolean notice = type == 18;

		g_autoptr(J) decrypted = NULL, unsealed_replace = NULL;
		if (type == 0) {
			J *replace = NULL;

			if (sealed_text(msg)) {
				g_autoptr(GError) key_error = NULL;
				decrypted = line_open_payload(s, msg, 0,
							      &key_error);
				text = exact_text(decrypted, "text");
				if (!text) {
					text = g_error_matches(
						       key_error, LINE_ERROR,
						       LINE_OWN_KEY_MISSING) ?
						       "[Letter Sealing key missing on this device]" :
						       "[Unable to decrypt this message]";
					boolean(row, "unreadable", TRUE);
				} else
					replace = field(decrypted, "REPLACE");
			} else {
				unsealed_replace =
					plain_replacement(field(msg, "18"));
				replace = unsealed_replace;
			}
			J *emojis = emoji_ranges(text,
						 field(field(replace, "sticon"),
						       "resources"),
						 FALSE, NULL);
			if (emojis)
				put(row, "emojis", emojis);
		}
		static const char *const kinds[] = {
			"text",	    "photo",	     "video",	   "audio",
			"html",	    "pdf",	     "call",	   "sticker",
			"presence", "gift",	     "board",	   "app",
			"link",	    "contact",	     "file",	   "location",
			"post",	    "rich",	     "chat_event", "music",
			"payment",  "externalImage", "flex"
		};
		J *metadata = field(msg, "18");

		g_autofree char *alt = type == 22 ? add_flex(row, metadata) :
						    NULL;

		if (type == 17) {
			J *rich = line_rich_parse(metadata, &alt);

			if (rich)
				put(row, "rich", rich);
		}
		g_autofree char *event = NULL;

		if (type == 18)
			event = chat_notice(s, names, metadata,
					    own	 ? "You" :
					    name ? name :
						   "A member",
					    text);
		else if (type == 6)
			event = call_record(metadata, own, &notice, text);

		if (event)
			text = event;
		else if (alt)
			text = alt;
		const char *filename = nonempty(metadata, "FILE_NAME"),
			   *related = nonempty(msg, "21");
		/* main.c names an unsafe one "attachment" when it lists it. */
		if (filename && !line_received_filename(filename))
			filename = NULL;
		if (type == 2) {
			guint64 duration;
			const char *ms = nonempty(metadata, "DURATION");

			if (ms &&
			    g_ascii_string_to_unsigned(ms, 10, 1, G_MAXINT32,
						       &duration, NULL))
				number(row, "durationMs", (gint64)duration);
			if (!filename)
				filename = "video.mp4";
		}
		if (filename)
			put(row, "filename", string(filename));
		if (related && json_object_get_int(field(msg, "22")) == 3)
			put(row, "replyTo", string(related));
		if (!notice && sender && talk_id(sender)) {
			put(row, "senderId", string(sender));
			const char *pic = strfield(s->pictures, sender);

			put(row, "avatarKey", string(pic ? pic : ""));
		}
		put(row, "type",
		    string(notice ? "chat_event" :
			   type >= 0 && type < (gint64)G_N_ELEMENTS(kinds) ?
				    kinds[type] :
				    "other"));
		J *reactions = field(msg, "27");

		if (!notice && length(reactions)) {
			static const char *const names_by_type[] = {
				NULL,  NULL,	  "nice", "love",
				"fun", "amazing", "sad",  "omg"
			};
			J *counts = obj();

			for (size_t j = 0; j < length(reactions); j++) {
				int k = json_object_get_int(field(
					field(at(reactions, j), "3"), "1"));
				if (k >= 2 &&
				    k < (int)G_N_ELEMENTS(names_by_type)) {
					const char *key = names_by_type[k];

					number(counts, key,
					       1 + json_object_get_int(
							   field(counts, key)));
				}
			}
			put(row, "reactions", counts);
		}
		if (type == 7) {
			const char *opt = exact_text(metadata, "STKOPT");

			if (opt && (!strcmp(opt, "S") || !strcmp(opt, "AS") ||
				    !strcmp(opt, "PS")))
				boolean(row, "hasSound", TRUE);
		}
		if (type == 15) {
			J *loc = field(msg, "11");

			if (json_object_is_type(loc, json_type_object)) {
				const char *title = nonempty(loc, "1"),
					   *addr = nonempty(loc, "2"),
					   *phone = nonempty(loc, "5");
				J *lat = field(loc, "3"),
				  *lon = field(loc, "4");
				J *card = obj();

				if (title)
					put(card, "title", string(title));
				if (addr)
					put(card, "address", string(addr));
				if (phone)
					put(card, "phone", string(phone));
				if (lat && lon &&
				    (json_object_is_type(lat,
							 json_type_double) ||
				     json_object_is_type(lat, json_type_int)) &&
				    (json_object_is_type(lon,
							 json_type_double) ||
				     json_object_is_type(lon, json_type_int))) {
					put(card, "lat",
					    json_object_new_double(
						    json_object_get_double(
							    lat)));
					put(card, "lon",
					    json_object_new_double(
						    json_object_get_double(
							    lon)));
				}
				put(row, "location", card);
				if (!text)
					text = title ? title : addr;
			}
		}
		if (type == 13) {
			const char *cmid = nonempty(metadata, "mid");
			const char *cname = nonempty(metadata, "displayName");

			if (cmid && talk_id(cmid) && cmid[0] == 'u')
				put(row, "contactMid", string(cmid));
			if (cname)
				put(row, "contactName", string(cname));
			if (!text && cname)
				text = cname;
		}
		put(row, "id", string(id ? id : ""));
		put(row, "from",
		    string(own	  ? "You" :
			   name	  ? name :
			   sender ? sender :
				    "LINE"));
		put(row, "text", string(text ? text : content_label(type)));
		boolean(row, "own", own && !notice);
		J *read_count = field(msg, "23");
		if (own && !notice && g_strcmp0(strfield(msg, "2"), s->mid) &&
		    json_object_is_type(read_count, json_type_int) &&
		    json_object_get_int(read_count) >= 0)
			put(row, "readCount", json_object_get(read_count));
		number(row, "time", json_object_get_int64(field(msg, "5")));
		json_object_array_add(rows, row);
	}
	json_object_array_sort(rows, message_order);
	if (messages) {
		J *index = obj();

		for (size_t i = 0; i < length(reply); i++) {
			J *msg = at(reply, i);
			const char *id = nonempty(msg, "4");

			if (id)
				put(index, id, json_object_get(msg));
		}
		*messages = index;
	}
	return rows;
}

static gboolean delivery_position(J *message, gint64 *time, guint64 *id)
{
	const char *digits = strfield(message, "4");
	*time = json_object_get_int64(field(message, "6"));
	return *time > 0 && digits &&
	       g_ascii_string_to_unsigned(digits, 10, 1, G_MAXINT64, id, NULL);
}

J *line_history_cursor(J *previous)
{
	gint64 boundary_time = G_MAXINT64;
	guint64 boundary_id = G_MAXUINT64;
	J *oldest = NULL;

	if (previous) {
		json_object_object_foreach(previous, key, message) {
			gint64 t;
			guint64 id;
			(void)key;
			if (delivery_position(message, &t, &id) &&
			    (t < boundary_time ||
			     (t == boundary_time && id < boundary_id))) {
				boundary_time = t;
				boundary_id = id;
				oldest = message;
			}
		}
	}
	if (!oldest)
		return NULL;
	J *cursor = obj();

	put(cursor, "4", json_object_get(field(oldest, "4")));
	put(cursor, "6", json_object_get(field(oldest, "6")));
	return cursor;
}

struct page_boundary {
	gint64 time;
	guint64 id, cleared_through;
	gboolean inclusive;
};

static J *previous_page(Line *s, const char *chat, struct page_boundary end,
			J **messages, gboolean *more, GError **error)
{
	g_autoptr(J) args = obj(), page = array();
	J *req = obj(), *cursor = obj();
	gboolean reached_floor = FALSE;

	put(args, "2:obj", req);
	number(args, "3:i32", 7);
	put(req, "1:str", string(chat));
	put(req, "2:obj", cursor);
	number(cursor, "1:i64", end.time);
	number(cursor, "2:i64", (gint64)end.id);
	number(req, "3:i32", HISTORY_LIMIT);
	g_autoptr(J) reply = line_rpc(
		s, "/S4", "getPreviousMessagesV2WithRequest", args, error);
	if (!reply)
		return NULL;
	if (!json_object_is_type(reply, json_type_array)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned an unreadable history page.");
		return NULL;
	}
	for (size_t i = 0; i < MIN(length(reply), HISTORY_LIMIT); i++) {
		J *message = at(reply, i);
		gint64 t;
		guint64 id;

		if (!delivery_position(message, &t, &id))
			continue;
		if (id <= end.cleared_through) {
			reached_floor = TRUE;
			continue;
		}
		if (t > end.time || (t == end.time && id > end.id) ||
		    (!end.inclusive && id == end.id))
			continue;
		json_object_array_add(page, json_object_get(message));
	}
	if (more)
		*more = !reached_floor && length(reply) >= HISTORY_LIMIT &&
			length(page) > 0;
	return history_rows(s, page, messages, error);
}

J *line_history_with_summary(Line *s, const char *chat, J *summary,
			     J **messages, GError **error)
{
	if (!talk_id(chat)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Select a LINE conversation.");
		return NULL;
	}
	g_autoptr(J) args = obj();

	put(args, "2:str", string(chat));
	number(args, "3:i32", HISTORY_LIMIT);
	g_autoptr(J) reply =
		line_rpc(s, "/S4", "getRecentMessagesV2", args, error);

	if (!reply)
		return NULL;
	/* An empty recent page can coexist with a message-box delivery head. */
	if (json_object_is_type(reply, json_type_array) && !length(reply) &&
	    !g_strcmp0(strfield(summary, "id"), chat)) {
		const char *digits = strfield(summary, "lastId");
		struct page_boundary end = {
			.time = json_object_get_int64(field(summary, "time")),
			.cleared_through = json_object_get_uint64(
				field(summary, "clearThrough")),
			.inclusive = TRUE,
		};

		if (end.time > 0 && digits &&
		    g_ascii_string_to_unsigned(digits, 10, 1, G_MAXINT64,
					       &end.id, NULL) &&
		    end.id > end.cleared_through)
			return previous_page(s, chat, end, messages, NULL,
					     error);
	}
	return history_rows(s, reply, messages, error);
}

J *line_history_raw(Line *s, const char *chat, J **messages, GError **error)
{
	return line_history_with_summary(s, chat, NULL, messages, error);
}

J *line_history_older_since(Line *s, const char *chat, J *boundary,
			    guint64 cleared_through, J **messages,
			    gboolean *more, GError **error)
{
	struct page_boundary end = { .cleared_through = cleared_through };

	if (!talk_id(chat) ||
	    !delivery_position(boundary, &end.time, &end.id)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE did not supply a cursor for earlier messages.");
		return NULL;
	}
	return previous_page(s, chat, end, messages, more, error);
}

J *line_history_older(Line *s, const char *chat, J *boundary, J **messages,
		      gboolean *more, GError **error)
{
	return line_history_older_since(s, chat, boundary, 0, messages, more,
					error);
}

static J *mention_metadata(const char *text, J *mentions)
{
	if (!length(mentions) || length(mentions) > 20)
		return NULL;
	g_autoptr(J) meta = obj();
	J *people = array();
	const char *cursor = text;
	guint64 position = 0;

	put(meta, "MENTIONEES", people);
	for (size_t i = 0; i < length(mentions); i++) {
		J *item = at(mentions, i);
		const char *id = exact_text(item, "id");
		guint64 start, width;

		if (!talk_id(id) || id[0] != 'u' ||
		    !emoji_integer(item, "start", &start) ||
		    !emoji_integer(item, "length", &width) || width < 2 ||
		    !emoji_boundary(&cursor, &position, start) ||
		    *cursor != '@' ||
		    !emoji_boundary(&cursor, &position, start + width))
			return NULL;
		J *person = obj();
		g_autofree char *from =
			g_strdup_printf("%" G_GUINT64_FORMAT, start);
		g_autofree char *to =
			g_strdup_printf("%" G_GUINT64_FORMAT, start + width);

		put(person, "S", string(from));
		put(person, "E", string(to));
		put(person, "M", string(id));
		json_object_array_add(people, person);
	}
	return g_steal_pointer(&meta);
}

J *line_send_decorated(Line *s, const char *chat, const char *text,
		       const char *reply_to, J *emojis, J *mentions,
		       GError **error)
{
	if (reply_to && (!*reply_to || strlen(reply_to) > 32 ||
			 reply_to[strspn(reply_to, "0123456789")])) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Select a message to reply to.");
		return NULL;
	}
	if (!talk_id(chat) || !text || !*text ||
	    !g_utf8_validate(text, -1, NULL)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Select a conversation and enter a text message.");
		return NULL;
	}
	g_autoptr(J) chunks = NULL, meta = obj();

	if (mentions) {
		g_autoptr(J) mention =
			chat[0] == 'c' ? mention_metadata(text, mentions) :
					 NULL;

		if (!mention) {
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"Choose a valid member to mention in this group.");
			return NULL;
		}
		put(meta, "MENTION",
		    string(json_object_to_json_string_ext(
			    mention, JSON_C_TO_STRING_PLAIN)));
	}
	if (!emojis ||
	    (json_object_is_type(emojis, json_type_array) && !length(emojis))) {
		chunks = line_seal_text(s, chat, text, error);
	} else {
		g_autoptr(J) ownership = array();
		J *resources = emoji_ranges(text, emojis, TRUE, ownership);

		if (!resources) {
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"The LINE emoji selection no longer matches this message.");
			return NULL;
		}
		g_autoptr(J) payload = obj();
		J *replace = obj(), *sticon = obj();

		put(payload, "text", string(text));
		put(payload, "REPLACE", replace);
		put(replace, "sticon", sticon);
		put(sticon, "resources", resources);
		chunks = line_seal_payload(s, chat, 0, payload, error);
		put(meta, "STICON_OWNERSHIP",
		    string(json_object_to_json_string_ext(
			    ownership, JSON_C_TO_STRING_PLAIN)));
	}
	if (!chunks)
		return NULL;
	put(meta, "e2eeVersion", string("2"));
	put(meta, "contentType", string("0"));
	put(meta, "e2eeMark", string("2"));
	return line_send_sealed(s, chat, 0, meta, chunks, reply_to, error);
}

J *line_send_emoji(Line *s, const char *chat, const char *text,
		   const char *reply_to, J *emojis, GError **error)
{
	return line_send_decorated(s, chat, text, reply_to, emojis, NULL,
				   error);
}

J *line_send_reply(Line *s, const char *chat, const char *text,
		   const char *reply_to, GError **error)
{
	return line_send_emoji(s, chat, text, reply_to, NULL, error);
}

J *line_send(Line *s, const char *chat, const char *text, GError **error)
{
	return line_send_reply(s, chat, text, NULL, error);
}

J *line_send_sealed(Line *s, const char *chat, int type, J *meta, J *chunks,
		    const char *reply_to, GError **error)
{
	if (length(chunks) != 5 || s->seq < 0 || s->seq >= G_MAXINT32) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Unable to prepare a Letter Sealing message.");
		return NULL;
	}
	g_autoptr(J) args = obj();
	J *msg = obj();

	number(args, "1:i32", ++s->seq);
	put(args, "2:obj", msg);
	put(msg, "2:str", string(chat));
	number(msg, "5:i64", 0);
	number(msg, "6:i64", 0);
	boolean(msg, "14:bool", FALSE);
	number(msg, "15:i32", type);
	put(msg, "18:map:str:str", json_object_get(meta));
	number(msg, "19:byte", 0);
	put(msg, "20:list:str", json_object_get(chunks));
	if (reply_to) {
		put(msg, "21:str", string(reply_to));
		number(msg, "22:i32", 3);
		number(msg, "24:i32", 1);
	}
	g_autoptr(J) reply = line_rpc(s, "/S4", "sendMessage", args, error);
	if (!reply)
		return NULL;
	const char *id = nonempty(reply, "4");

	if (!id) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned no message ID. Check the chat before resending.");
		return NULL;
	}
	J *receipt = obj();

	put(receipt, "id", string(id));
	J *sent_at = field(reply, "5");

	if (json_object_is_type(sent_at, json_type_int))
		put(receipt, "sentAt", json_object_get(sent_at));
	return receipt;
}

J *line_history(Line *s, const char *chat, GError **error)
{
	return line_history_raw(s, chat, NULL, error);
}

J *line_message_action(Line *s, const char *id, const char *reaction,
		       GError **error)
{
	guint64 numeric;

	if (!g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &numeric,
					NULL) ||
	    s->seq >= G_MAXINT32) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid LINE message or sequence.");
		return NULL;
	}
	g_autoptr(J) args = obj();
	if (reaction) {
		static const char *const names[] = { "none", "nice",	"love",
						     "fun",  "amazing", "sad",
						     "omg" };
		guint i = 0;

		while (i < G_N_ELEMENTS(names) && strcmp(reaction, names[i]))
			i++;
		if (i == G_N_ELEMENTS(names)) {
			g_set_error_literal(error, LINE_ERROR, 1,
					    "Unknown reaction.");
			return NULL;
		}
		J *req = obj(), *kind = obj();

		put(args, "1:obj", req);
		number(req, "1:i32", ++s->seq);
		number(req, "2:i64", numeric);
		put(req, "3:obj", kind);
		number(kind, "1:i32", i + 1);
	} else {
		number(args, "1:i32", ++s->seq);
		put(args, "2:str", string(id));
	}
	g_autoptr(J) ack = line_rpc(
		s, "/S4", reaction ? "react" : "unsendMessage", args, error);
	if (!ack)
		return NULL;
	J *out = obj();

	put(out, "id", string(id));
	return out;
}

static J *announcement_row(Line *s, J *entry)
{
	J *contents = field(entry, "3");
	const char *text = nonempty(contents, "2");
	J *id = field(entry, "1");

	if (!json_object_is_type(id, json_type_int) ||
	    json_object_get_int64(id) <= 0)
		return NULL;
	J *permission = field(entry, "6");
	gboolean allowed = json_object_is_type(permission, json_type_int) &&
			   (json_object_get_int(permission) == 0 ||
			    (json_object_get_int(permission) == 1 &&
			     !g_strcmp0(strfield(entry, "4"), s->mid)));
	J *row = obj();

	put(row, "id", string(json_object_get_string(id)));
	put(row, "text", string(text ? text : "Shared announcement"));
	boolean(row, "canRemove", allowed);
	return row;
}

J *line_announcements(Line *s, const char *chat, GError **error)
{
	g_autoptr(J) args = obj();

	put(args, "2:str", string(chat));
	g_autoptr(J) result =
		line_rpc(s, "/S4", "getChatRoomAnnouncements", args, error);

	if (!result)
		return NULL;
	if (!json_object_is_type(result, json_type_array)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE returned an unreadable announcements list.");
		return NULL;
	}
	J *rows = array();

	for (size_t i = 0; i < MIN(length(result), 50); i++) {
		J *row = announcement_row(s, at(result, i));

		if (row)
			json_object_array_add(rows, row);
	}
	return rows;
}

J *line_announce_message(Line *s, const char *chat, const char *id,
			 const char *text, gint32 seq, GError **error)
{
	guint64 numeric;

	if (!talk_id(chat) || !id ||
	    !g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &numeric,
					NULL)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Choose a message to announce.");
		return NULL;
	}
	g_autoptr(J) args = obj();
	J *contents = obj();
	g_autofree char *link = g_strdup_printf(
		"line://nv/chatMsg?chatId=%s&messageId=%s", chat, id);

	number(args, "1:i32", seq);
	put(args, "2:str", string(chat));
	number(args, "3:i32", 0);
	put(args, "4:obj", contents);
	number(contents, "1:i32", 5);
	put(contents, "2:str", string(text));
	put(contents, "3:str", string(link));
	g_autoptr(J) result =
		line_rpc(s, "/S4", "createChatRoomAnnouncement", args, error);
	J *row = result ? announcement_row(s, result) : NULL;

	if (result && !row)
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"Check the announcements list before retrying.");
	return row;
}

J *line_remove_announcement(Line *s, const char *chat, const char *id,
			    gint32 seq, GError **error)
{
	guint64 numeric;

	if (!id || !g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &numeric,
					       NULL)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Select an announcement to remove.");
		return NULL;
	}
	g_autoptr(J) args = obj();

	number(args, "1:i32", seq);
	put(args, "2:str", string(chat));
	number(args, "3:i64", (gint64)numeric);
	return line_rpc(s, "/S4", "removeChatRoomAnnouncement", args, error);
}
