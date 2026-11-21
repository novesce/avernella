// SPDX-License-Identifier: GPL-3.0-or-later
#include "groups.h"
#include "messaging.h"
#include <string.h>

static gpointer problem(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
	return NULL;
}

static gboolean mid(const char *s, char type)
{
	if (!s || s[0] != type || strlen(s) != 33)
		return FALSE;
	for (int i = 1; i < 33; i++)
		if (!g_ascii_isxdigit(s[i]))
			return FALSE;
	return TRUE;
}

static gboolean valid_users(Line *s, J *users, gboolean allow_empty,
			    GError **error)
{
	size_t n = json_object_is_type(users, json_type_array) ?
			   json_object_array_length(users) :
			   500;
	if (n > 499 || (!allow_empty && !n)) {
		problem(error, "Choose up to 499 friends to invite.");
		return FALSE;
	}
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (size_t i = 0; i < n; i++) {
		J *item = json_object_array_get_idx(users, i);
		const char *id = json_object_is_type(item, json_type_string) ?
					 json_object_get_string(item) :
					 NULL;
		if (!mid(id, 'u') || json_object_get_string_len(item) != 33 ||
		    !g_strcmp0(id, s->mid) ||
		    !g_hash_table_add(seen, (gpointer)id)) {
			problem(error,
				"Choose each friend once from the friends list.");
			return FALSE;
		}
	}
	return TRUE;
}

static J *read_group(Line *s, const char *chat, GError **error)
{
	if (!mid(chat, 'c'))
		return problem(error, "Choose a group conversation.");
	g_autoptr(J) args = obj();
	J *req = obj(), *ids = json_object_new_array();

	json_object_object_add(args, "1:obj", req);
	json_object_array_add(ids, json_object_new_string(chat));
	json_object_object_add(req, "1:list:str", ids);
	json_object_object_add(req, "2:bool", json_object_new_boolean(TRUE));
	json_object_object_add(req, "3:bool", json_object_new_boolean(TRUE));
	json_object_object_add(args, "2:i32", json_object_new_int(7));
	g_autoptr(J) reply = line_rpc(s, "/S4", "getChats", args, error);
	if (!reply)
		return NULL;
	J *rows = field(reply, "1");

	for (size_t i = 0; json_object_is_type(rows, json_type_array) &&
			   i < json_object_array_length(rows);
	     i++) {
		J *c = json_object_array_get_idx(rows, i);

		if (!g_strcmp0(strfield(c, "2"), chat))
			return json_object_get(c);
	}
	return problem(error, "LINE did not return this group.");
}

static J *group_row(J *chat, const char *fallback)
{
	J *row = obj();
	const char *name = strfield(chat, "6"), *id = strfield(chat, "2"),
		   *picture = strfield(chat, "7");
	json_object_object_add(row, "id", json_object_new_string(id));
	json_object_object_add(
		row, "name",
		json_object_new_string(name && *name ? name : fallback));
	if (picture)
		json_object_object_add(row, "avatarKey",
				       json_object_new_string(picture));
	return row;
}

static void collect_users(J *map, J *names)
{
	if (!json_object_is_type(map, json_type_object))
		return;
	json_object_object_foreach(map, id, ignored) {
		(void)ignored;
		if (mid(id, 'u'))
			json_object_object_add(names, id,
					       json_object_new_string(""));
	}
}

static int by_name(const void *a, const void *b)
{
	J *x = *(J *const *)a, *y = *(J *const *)b;
	int cmp = g_utf8_collate(strfield(x, "name"), strfield(y, "name"));

	return cmp ? cmp : strcmp(strfield(x, "id"), strfield(y, "id"));
}

static J *people(J *map, J *names)
{
	J *rows = json_object_new_array();

	if (!json_object_is_type(map, json_type_object))
		return rows;
	json_object_object_foreach(map, id, ignored) {
		(void)ignored;
		const char *name = strfield(names, id);

		if (!name)
			continue;
		J *row = obj();

		json_object_object_add(row, "id", json_object_new_string(id));
		json_object_object_add(
			row, "name", json_object_new_string(*name ? name : id));
		json_object_array_add(rows, row);
	}
	json_object_array_sort(rows, by_name);
	return rows;
}

J *line_group_members(Line *s, const char *id, GError **error)
{
	g_autoptr(J) chat = read_group(s, id, error);
	if (!chat)
		return NULL;
	J *extra = field(field(chat, "8"), "1");

	if (!json_object_is_type(extra, json_type_object))
		return problem(error,
			       "LINE did not return membership details.");
	J *members = field(extra, "4"), *pending = field(extra, "5");

	if (!json_object_is_type(members, json_type_object) ||
	    (pending && !json_object_is_type(pending, json_type_object)))
		return problem(error,
			       "LINE did not return a readable member list.");
	g_autoptr(J) names = obj();
	collect_users(members, names);
	collect_users(pending, names);
	if (s->mid && field(names, s->mid))
		json_object_object_add(names, s->mid,
				       json_object_new_string("You"));
	line_resolve_names(s, names, 'u');
	J *row = group_row(chat, id);

	json_object_object_add(row, "members", people(members, names));
	json_object_object_add(row, "invitees", people(pending, names));
	J *type = field(chat, "1");

	json_object_object_add(
		row, "canInvite",
		json_object_new_boolean(
			json_object_is_type(type, json_type_int) &&
			json_object_get_int(type) == 0));
	json_object_object_add(
		row, "canManage",
		json_object_new_boolean(
			json_object_is_type(type, json_type_int) &&
			json_object_get_int(type) == 0 && s->mid &&
			field(members, s->mid)));
	return row;
}

static char *clean_group_name(const char *name, GError **error)
{
	if (!name || !g_utf8_validate(name, -1, NULL))
		return problem(error, "Enter a group name.");
	g_autofree char *clean = g_strdup(name);

	g_strstrip(clean);
	gsize units = 0;

	for (const char *p = clean; *p; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);

		if (g_unichar_iscntrl(ch))
			return problem(error, "Use a single-line group name.");
		units += ch > 0xffff ? 2 : 1;
	}
	if (!units || units > 50)
		return problem(error,
			       "Enter a group name of up to 50 characters.");
	return g_steal_pointer(&clean);
}

J *line_group_create(Line *s, const char *name, J *users, GError **error)
{
	g_autofree char *clean = clean_group_name(name, error);

	if (!clean)
		return NULL;
	if (!valid_users(s, users, TRUE, error))
		return NULL;
	if (s->seq >= G_MAXINT32)
		return problem(error, "Pair a new session to create a group.");
	g_autoptr(J) args = obj();
	J *req = obj();

	json_object_object_add(args, "1:obj", req);
	json_object_object_add(req, "1:i32", json_object_new_int64(++s->seq));
	json_object_object_add(
		req, "2:i32",
		json_object_new_int(
			0)); /* GROUP, requires invitation acceptance */
	json_object_object_add(req, "3:str", json_object_new_string(clean));
	json_object_object_add(req, "4:set:str", json_object_get(users));
	g_autoptr(J) reply = line_rpc(s, "/S4", "createChat", args, error);
	if (!reply)
		return NULL;
	J *chat = field(reply, "1");

	if (!mid(strfield(chat, "2"), 'c'))
		return problem(
			error,
			"LINE did not confirm the new group. Refresh Chats before trying again.");
	return group_row(chat, clean);
}

J *line_group_invite(Line *s, const char *chat, J *users, GError **error)
{
	if (!valid_users(s, users, FALSE, error))
		return NULL;
	g_autoptr(J) info = read_group(s, chat, error);
	if (!info)
		return NULL;
	J *type = field(info, "1"), *extra = field(field(info, "8"), "1");

	if (!json_object_is_type(type, json_type_int) ||
	    json_object_get_int(type) != 0 ||
	    !json_object_is_type(extra, json_type_object))
		return problem(
			error,
			"This group does not use invitations that friends accept.");
	for (size_t i = 0; i < json_object_array_length(users); i++) {
		const char *id = json_object_get_string(
			json_object_array_get_idx(users, i));
		if (field(field(extra, "4"), id) ||
		    field(field(extra, "5"), id))
			return problem(
				error,
				"A selected friend is already a member or has a pending invitation. Refresh the member list.");
	}
	if (s->seq >= G_MAXINT32)
		return problem(error, "Pair a new session to invite friends.");
	g_autoptr(J) args = obj();
	J *req = obj();

	json_object_object_add(args, "1:obj", req);
	json_object_object_add(req, "1:i32", json_object_new_int64(++s->seq));
	json_object_object_add(req, "2:str", json_object_new_string(chat));
	json_object_object_add(req, "3:set:str", json_object_get(users));
	g_autoptr(J) ack = line_rpc(s, "/S4", "inviteIntoChat", args, error);
	if (!ack)
		return NULL;
	return group_row(info, chat);
}

J *line_group_invitations(Line *s, GError **error)
{
	g_autoptr(J) args = obj();
	J *req = obj();

	json_object_object_add(args, "1:obj", req);
	json_object_object_add(req, "1:bool", json_object_new_boolean(FALSE));
	json_object_object_add(req, "2:bool", json_object_new_boolean(TRUE));
	json_object_object_add(args, "2:i32", json_object_new_int(7));
	g_autoptr(J) reply = line_rpc(s, "/S4", "getAllChatMids", args, error);
	if (!reply)
		return NULL;
	J *ids = field(reply, "2");

	if (ids && !json_object_is_type(ids, json_type_array))
		return problem(
			error,
			"LINE returned an invalid group invitation list.");
	g_autoptr(J) names = obj();
	for (size_t i = 0; ids && i < json_object_array_length(ids); i++) {
		J *item = json_object_array_get_idx(ids, i);
		const char *id = json_object_is_type(item, json_type_string) ?
					 json_object_get_string(item) :
					 NULL;
		if (mid(id, 'c') && json_object_get_string_len(item) == 33)
			json_object_object_add(names, id,
					       json_object_new_string(""));
	}
	line_resolve_names(s, names, 'c');
	return people(names, names);
}

J *line_group_respond(Line *s, const char *chat, gboolean accept,
		      GError **error)
{
	if (!mid(chat, 'c'))
		return problem(error, "Select a received group invitation.");
	if (s->seq >= G_MAXINT32)
		return problem(error,
			       "Pair a new session to respond to invitations.");
	g_autoptr(J) args = obj();
	J *req = obj();

	json_object_object_add(args, "1:obj", req);
	json_object_object_add(req, "1:i32", json_object_new_int64(++s->seq));
	json_object_object_add(req, "2:str", json_object_new_string(chat));
	g_autoptr(J) ack = line_rpc(s, "/S4",
				    accept ? "acceptChatInvitation" :
					     "rejectChatInvitation",
				    args, error);
	if (!ack)
		return NULL;
	J *row = obj();

	json_object_object_add(row, "id", json_object_new_string(chat));
	return row;
}

/* Refresh the live membership before a destructive action or ticket access. */
static J *joined_group(Line *s, const char *id, GError **error)
{
	g_autoptr(J) group = read_group(s, id, error);
	if (!group)
		return NULL;
	J *type = field(group, "1"), *extra = field(field(group, "8"), "1");

	if (!json_object_is_type(type, json_type_int) ||
	    json_object_get_int(type) != 0 || !mid(s->mid, 'u') ||
	    !field(field(extra, "4"), s->mid))
		return problem(error,
			       "You must be a current member of this group.");
	return g_steal_pointer(&group);
}

static J *request(gint32 seq, J **inner)
{
	J *args = obj();
	*inner = obj();
	json_object_object_add(args, "1:obj", *inner);
	json_object_object_add(*inner, "1:i32", json_object_new_int(seq));
	return args;
}

static J *update_group(Line *s, J *group, J *chat, int attr, gint32 seq,
		       GError **error)
{
	J *req;

	g_autoptr(J) args = request(seq, &req);
	json_object_object_add(req, "2:obj", json_object_get(chat));
	json_object_object_add(req, "3:i32", json_object_new_int(attr));
	g_autoptr(J) ack = line_rpc(s, "/S4", "updateChat", args, error);
	return ack ? group_row(group, "Group") : NULL;
}

J *line_group_rename(Line *s, const char *id, const char *name, gint32 seq,
		     GError **error)
{
	g_autofree char *clean = clean_group_name(name, error);

	if (!clean)
		return NULL;
	if (seq <= 0)
		return problem(error, "Unable to reserve a group request.");
	g_autoptr(J) group = joined_group(s, id, error);
	if (!group)
		return NULL;
	if (!g_strcmp0(strfield(group, "6"), clean))
		return group_row(group, clean);
	g_autoptr(J) patch = obj();
	json_object_object_add(patch, "2:str", json_object_new_string(id));
	json_object_object_add(patch, "6:str", json_object_new_string(clean));
	J *result = update_group(s, group, patch, 1, seq, error);

	if (result)
		json_object_object_add(result, "name",
				       json_object_new_string(clean));
	return result;
}

static J *drop(Line *s, J *group, const char *user, const char *method,
	       gint32 seq, GError **error)
{
	J *req;

	g_autoptr(J) args = request(seq, &req);
	json_object_object_add(req, "2:str",
			       json_object_new_string(strfield(group, "2")));
	if (user) {
		J *targets = json_object_new_array();

		json_object_array_add(targets, json_object_new_string(user));
		json_object_object_add(req, "3:set:str", targets);
	}
	g_autoptr(J) ack = line_rpc(s, "/S4", method, args, error);
	return ack ? group_row(group, "Group") : NULL;
}

J *line_group_remove(Line *s, const char *id, const char *user,
		     gboolean pending, gint32 seq, GError **error)
{
	if (!mid(user, 'u') || !g_strcmp0(user, s->mid) || seq <= 0)
		return problem(
			error,
			"Select another member or a pending invitation.");
	g_autoptr(J) group = joined_group(s, id, error);
	if (!group)
		return NULL;
	J *extra = field(field(group, "8"), "1");

	if (!field(field(extra, pending ? "5" : "4"), user) ||
	    field(field(extra, pending ? "4" : "5"), user))
		return problem(
			error,
			"This person's group membership has changed. Refresh the member list.");
	return drop(s, group, user,
		    pending ? "cancelChatInvitation" : "deleteOtherFromChat",
		    seq, error);
}

J *line_group_leave(Line *s, const char *id, gint32 seq, GError **error)
{
	if (seq <= 0)
		return problem(error, "Unable to reserve a group request.");
	g_autoptr(J) group = joined_group(s, id, error);
	return group ? drop(s, group, NULL, "deleteSelfFromChat", seq, error) :
		       NULL;
}

static gboolean link_allowed(J *extra)
{
	J *blocked = field(extra, "7");

	return (!blocked || (json_object_is_type(blocked, json_type_boolean) &&
			     !json_object_get_boolean(blocked))) &&
	       json_object_is_type(field(extra, "2"), json_type_boolean);
}

static gboolean ticket_valid(J *v)
{
	if (!json_object_is_type(v, json_type_string))
		return FALSE;
	const char *s = json_object_get_string(v);
	int len = json_object_get_string_len(v);

	if (len < 1 || len > 192 || strlen(s) != (size_t)len)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && *s != '_' && *s != '-')
			return FALSE;
	return TRUE;
}

static J *ticket_row(J *group, gboolean allowed, gboolean enabled, J *ticket)
{
	J *result = group_row(group, "Group");

	json_object_object_add(result, "allowed",
			       json_object_new_boolean(allowed));
	json_object_object_add(result, "enabled",
			       json_object_new_boolean(enabled));
	if (allowed && enabled && ticket_valid(ticket)) {
		g_autofree char *url =
			g_strconcat("https://line.me/R/ti/g/",
				    json_object_get_string(ticket), NULL);
		json_object_object_add(result, "url",
				       json_object_new_string(url));
	}
	return result;
}

J *line_group_link(Line *s, const char *id, GError **error)
{
	g_autoptr(J) group = joined_group(s, id, error);
	if (!group)
		return NULL;
	J *extra = field(field(group, "8"), "1");
	gboolean allowed = link_allowed(extra);

	return ticket_row(group, allowed,
			  allowed &&
				  !json_object_get_boolean(field(extra, "2")),
			  field(extra, "3"));
}

J *line_group_set_link(Line *s, const char *id, gboolean enabled, gint32 seq,
		       GError **error)
{
	if (seq <= 0)
		return problem(error, "Unable to reserve a group request.");
	g_autoptr(J) group = joined_group(s, id, error);
	if (!group)
		return NULL;
	J *extra = field(field(group, "8"), "1");

	if (!link_allowed(extra))
		return problem(
			error,
			"LINE has disabled invitations by link for this group.");
	if (json_object_get_boolean(field(extra, "2")) == !enabled)
		return ticket_row(group, TRUE, enabled, field(extra, "3"));
	g_autoptr(J) patch = obj();
	J *u = obj(), *extra_patch = obj();

	json_object_object_add(patch, "2:str", json_object_new_string(id));
	json_object_object_add(patch, "8:obj", u);
	json_object_object_add(u, "1:obj", extra_patch);
	json_object_object_add(extra_patch, "2:bool",
			       json_object_new_boolean(!enabled));
	g_autoptr(J) ack = update_group(s, group, patch, 4, seq, error);
	/* LINE may issue a fresh ticket on enabling. Only a new read may show it. */
	return ack ? ticket_row(group, TRUE, enabled, NULL) : NULL;
}

J *line_group_renew_link(Line *s, const char *id, gint32 seq, GError **error)
{
	if (seq <= 0)
		return problem(error, "Unable to reserve a group request.");
	g_autoptr(J) group = joined_group(s, id, error);
	if (!group)
		return NULL;
	J *extra = field(field(group, "8"), "1");

	if (!link_allowed(extra) || json_object_get_boolean(field(extra, "2")))
		return problem(
			error,
			"Enable invitations by link before requesting a new link.");
	J *req;

	g_autoptr(J) args = request(seq, &req);
	json_object_object_add(req, "2:str", json_object_new_string(id));
	g_autoptr(J) ack = line_rpc(s, "/S4", "reissueChatTicket", args, error);
	if (!ack)
		return NULL;
	if (!ticket_valid(field(ack, "1")))
		return problem(
			error,
			"LINE returned no usable group link. Read the current link before trying again.");
	return ticket_row(group, TRUE, TRUE, field(ack, "1"));
}
