// SPDX-License-Identifier: GPL-3.0-or-later
#include "friends.h"
#include <string.h>

enum {
	CONTACT_BATCH = 100,
	MAX_PAGE = 10000,
	STATUS_LIMIT = 8192,
	/* TalkService FriendRequestDirection. */
	REQUEST_INCOMING = 1
};

static J *fail(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return NULL;
}

/*
 * A person searches for a friend or two at a time, not dozens a minute. These
 * limits stay well clear of that while never slowing ordinary use.
 */
const struct line_pace_rule line_lookup_pace = {
	.gap_us = (gint64)3 * G_USEC_PER_SEC,
	.burst = 10,
	.window_us = (gint64)10 * 60 * G_USEC_PER_SEC,
	.cooldown_us = (gint64)60 * 60 * G_USEC_PER_SEC,
};
const struct line_pace_rule line_add_pace = {
	.gap_us = 0,
	.burst = 10,
	.window_us = (gint64)10 * 60 * G_USEC_PER_SEC,
	.cooldown_us = (gint64)60 * 60 * G_USEC_PER_SEC,
};

gint64 line_pace_wait(const struct line_pace *p, const struct line_pace_rule *r,
		      gint64 now)
{
	gint64 until = p->blocked_until;
	guint burst = MIN(r->burst, (guint)LINE_PACE_SLOTS);
	guint used = 0;
	gint64 newest = 0, oldest = G_MAXINT64;

	for (guint i = 0; i < LINE_PACE_SLOTS; i++) {
		gint64 t = p->recent[i];

		if (!t || now - t >= r->window_us)
			continue;
		used++;
		newest = MAX(newest, t);
		oldest = MIN(oldest, t);
	}
	if (newest && r->gap_us)
		until = MAX(until, newest + r->gap_us);
	if (burst && used >= burst)
		until = MAX(until, oldest + r->window_us);
	if (until <= now)
		return 0;
	return (until - now + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC;
}

void line_pace_record(struct line_pace *p, const struct line_pace_rule *r,
		      gint64 now, gboolean limited)
{
	p->recent[p->next] = now;
	p->next = (p->next + 1) % LINE_PACE_SLOTS;
	if (limited)
		p->blocked_until = MAX(p->blocked_until, now + r->cooldown_us);
}

static J *child(J *parent, const char *key)
{
	J *node = obj();

	json_object_object_add(parent, key, node);
	return node;
}

static void put_string(J *o, const char *key, const char *value)
{
	json_object_object_add(o, key, json_object_new_string(value));
}

static void put_bool(J *o, const char *key, gboolean value)
{
	json_object_object_add(o, key, json_object_new_boolean(value));
}

static const char *clean_string(J *v, gsize limit)
{
	if (!json_object_is_type(v, json_type_string))
		return NULL;
	const char *text = json_object_get_string(v);
	gsize len = json_object_get_string_len(v);

	return len && len <= limit && strlen(text) == len &&
			       g_utf8_validate(text, len, NULL) ?
		       text :
		       NULL;
}

static gboolean valid_mid(const char *id)
{
	if (!id || *id != 'u' || strlen(id) != 33)
		return FALSE;
	for (int i = 1; i < 33; i++)
		if (!g_ascii_isxdigit(id[i]))
			return FALSE;
	return TRUE;
}

static gboolean valid_ticket(const char *ticket)
{
	if (!ticket || !*ticket || strlen(ticket) > 256)
		return FALSE;
	for (const char *p = ticket; *p; p++)
		if (!g_ascii_isalnum(*p) && !strchr("._-", *p))
			return FALSE;
	return TRUE;
}

static gboolean valid_search_id(const char *id)
{
	if (!id || !*id || strlen(id) > 128)
		return FALSE;
	for (const char *p = id; *p; p++)
		if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) &&
		    !strchr("._-", *p))
			return FALSE;
	return TRUE;
}

/*
 * QR links are data, never navigation. Restrict the host/path and pass the
 * exact ticket or the ID variant (~id) to its corresponding lookup union.
 */
enum friend_source {
	VIA_PHONE = 2,
	VIA_ID = 3,
	VIA_TICKET = 4
};

static char *search_value(const char *kind, const char *value,
			  enum friend_source *source, GError **error)
{
	*source = VIA_ID;
	if (!kind || !value || !*value || strlen(value) > 2048)
		goto invalid;
	if (!strcmp(kind, "id")) {
		if (valid_search_id(value))
			return g_strdup(value);
	} else if (!strcmp(kind, "phone")) {
		if (*value != '+' || value[1] < '1' || value[1] > '9' ||
		    strlen(value) > 16 || !value[2] ||
		    strspn(value + 1, "0123456789") != strlen(value + 1))
			goto invalid;
		*source = VIA_PHONE;
		return g_strdup(value);
	} else if (!strcmp(kind, "link")) {
		const char *const prefixes[] = { "https://line.me/ti/p/",
						 "https://line.me/R/ti/p/",
						 "line://ti/p/" };
		for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++) {
			if (!g_str_has_prefix(value, prefixes[i]))
				continue;
			const char *begin = value + strlen(prefixes[i]);

			g_autofree char *component =
				g_strndup(begin, strcspn(begin, "?#"));
			if (component[0] == '~' &&
			    valid_search_id(component + 1))
				return g_strdup(component + 1);
			if (!valid_ticket(component))
				goto invalid;
			*source = VIA_TICKET;
			return g_steal_pointer(&component);
		}
		if (valid_ticket(value)) {
			*source = VIA_TICKET;
			return g_strdup(value);
		}
	}
invalid:
	fail(error,
	     "Enter one LINE ID, an international number beginning with +, or a personal LINE QR invitation link.");
	return NULL;
}

/*
 * A field-1 RelationService RejectedException has its own error enum. In
 * particular 1 is an unavailable target, not an invalid access token.
 */
static J *relation(Line *s, const char *method, J *args, GError **error)
{
	g_autoptr(GError) original = NULL;
	J *reply = line_rpc(s, "/RE4", method, args, &original);

	if (reply)
		return reply;
	const int reason = s->service_code;

	s->service_code = -1;
	static const char *const reasons[] = {
		NULL,
		"LINE could not find an available account for this request.",
		"LINE requires age verification for this account to use friend search.",
		"LINE's friend limit has been reached.",
		"LINE temporarily limited friend requests. Try again later.",
		"LINE declined the friend request data.",
		"LINE requires this person's QR invitation for this request."
	};
	if (g_error_matches(original, LINE_ERROR, 1) && reason > 0 &&
	    (guint)reason < G_N_ELEMENTS(reasons)) {
		/* Reason 4 is LINE's own rate limit: the worker pauses on it. */
		g_set_error_literal(error, LINE_ERROR,
				    reason == 4 ? LINE_RATE_LIMITED : 1,
				    reasons[reason]);
		return NULL;
	}
	if (original)
		g_propagate_error(error, g_steal_pointer(&original));
	else
		fail(error, "LINE could not complete the friend request.");
	return NULL;
}

static J *make_row(Line *s, const char *id, const char *profile_name,
		   const char *alias, const char *avatar, gboolean friend,
		   gboolean blocked, gboolean hidden, gboolean favorite)
{
	J *row = obj();
	gboolean self = !g_strcmp0(id, s->mid);

	put_string(row, "id", id);
	const char *original = profile_name && *profile_name ? profile_name :
							       id;
	put_string(row, "name", alias && *alias ? alias : original);
	put_string(row, "profileName", original);
	put_string(row, "overrideName", alias ? alias : "");
	put_bool(row, "friend", friend);
	put_bool(row, "blocked", blocked);
	put_bool(row, "hidden", hidden);
	put_bool(row, "favorite", favorite);
	put_bool(row, "self", self);
	put_bool(row, "canAdd", !self && !friend && !blocked);
	if (avatar && (!*avatar || avatar[0] != '/' || avatar[1] == '/' ||
		       strpbrk(avatar, "\r\n")))
		avatar = NULL;
	put_string(row, "avatarKey", avatar ? avatar : "");
	if (avatar) {
		if (!s->pictures)
			s->pictures = obj();
		put_string(s->pictures, id, avatar);
	} else if (s->pictures)
		json_object_object_del(s->pictures, id);
	return row;
}

/*
 * A V3 friend/block detail is a union (1 user, 2 bot, 3 absent). Older
 * servers can omit a whole detail. Listing already establishes friendship,
 * while an explicit notFriend wins if the relationship changed mid-page.
 */
static J *v3_row(Line *s, J *raw, gboolean listed, gboolean listed_blocked)
{
	const char *id = clean_string(field(raw, "1"), 33);

	if (!valid_mid(id))
		return NULL;
	J *detail = field(raw, "4"), *blocked = field(raw, "5"),
	  *profile = field(raw, "3");
	J *f = field(detail, "1");

	if (!f)
		f = field(detail, "2");
	J *row = make_row(s, id, clean_string(field(profile, "2"), 1024),
			  clean_string(field(f, "3"), 1024),
			  clean_string(field(profile, "3"), 1024),
			  f || (!detail && listed),
			  field(blocked, "1") || field(blocked, "2") ||
				  (!blocked && listed_blocked),
			  json_object_get_boolean(field(f, "6")),
			  json_object_get_int64(field(f, "4")) > 0);
	gboolean official = json_object_get_int(field(raw, "2")) == 2 ||
			    field(detail, "2") || field(blocked, "2");
	const char *status =
		clean_string(field(field(profile, "4"), "1"), STATUS_LIMIT);

	put_string(row, "status", status ? status : "");
	put_bool(row, "official", official);
	if (official)
		put_bool(row, "canAdd", FALSE);
	return row;
}

static J *v2_row(Line *s, const char *id, J *raw, gboolean listed_blocked)
{
	const char *inner = clean_string(field(raw, "1"), 33);

	if (!json_object_is_type(raw, json_type_object) ||
	    (field(raw, "1") && g_strcmp0(inner, id)))
		return NULL;
	int status = json_object_get_int(field(raw, "11"));
	J *row = make_row(s, id, clean_string(field(raw, "22"), 1024),
			  clean_string(field(raw, "27"), 1024),
			  clean_string(field(raw, "37"), 1024),
			  !field(raw, "11") || status == 1 || status == 2,
			  status == 2 || status == 4 || status == 6 ||
				  (!field(raw, "11") && listed_blocked),
			  !!(json_object_get_int64(field(raw, "36")) & 4),
			  json_object_get_int64(field(raw, "28")) > 0);
	/* Contact.statusMessage is field 26 in the legacy talk struct. */
	const char *message = clean_string(field(raw, "26"), STATUS_LIMIT);

	put_string(row, "status", message ? message : "");
	return row;
}

enum friend_roster {
	ROSTER_VISIBLE,
	ROSTER_HIDDEN,
	ROSTER_BLOCKED,
	ROSTER_RECOMMENDED
};

static gboolean flag(J *row, const char *key)
{
	return json_object_get_boolean(field(row, key));
}

static void append_friend(J *rows, J *row, enum friend_roster view)
{
	gboolean blocked = flag(row, "blocked"), hidden = flag(row, "hidden");

	if (row && flag(row, "friend") && !flag(row, "self") &&
	    !flag(row, "official") &&
	    (view == ROSTER_BLOCKED ?
		     blocked :
		     !blocked && hidden == (view == ROSTER_HIDDEN))) {
		json_object_array_add(rows, row);
	} else
		json_object_put(row);
}

/*
 * Only accept profiles requested in this service-issued page, including if
 * the server omits a deleted contact or returns responses out of order.
 */
static gboolean details(Line *s, J *mids, J *rows, enum friend_roster view,
			gboolean *v2, GError **error)
{
	g_autoptr(J) wanted = obj(), args = obj();
	J *request = child(args, "1:obj"), *targets = json_object_new_array();

	json_object_object_add(request, "1:list:obj", targets);
	json_object_object_add(request, "2:i32",
			       json_object_new_int(5)); /* AUTO_REPAIR */
	for (guint i = 0; i < json_object_array_length(mids); i++) {
		const char *id = json_object_get_string(
			json_object_array_get_idx(mids, i));
		put_bool(wanted, id, TRUE);
		J *one = obj();

		put_string(one, "1:str", id);
		json_object_array_add(targets, one);
	}
	if (!*v2) {
		g_autoptr(GError) problem = NULL;
		g_autoptr(J) reply =
			relation(s, "getContactsV3", args, &problem);
		if (reply) {
			J *contacts = field(reply, "1");

			if (!json_object_is_type(contacts, json_type_array)) {
				fail(error,
				     "LINE returned an unreadable contact list.");
				return FALSE;
			}
			for (guint i = 0;
			     i < json_object_array_length(contacts); i++) {
				J *item =
					json_object_array_get_idx(contacts, i);
				const char *id =
					clean_string(field(item, "1"), 33);
				if (!id || !field(wanted, id))
					continue;
				json_object_object_del(wanted, id);
				if (view == ROSTER_BLOCKED &&
				    json_object_get_boolean(field(
					    field(field(item, "5"), "1"), "3")))
					continue;
				if (view == ROSTER_RECOMMENDED) {
					J *recommend =
						field(field(item, "6"), "1");

					if (json_object_get_int(
						    field(item, "2")) != 1 ||
					    !field(field(item, "4"), "3") ||
					    !field(field(item, "5"), "3") ||
					    !json_object_is_type(
						    recommend,
						    json_type_object) ||
					    json_object_get_boolean(
						    field(recommend, "4")))
						continue;
					J *row = v3_row(s, item, FALSE, FALSE);

					if (!flag(row, "canAdd")) {
						json_object_put(row);
						continue;
					}
					put_bool(row, "recommended", TRUE);
					json_object_array_add(rows, row);
				} else
					append_friend(
						rows,
						v3_row(s, item, TRUE,
						       view == ROSTER_BLOCKED),
						view);
			}
			return TRUE;
		}
		if (view == ROSTER_RECOMMENDED ||
		    !g_error_matches(problem, LINE_ERROR,
				     LINE_METHOD_UNAVAILABLE)) {
			g_propagate_error(error, g_steal_pointer(&problem));
			return FALSE;
		}
		*v2 = TRUE;
	}
	g_autoptr(J) legacy = obj();
	json_object_object_add(child(legacy, "1:obj"), "1:list:str",
			       json_object_get(mids));
	g_autoptr(J) reply = line_rpc(s, "/S4", "getContactsV2", legacy, error);
	if (!reply)
		return FALSE;
	J *entries = field(reply, "1");

	if (!json_object_is_type(entries, json_type_object)) {
		fail(error, "LINE returned an unreadable contact list.");
		return FALSE;
	}
	json_object_object_foreach(entries, id, entry) {
		J *raw = field(entry, "3");

		if (field(wanted, id))
			append_friend(rows,
				      v2_row(s, id, raw,
					     view == ROSTER_BLOCKED),
				      view);
	}
	return TRUE;
}

static J *friends_page(Line *s, enum friend_roster view, const char *cursor,
		       GError **error)
{
	if (cursor && (!*cursor || strlen(cursor) > 4096))
		return fail(error, "Invalid friend list cursor.");
	g_autoptr(J) args = obj();
	J *request = child(args, "1:obj");

	json_object_object_add(
		request, "2:i32",
		json_object_new_int(view == ROSTER_BLOCKED ?
					    1 :
					    2)); /* BLOCK / NOT_BLOCK */
	if (cursor)
		put_string(request, "1:str", cursor);
	const char *method = view == ROSTER_RECOMMENDED ?
				     "getUserRecommendIds" :
				     "getUserFriendIds";
	g_autoptr(J) page = relation(s, method, args, error);
	if (!page)
		return NULL;
	J *ids = field(page, "1"), *next = field(page, "2");

	if (!json_object_is_type(page, json_type_object) ||
	    (ids && !json_object_is_type(ids, json_type_array)))
		return fail(error, "LINE returned an unreadable friend list.");
	const size_t count = ids ? json_object_array_length(ids) : 0;

	if (count > MAX_PAGE)
		return fail(error, "LINE returned an unreadable friend list.");
	g_autoptr(J) result = obj(), seen = obj();
	J *rows = json_object_new_array();

	json_object_object_add(result, "friends", rows);
	if (next && (!json_object_is_type(next, json_type_string) ||
		     json_object_get_string_len(next))) {
		const char *token = clean_string(next, 4096);

		if (!token || !g_strcmp0(cursor, token))
			return fail(
				error,
				"LINE returned an invalid friend list cursor.");
		put_string(result, "next", token);
	}
	gboolean v2 = FALSE;

	for (guint i = 0; i < count;) {
		g_autoptr(J) batch = json_object_new_array();
		while (i < count &&
		       json_object_array_length(batch) < CONTACT_BATCH) {
			J *item = json_object_array_get_idx(ids, i++);
			const char *id = clean_string(item, 33);

			if (!valid_mid(id) || field(seen, id) ||
			    !g_strcmp0(id, s->mid))
				continue;
			put_bool(seen, id, TRUE);
			json_object_array_add(batch, json_object_get(item));
		}
		if (json_object_array_length(batch) &&
		    !details(s, batch, rows, view, &v2, error))
			return NULL;
	}
	return g_steal_pointer(&result);
}

J *line_friends_page(Line *s, const char *cursor, GError **error)
{
	return friends_page(s, ROSTER_VISIBLE, cursor, error);
}

J *line_managed_friends_page(Line *s, const char *view, const char *cursor,
			     GError **error)
{
	if (!g_strcmp0(view, "hidden"))
		return friends_page(s, ROSTER_HIDDEN, cursor, error);
	if (!g_strcmp0(view, "blocked"))
		return friends_page(s, ROSTER_BLOCKED, cursor, error);
	if (!g_strcmp0(view, "recommendations"))
		return friends_page(s, ROSTER_RECOMMENDED, cursor, error);
	return fail(error, "Choose hidden or blocked friends.");
}

J *line_friend_lookup(Line *s, const char *kind, const char *value,
		      GError **error)
{
	enum friend_source source;
	g_autofree char *lookup = search_value(kind, value, &source, error);

	if (!lookup)
		return NULL;
	g_autoptr(J) args = obj();
	J *request = child(args, "1:obj");

	if (source == VIA_PHONE)
		put_string(request, "1:str", lookup);
	else
		put_string(child(request, "1:obj"),
			   source == VIA_TICKET ? "2:str" : "1:str", lookup);
	const char *method = source == VIA_PHONE ?
				     "findContactByPhoneV3" :
				     "findContactBySearchIdOrTicketV3";
	g_autoptr(J) reply = relation(s, method, args, error);
	if (!reply)
		return NULL;
	J *row = v3_row(s, reply, FALSE, FALSE);

	if (!row)
		return fail(
			error,
			"LINE could not find an available account for this request.");
	return row;
}

static J *added(Line *s, J *args, J *row, GError **error)
{
	g_autoptr(J) ack = relation(s, "addFriendByMid", args, error);

	if (!ack)
		return NULL;
	if (!json_object_is_type(ack, json_type_object))
		return fail(error,
			    "LINE returned an unreadable friend receipt.");
	return make_row(s, strfield(row, "id"), strfield(row, "profileName"),
			strfield(row, "overrideName"),
			strfield(row, "avatarKey"), TRUE, FALSE, FALSE, FALSE);
}

J *line_add_recommendation(Line *s, const char *id, gint32 seq, GError **error)
{
	if (!valid_mid(id) || !g_strcmp0(id, s->mid) || seq <= 0)
		return fail(
			error,
			"Choose a person from your friend recommendations.");
	g_autoptr(J) mids = json_object_new_array(),
		     rows = json_object_new_array();
	gboolean legacy = FALSE;

	json_object_array_add(mids, json_object_new_string(id));
	if (!details(s, mids, rows, ROSTER_RECOMMENDED, &legacy, error))
		return NULL;
	if (!json_object_array_length(rows))
		return fail(
			error,
			"This friend recommendation is no longer available.");
	g_autoptr(J) args = obj();
	J *req = child(args, "1:obj"), *tracking = child(req, "3:obj");

	json_object_object_add(req, "1:i32", json_object_new_int(seq));
	put_string(req, "2:str", id);
	put_string(tracking, "1:str",
		   "{\"screen\":\"friendAdd:recommend\",\"spec\":\"native\"}");
	child(child(tracking, "2:obj"), "10:obj");
	return added(s, args, json_object_array_get_idx(rows, 0), error);
}

J *line_friend_add(Line *s, J *result, const char *kind, const char *value,
		   gint32 seq, GError **error)
{
	const char *id = clean_string(field(result, "id"), 33);

	if (seq <= 0 || !valid_mid(id) || !g_strcmp0(id, s->mid) ||
	    !json_object_get_boolean(field(result, "canAdd")) ||
	    json_object_get_boolean(field(result, "friend")) ||
	    json_object_get_boolean(field(result, "blocked")))
		return fail(
			error,
			"Select an available person from your friend search.");
	enum friend_source source;
	g_autofree char *lookup = search_value(kind, value, &source, error);

	if (!lookup)
		return NULL;
	g_autoptr(J) args = obj();
	J *request = child(args, "1:obj"), *tracking = child(request, "3:obj"),
	  *meta = child(tracking, "2:obj");
	json_object_object_add(request, "1:i32", json_object_new_int(seq));
	put_string(request, "2:str", id);
	if (source == VIA_ID)
		put_string(
			tracking, "1:str",
			"{\"screen\":\"friendAdd:idSearch\",\"spec\":\"native\"}");
	if (source == VIA_PHONE)
		put_string(
			tracking, "1:str",
			"{\"screen\":\"friendAdd:phoneSearch\",\"spec\":\"native\"}");
	const char *arm = source == VIA_TICKET ? "4:obj" :
			  source == VIA_ID     ? "3:obj" :
						 "2:obj";
	put_string(child(meta, arm), "1:str", lookup);
	return added(s, args, result, error);
}

J *line_friend_invite(Line *s, GError **error)
{
	g_autoptr(J) reply = line_rpc(s, "/S4", "getUserTicket", NULL, error);
	if (!reply)
		return NULL;
	const char *ticket = clean_string(field(reply, "1"), 256);
	gint64 expires = json_object_get_int64(field(reply, "10"));

	if (!valid_ticket(ticket) ||
	    (expires > 0 && expires <= g_get_real_time() / 1000))
		return fail(
			error,
			"Open My QR code on your phone to get a current personal link.");
	g_autofree char *url =
		g_strconcat("https://line.me/ti/p/", ticket, NULL);
	J *result = obj();

	put_string(result, "url", url);
	return result;
}

enum friend_setting {
	SET_FAVORITE,
	SET_ALIAS,
	SET_HIDDEN,
	SET_BLOCK
};

static J *update_friend(Line *s, J *row, enum friend_setting setting,
			const char *name, gboolean value, gint32 seq,
			GError **error)
{
	const char *id = clean_string(field(row, "id"), 33);
	gboolean hidden = flag(row, "hidden"), blocked = flag(row, "blocked");
	gboolean allowed = setting == SET_BLOCK ?
				   (blocked != value && (blocked || !hidden)) :
			   setting == SET_HIDDEN ?
				   (!blocked && hidden != value) :
				   (!blocked && !hidden);
	if (seq <= 0 || !valid_mid(id) || !g_strcmp0(id, s->mid) ||
	    !flag(row, "friend") || flag(row, "official") || !allowed)
		return fail(
			error,
			"Select a friend whose current settings permit this change.");
	g_autoptr(J) args = obj();
	J *request = child(args, "1:obj");

	json_object_object_add(request, "1:i32", json_object_new_int(seq));
	put_string(request, "2:str", id);
	const char *method;

	switch (setting) {
	case SET_ALIAS:
		method = "updateUserFriendOverridenName";
		put_string(request, "3:str", name);
		break;
	case SET_FAVORITE:
		method = "updateFriendFavorite";
		put_bool(request, "3:bool", value);
		break;
	case SET_HIDDEN:
		method = "updateFriendHidden";
		put_bool(request, "3:bool", value);
		break;
	case SET_BLOCK:
		method = value ? "blockRelation" : "unblockRelation";
		break;
	default:
		return fail(error, "Unknown friend setting.");
	}
	g_autoptr(J) ack = relation(s, method, args, error);
	if (!ack)
		return NULL;
	if (!json_object_is_type(ack, json_type_object))
		return fail(error,
			    "LINE returned an unreadable friend update.");
	J *updated = obj();

	json_object_object_foreach(row, key, val)
		json_object_object_add(updated, key, json_object_get(val));
	switch (setting) {
	case SET_ALIAS: {
		put_string(updated, "overrideName", name);
		const char *profile =
			clean_string(field(row, "profileName"), 1024);
		put_string(updated, "name",
			   *name   ? name :
			   profile ? profile :
				     id);
		if (s->profiles)
			g_hash_table_remove(s->profiles, id);
		break;
	}
	case SET_FAVORITE:
		put_bool(updated, "favorite", value);
		break;
	case SET_HIDDEN:
		put_bool(updated, "hidden", value);
		break;
	case SET_BLOCK:
		put_bool(updated, "blocked", value);
		put_bool(updated, "hidden", FALSE);
		break;
	}
	return updated;
}

J *line_friend_visibility(Line *s, J *row, gboolean hidden, gint32 seq,
			  GError **error)
{
	return update_friend(s, row, SET_HIDDEN, NULL, hidden, seq, error);
}

J *line_friend_block(Line *s, J *row, gboolean blocked, gint32 seq,
		     GError **error)
{
	return update_friend(s, row, SET_BLOCK, NULL, blocked, seq, error);
}

J *line_friend_favorite(Line *s, J *row, gboolean favorite, gint32 seq,
			GError **error)
{
	return update_friend(s, row, SET_FAVORITE, NULL, favorite, seq, error);
}

J *line_friend_rename(Line *s, J *row, const char *name, gint32 seq,
		      GError **error)
{
	if (!name || strlen(name) > 1024 || !g_utf8_validate(name, -1, NULL))
		return fail(error, "Enter a valid friend display name.");
	for (const char *p = name; *p; p = g_utf8_next_char(p))
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			return fail(error,
				    "Enter a single-line friend display name.");
	return update_friend(s, row, SET_ALIAS, name, FALSE, seq, error);
}

J *line_own_profile(Line *s, GError **error)
{
	g_autoptr(J) args = obj();
	json_object_object_add(args, "1:i32",
			       json_object_new_int(8)); /* USER_INITIATED */
	g_autoptr(J) raw = line_rpc(s, "/S4", "getProfile", args, error);
	if (!raw)
		return NULL;
	const char *id = clean_string(field(raw, "1"), 33);
	const char *name = clean_string(field(raw, "20"), 1024);
	const char *status = strfield(raw, "24");
	J *status_obj = field(raw, "24");

	if (!valid_mid(id) || g_strcmp0(s->mid, id) || !name ||
	    (status_obj &&
	     (!status || (json_object_get_string_len(status_obj) &&
			  !clean_string(status_obj, 8192)))))
		return fail(error,
			    "LINE returned an incomplete account profile.");
	J *row = obj();

	put_string(row, "id", id);
	put_string(row, "name", name);
	put_string(row, "status", status ? status : "");
	const char *line_id = clean_string(field(raw, "3"), 128);
	const char *avatar = clean_string(field(raw, "33"), 1024);

	if (line_id)
		put_string(row, "lineId", line_id);
	if (avatar &&
	    (avatar[0] != '/' || avatar[1] == '/' || strpbrk(avatar, "\r\n")))
		avatar = NULL;
	put_string(row, "avatarKey", avatar ? avatar : "");
	if (avatar) {
		if (!s->pictures)
			s->pictures = obj();
		put_string(s->pictures, id, avatar);
	} else if (s->pictures)
		json_object_object_del(s->pictures, id);
	return row;
}

static gboolean profile_value(J *v, gboolean status)
{
	if (!json_object_is_type(v, json_type_string))
		return FALSE;
	const char *s = json_object_get_string(v);
	int bytes = json_object_get_string_len(v);

	if (bytes > 4096 || strlen(s) != (gsize)bytes ||
	    !g_utf8_validate(s, bytes, NULL))
		return FALSE;
	guint length = 0;
	gboolean visible = FALSE;

	for (; *s; s = g_utf8_next_char(s)) {
		gunichar c = g_utf8_get_char(s);

		if (g_unichar_iscntrl(c) && !(status && c == '\n'))
			return FALSE;
		length += 1 + (status && c > 0xffff);
		visible |= !g_unichar_isspace(c);
	}
	return status ? length <= 500 : (visible && length <= 20);
}

J *line_own_profile_update(Line *s, J *changes, gint32 seq, GError **error)
{
	if (seq <= 0 || !json_object_is_type(changes, json_type_object) ||
	    !json_object_object_length(changes))
		return fail(error, "Choose a profile field to update.");
	json_object_object_foreach(changes, key, value) {
		gboolean status = !strcmp(key, "status");

		if ((!status && strcmp(key, "name")) ||
		    !profile_value(value, status))
			return fail(
				error,
				"Enter a display name of up to 20 characters or a status of up to 500 characters.");
	}
	g_autoptr(J) row = line_own_profile(s, error);
	if (!row)
		return NULL;
	g_autoptr(J) args = obj();
	json_object_object_add(args, "1:i32", json_object_new_int(seq));
	J *attributes = child(child(args, "2:obj"), "1:map:i32:obj");

	json_object_object_foreach(changes, name, edit) {
		if (!g_strcmp0(strfield(row, name),
			       json_object_get_string(edit)))
			continue;
		J *content =
			child(attributes, !strcmp(name, "name") ? "2" : "16");

		json_object_object_add(content, "1:str", json_object_get(edit));
		child(content, "2:map:str:str");
		json_object_object_add(row, name, json_object_get(edit));
	}
	if (json_object_object_length(attributes)) {
		g_autoptr(J) ack = line_rpc(s, "/S4", "updateProfileAttributes",
					    args, error);
		if (!ack)
			return NULL;
		if (!json_object_is_type(ack, json_type_object))
			return fail(
				error,
				"LINE returned an unreadable profile update.");
	}
	return g_steal_pointer(&row);
}

/*
 * One person's profile for a popout: getContactsV3 with USER_INITIATED.
 * The V3 TargetProfileDetail carries statusMessage as a struct whose
 * field 1 is the text. Legacy servers answer through getContactsV2.
 */
J *line_friend_card(Line *s, const char *id, GError **error)
{
	if (!valid_mid(id))
		return fail(error, "Choose a LINE account.");
	g_autoptr(J) args = obj();
	J *request = child(args, "1:obj"), *targets = json_object_new_array();
	J *one = obj();

	json_object_object_add(request, "1:list:obj", targets);
	json_object_object_add(request, "2:i32",
			       json_object_new_int(8)); /* USER_INITIATED */
	put_string(one, "1:str", id);
	json_object_array_add(targets, one);
	g_autoptr(GError) problem = NULL;
	g_autoptr(J) reply = relation(s, "getContactsV3", args, &problem);
	J *row = NULL;

	if (reply) {
		J *contacts = field(reply, "1");

		for (guint i = 0;
		     json_object_is_type(contacts, json_type_array) &&
		     i < json_object_array_length(contacts) && !row;
		     i++) {
			J *item = json_object_array_get_idx(contacts, i);

			if (g_strcmp0(clean_string(field(item, "1"), 33), id))
				continue;
			row = v3_row(s, item, FALSE, FALSE);
			J *recommend = field(field(item, "6"), "1");

			if (row &&
			    json_object_is_type(recommend, json_type_object))
				put_bool(row, "recommended", TRUE);
		}
	} else if (g_error_matches(problem, LINE_ERROR,
				   LINE_METHOD_UNAVAILABLE)) {
		g_autoptr(J) legacy = obj(), mids = json_object_new_array();

		json_object_array_add(mids, json_object_new_string(id));
		json_object_object_add(child(legacy, "1:obj"), "1:list:str",
				       g_steal_pointer(&mids));
		g_autoptr(J) old =
			line_rpc(s, "/S4", "getContactsV2", legacy, error);
		if (!old)
			return NULL;
		J *raw = field(field(field(old, "1"), id), "3");

		row = raw ? v2_row(s, id, raw, FALSE) : NULL;
	} else {
		g_propagate_error(error, g_steal_pointer(&problem));
		return NULL;
	}
	if (!row)
		return fail(error, "LINE could not find this account.");
	return row;
}

/*
 * Incoming friend requests. A FriendRequest is eMid (1), mid (2),
 * direction (3), method (4), param (5), timestamp (6), seqId (7),
 * displayName (10) and picturePath (11). Only requests that name a
 * MID can be accepted by addFriendByMid, so others are omitted.
 */
J *line_friend_requests(Line *s, GError **error)
{
	g_autoptr(J) args = obj();

	json_object_object_add(args, "1:i32",
			       json_object_new_int(REQUEST_INCOMING));
	json_object_object_add(args, "2:i64", json_object_new_int64(0));
	g_autoptr(J) reply =
		line_rpc(s, "/S4", "getFriendRequests", args, error);
	if (!reply)
		return NULL;
	if (!json_object_is_type(reply, json_type_array) ||
	    json_object_array_length(reply) > MAX_PAGE)
		return fail(error, "LINE returned an unreadable request list.");
	J *rows = json_object_new_array();

	for (guint i = 0; i < json_object_array_length(reply); i++) {
		J *item = json_object_array_get_idx(reply, i);
		const char *mid = clean_string(field(item, "2"), 33);

		if (!valid_mid(mid) || !g_strcmp0(mid, s->mid) ||
		    json_object_get_int(field(item, "3")) != REQUEST_INCOMING)
			continue;
		J *row = make_row(s, mid, clean_string(field(item, "10"), 1024),
				  NULL, clean_string(field(item, "11"), 1024),
				  FALSE, FALSE, FALSE, FALSE);
		const char *emid = clean_string(field(item, "1"), 256);

		put_string(row, "status", "");
		put_string(row, "eMid", emid && valid_ticket(emid) ? emid : "");
		json_object_object_add(
			row, "time",
			json_object_new_int64(
				json_object_get_int64(field(item, "6"))));
		put_bool(row, "request", TRUE);
		json_object_array_add(rows, row);
	}
	return rows;
}

J *line_friend_request_decline(Line *s, J *request, GError **error)
{
	const char *id = clean_string(field(request, "id"), 33);
	const char *emid = strfield(request, "eMid");

	if (!valid_mid(id) || !flag(request, "request"))
		return fail(error, "Choose a friend request.");
	/* removeFriendRequest_args(direction, midOrEMid), unwrapped. */
	g_autoptr(J) args = obj();

	json_object_object_add(args, "1:i32",
			       json_object_new_int(REQUEST_INCOMING));
	put_string(args, "2:str", emid && *emid ? emid : id);
	g_autoptr(J) ack =
		line_rpc(s, "/S4", "removeFriendRequest", args, error);
	if (!ack)
		return NULL;
	J *result = obj();

	put_string(result, "id", id);
	return result;
}

/*
 * Add someone met outside friend search. The AddFriendTracking reference
 * union names where: 5 groupMemberList, 13 strangerMessage and
 * 21 friendRequest, each an empty marker struct as for recommendations.
 */
J *line_friend_add_from(Line *s, J *row, enum line_friend_source source,
			gint32 seq, GError **error)
{
	const char *id = clean_string(field(row, "id"), 33);

	if (seq <= 0 || !valid_mid(id) || !g_strcmp0(id, s->mid) ||
	    !flag(row, "canAdd") || flag(row, "friend") ||
	    flag(row, "blocked") || flag(row, "official") ||
	    (source != LINE_FROM_GROUP && source != LINE_FROM_STRANGER &&
	     source != LINE_FROM_REQUEST))
		return fail(error, "This person cannot be added right now.");
	g_autoptr(J) args = obj();
	J *request = child(args, "1:obj"), *tracking = child(request, "3:obj");
	char arm[12];

	json_object_object_add(request, "1:i32", json_object_new_int(seq));
	put_string(request, "2:str", id);
	g_snprintf(arm, sizeof(arm), "%d:obj", source);
	child(child(tracking, "2:obj"), arm);
	J *friend = added(s, args, row, error);

	const char *status = strfield(row, "status");

	if (friend)
		put_string(friend, "status", status ? status : "");
	return friend;
}
