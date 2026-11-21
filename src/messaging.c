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

			if (!talk_id(id) || (MAX(hidden, removed) > 0 &&
					     last <= MAX(hidden, removed)))
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

static const char *content_label(gint64 n)
{
	static const char *const labels[] = {
		"[Empty message]", "[Photo]",	    "[Video]",
		"[Audio]",	   "[HTML]",	    "[PDF]",
		"[Call]",	   "[Sticker]",	    "[Presence]",
		"[Gift]",	   "[Group post]",  "[App link]",
		"[Link]",	   "[Contact]",	    "[File]",
		"[Location]",	   "[Post]",	    "[Rich message]",
		"[Chat event]",	   "[Music]",	    "[Payment]",
		"[Photo]",	   "[Flex message]"
	};
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
		const char *mid = nonempty(at(reply, i), "1");

		if (talk_id(mid) && mid[0] == 'u' &&
		    g_strcmp0(mid, s->mid) != 0)
			put(names, mid, string(""));
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
			"text",	    "photo",	     "video", "audio",
			"html",	    "pdf",	     "call",  "sticker",
			"presence", "gift",	     "board", "app",
			"link",	    "contact",	     "file",  "location",
			"post",	    "rich",	     "event", "music",
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

		if (alt)
			text = alt;
		const char *filename = nonempty(metadata, "FILE_NAME"),
			   *related = nonempty(msg, "21");
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
		if (sender && talk_id(sender)) {
			put(row, "senderId", string(sender));
			const char *pic = strfield(s->pictures, sender);

			put(row, "avatarKey", string(pic ? pic : ""));
		}
		put(row, "type",
		    string(type >= 0 && type < (gint64)G_N_ELEMENTS(kinds) ?
				   kinds[type] :
				   "other"));
		J *reactions = field(msg, "27");

		if (length(reactions)) {
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
		if (type == 15 && !text)
			text = nonempty(field(msg, "11"), "2");
		if (type == 13 && !text)
			text = nonempty(metadata, "displayName");
		put(row, "id", string(id ? id : ""));
		put(row, "from",
		    string(own	  ? "You" :
			   name	  ? name :
			   sender ? sender :
				    "LINE"));
		put(row, "text", string(text ? text : content_label(type)));
		boolean(row, "own", own);
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

J *line_history_raw(Line *s, const char *chat, J **messages, GError **error)
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
	return reply ? history_rows(s, reply, messages, error) : NULL;
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

J *line_history_older(Line *s, const char *chat, J *boundary, J **messages,
		      gboolean *more, GError **error)
{
	gint64 boundary_time;
	guint64 boundary_id;

	if (!talk_id(chat) ||
	    !delivery_position(boundary, &boundary_time, &boundary_id)) {
		g_set_error_literal(
			error, LINE_ERROR, 1,
			"LINE did not supply a cursor for earlier messages.");
		return NULL;
	}
	g_autoptr(J) args = obj(), page = array();
	J *req = obj(), *cursor = obj();

	put(args, "2:obj", req);
	number(args, "3:i32", 7);
	put(req, "1:str", string(chat));
	put(req, "2:obj", cursor);
	number(cursor, "1:i64", boundary_time);
	number(cursor, "2:i64", (gint64)boundary_id);
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
	for (size_t i = 0; i < length(reply); i++) {
		J *message = at(reply, i);
		gint64 t;
		guint64 id;

		if (delivery_position(message, &t, &id) &&
		    (t < boundary_time ||
		     (t == boundary_time && id < boundary_id)))
			json_object_array_add(page, json_object_get(message));
	}
	*more = length(reply) >= HISTORY_LIMIT && length(page) > 0;
	return history_rows(s, page, messages, error);
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
	if (type)
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
