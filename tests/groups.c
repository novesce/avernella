// SPDX-License-Identifier: GPL-3.0-or-later
#include "groups.h"
#include "messaging.h"

#define GROUP "caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SELF "ubbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ALICE "ucccccccccccccccccccccccccccccccc"
#define BEN "udddddddddddddddddddddddddddddddd"
#define UNKNOWN "ueeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

static int reads, writes, mode, expected_seq;
static gboolean reject_write, list_incomplete;
static const char *expected_method;
static gboolean removed_self, link_off, link_prohibited;
static const char *ticket, *renewed;
static J *sent;

static J *targets(void)
{
	return json_tokener_parse("[\"" UNKNOWN "\"]");
}

void line_resolve_names(Line *s, J *names, char type)
{
	(void)s;
	if (type == 'c' && field(names, GROUP))
		json_object_object_add(names, GROUP,
				       json_object_new_string("Fixture group"));
	if (field(names, ALICE))
		json_object_object_add(names, ALICE,
				       json_object_new_string("Alice"));
	if (field(names, BEN))
		json_object_object_add(names, BEN,
				       json_object_new_string("Ben"));
}

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)s;
	g_autoptr(GError) wire_error = NULL;
	g_autoptr(GBytes) wire = line_encode_call(method, args, &wire_error);
	g_assert_no_error(wire_error);
	gsize wire_size;
	const guint8 *bytes = g_bytes_get_data(wire, &wire_size);
	gsize skip = 4 + strlen(method);

	g_assert_cmpuint(wire_size, >, skip);
	g_autoptr(J) unpacked =
		line_decode_struct(bytes + skip, wire_size - skip, &wire_error);
	g_assert_no_error(wire_error);
	g_assert_cmpstr(path, ==, "/S4");
	J *r = field(args, "1:obj");

	if (!strcmp(method, "getChats")) {
		reads++;
		g_assert_cmpstr(
			json_object_get_string(json_object_array_get_idx(
				field(r, "1:list:str"), 0)),
			==, GROUP);
		g_assert_true(json_object_get_boolean(field(r, "2:bool")));
		g_assert_true(json_object_get_boolean(field(r, "3:bool")));
		J *reply = json_tokener_parse(
			"{\"1\":[{\"1\":0,\"2\":\"" GROUP
			"\",\"6\":\"Fixture group\",\"8\":{\"1\":{\"4\":{\"" ALICE
			"\":9,\"" SELF "\":11},\"5\":{\"" BEN "\":15}}}}]}");
		J *g = json_object_array_get_idx(field(reply, "1"), 0);

		json_object_object_add(g, "1", json_object_new_int(mode));
		J *extra = field(field(g, "8"), "1");

		if (removed_self)
			json_object_object_del(field(extra, "4"), SELF);
		json_object_object_add(
			extra, "7", json_object_new_boolean(link_prohibited));
		json_object_object_add(extra, "2",
				       json_object_new_boolean(link_off));
		if (ticket)
			json_object_object_add(extra, "3",
					       json_object_new_string(ticket));
		if (list_incomplete)
			json_object_object_del(g, "8");
		return reply;
	}
	if (!strcmp(method, "getAllChatMids")) {
		reads++;
		g_assert_false(json_object_get_boolean(field(r, "1:bool")));
		g_assert_true(json_object_get_boolean(field(r, "2:bool")));
		return json_tokener_parse("{\"2\":[\"" GROUP "\",\"" GROUP
					  "\",\"" SELF "\"]}");
	}
	writes++;
	g_clear_pointer(&sent, json_object_put);
	sent = json_object_get(field(unpacked, "1"));
	g_assert_cmpstr(method, ==, expected_method);
	g_assert_cmpint(json_object_get_int(field(r, "1:i32")), ==,
			expected_seq);
	if (reject_write) {
		g_set_error_literal(error, LINE_ERROR, LINE_TIMEOUT,
				    "Unconfirmed write");
		return NULL;
	}
	if (!strcmp(method, "createChat")) {
		g_assert_cmpint(json_object_get_int(field(r, "2:i32")), ==, 0);
		g_assert_cmpstr(strfield(r, "3:str"), ==, "Camping");
		g_assert_true(json_object_is_type(field(r, "4:set:str"),
						  json_type_array));
		return json_tokener_parse("{\"1\":{\"1\":0,\"2\":\"" GROUP
					  "\",\"6\":\"Camping\"}}");
	}
	if (!strcmp(method, "updateChat")) {
		g_assert_cmpstr(strfield(field(sent, "2"), "2"), ==, GROUP);
		return obj();
	}
	if (!strcmp(method, "reissueChatTicket")) {
		J *resp = obj();

		if (renewed)
			json_object_object_add(resp, "1",
					       json_object_new_string(renewed));
		return resp;
	}
	g_assert_cmpstr(strfield(r, "2:str"), ==, GROUP);
	if (!strcmp(method, "inviteIntoChat")) {
		g_assert_cmpuint(
			json_object_array_length(field(r, "3:set:str")), ==, 1);
		g_assert_cmpstr(
			json_object_get_string(json_object_array_get_idx(
				field(r, "3:set:str"), 0)),
			==, UNKNOWN);
	}
	return obj();
}

static void create(void)
{
	Line s = { .mid = SELF, .seq = 16 };

	writes = 0;
	expected_seq = 17;
	expected_method = "createChat";
	reject_write = FALSE;
	g_autoptr(GError) error = NULL;
	g_autoptr(J) users = targets();
	g_autoptr(J) result =
		line_group_create(&s, " Camping  ", users, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(result, "id"), ==, GROUP);
	g_assert_cmpstr(strfield(result, "name"), ==, "Camping");
	g_assert_cmpint(s.seq, ==, 17);
	g_assert_cmpint(writes, ==, 1);
	expected_seq = 18;
	reject_write = TRUE;
	g_assert_null(line_group_create(&s, "Camping", users, &error));
	g_assert_error(error, LINE_ERROR, LINE_TIMEOUT);
	g_assert_cmpint(writes, ==, 2);
	reject_write = FALSE;
}

static void invalid(void)
{
	Line s = { .mid = SELF, .seq = 1 };

	writes = 0;
	g_autoptr(J) users = targets();
	g_assert_null(line_group_create(&s, " ", users, NULL));
	g_assert_null(line_group_create(&s, "bad\nname", users, NULL));
	g_assert_null(line_group_create(&s, "Camping", NULL, NULL));
	g_autofree char *long_name = g_strnfill(51, 'x');

	g_assert_null(line_group_create(&s, long_name, users, NULL));
	json_object_array_add(users, json_object_new_string(UNKNOWN));
	g_assert_null(line_group_create(&s, "Camping", users, NULL));
	json_object_array_put_idx(users, 1, json_object_new_string(SELF));
	g_assert_null(line_group_create(&s, "Camping", users, NULL));
	json_object_array_put_idx(users, 1, json_object_new_string(GROUP));
	g_assert_null(line_group_create(&s, "Camping", users, NULL));
	g_assert_cmpint(writes, ==, 0);
	g_assert_cmpint(s.seq, ==, 1);
}

static void members(void)
{
	Line s = { .mid = SELF };

	reads = writes = 0;
	mode = 0;
	list_incomplete = FALSE;
	g_autoptr(J) view = line_group_members(&s, GROUP, NULL);
	g_assert_nonnull(view);
	g_assert_cmpint(reads, ==, 1);
	g_assert_true(json_object_get_boolean(field(view, "canInvite")));
	J *members = field(view, "members"), *pending = field(view, "invitees");

	g_assert_cmpuint(json_object_array_length(members), ==, 2);
	g_assert_cmpuint(json_object_array_length(pending), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(members, 0), "name"),
			==, "Alice");
	g_assert_cmpstr(strfield(json_object_array_get_idx(members, 1), "name"),
			==, "You");
	g_assert_cmpstr(strfield(json_object_array_get_idx(pending, 0), "name"),
			==, "Ben");
	list_incomplete = TRUE;
	g_assert_null(line_group_members(&s, GROUP, NULL));
	list_incomplete = FALSE;
	g_assert_cmpint(writes, ==, 0);
}

static void invite(void)
{
	Line s = { .mid = SELF, .seq = 10 };

	g_autoptr(J) users = targets();
	mode = 1;
	writes = reads = 0;
	list_incomplete = FALSE;
	g_assert_null(line_group_invite(&s, GROUP, users, NULL));
	g_assert_cmpint(writes, ==, 0);
	mode = 0;
	expected_method = "inviteIntoChat";
	expected_seq = 11;
	g_autoptr(J) ack = line_group_invite(&s, GROUP, users, NULL);
	g_assert_nonnull(ack);
	json_object_array_put_idx(users, 0, json_object_new_string(BEN));
	g_assert_null(line_group_invite(&s, GROUP, users, NULL));
	json_object_array_put_idx(users, 0, json_object_new_string(ALICE));
	g_assert_null(line_group_invite(&s, GROUP, users, NULL));
	g_assert_cmpint(writes, ==, 1);
	g_assert_cmpint(s.seq, ==, 11);
}

static void respond(void)
{
	Line s = { .mid = SELF, .seq = 12 };

	writes = reads = 0;
	g_autoptr(J) rows = line_group_invitations(&s, NULL);
	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(rows, 0), "name"),
			==, "Fixture group");
	g_assert_cmpint(writes, ==, 0);
	expected_method = "acceptChatInvitation";
	expected_seq = 13;
	g_autoptr(J) join = line_group_respond(&s, GROUP, TRUE, NULL);
	g_assert_cmpstr(strfield(join, "id"), ==, GROUP);
	expected_method = "rejectChatInvitation";
	expected_seq = 14;
	g_autoptr(J) decline = line_group_respond(&s, GROUP, FALSE, NULL);
	g_assert_cmpstr(strfield(decline, "id"), ==, GROUP);
	g_assert_null(line_group_respond(&s, SELF, FALSE, NULL));
	g_assert_cmpint(writes, ==, 2);
	g_assert_cmpint(s.seq, ==, 14);
}

static void reset_management(void)
{
	reads = writes = mode = 0;
	removed_self = reject_write = list_incomplete = link_off =
		link_prohibited = FALSE;
	ticket = "live_Ticket-123";
	renewed = "renewed_Ticket-456";
	expected_seq = 31;
}

static void rename_group(void)
{
	reset_management();
	Line s = { .mid = SELF, .seq = 31 };

	expected_method = "updateChat";
	g_autoptr(GError) e = NULL;
	g_autoptr(J) ack =
		line_group_rename(&s, GROUP, "  New group  ", 31, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(strfield(ack, "name"), ==, "New group");
	g_assert_cmpint(json_object_get_int(field(sent, "3")), ==, 1);
	g_assert_cmpstr(strfield(field(sent, "2"), "6"), ==, "New group");
	g_assert_cmpint(json_object_object_length(field(sent, "2")), ==, 2);
	g_assert_null(line_group_rename(&s, GROUP, "bad\nname", 31, &e));
	g_clear_error(&e);
	g_autoptr(J) unchanged =
		line_group_rename(&s, GROUP, "Fixture group", 31, &e);
	g_assert_nonnull(unchanged);
	removed_self = TRUE;
	g_assert_null(line_group_rename(&s, GROUP, "Stranger", 31, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_cmpint(writes, ==, 1);
	g_assert_cmpint(s.seq, ==, 31);
}

static void manage_members(void)
{
	reset_management();
	Line s = { .mid = SELF, .seq = 31 };

	expected_method = "cancelChatInvitation";
	g_autoptr(J) canceled =
		line_group_remove(&s, GROUP, BEN, TRUE, 31, NULL);
	g_assert_nonnull(canceled);
	g_assert_cmpstr(json_object_get_string(
				json_object_array_get_idx(field(sent, "3"), 0)),
			==, BEN);
	expected_method = "deleteOtherFromChat";
	g_autoptr(J) removed =
		line_group_remove(&s, GROUP, ALICE, FALSE, 31, NULL);
	g_assert_nonnull(removed);
	g_assert_cmpstr(json_object_get_string(
				json_object_array_get_idx(field(sent, "3"), 0)),
			==, ALICE);
	g_assert_null(line_group_remove(&s, GROUP, ALICE, TRUE, 31, NULL));
	g_assert_null(line_group_remove(&s, GROUP, BEN, FALSE, 31, NULL));
	g_assert_null(line_group_remove(&s, GROUP, UNKNOWN, FALSE, 31, NULL));
	g_assert_null(line_group_remove(&s, GROUP, SELF, FALSE, 31, NULL));
	g_assert_cmpint(writes, ==, 2);
	removed_self = TRUE;
	g_assert_null(line_group_leave(&s, GROUP, 31, NULL));
	removed_self = FALSE;
	mode = 1;
	g_assert_null(line_group_leave(&s, GROUP, 31, NULL));
	mode = 0;
	expected_method = "deleteSelfFromChat";
	g_autoptr(J) left = line_group_leave(&s, GROUP, 31, NULL);
	g_assert_nonnull(left);
	g_assert_cmpint(json_object_object_length(sent), ==, 2);
	g_assert_cmpint(writes, ==, 3);
}

static void links(void)
{
	reset_management();
	Line s = { .mid = SELF };

	g_autoptr(J) info = line_group_link(&s, GROUP, NULL);
	g_assert_true(json_object_get_boolean(field(info, "allowed")));
	g_assert_true(json_object_get_boolean(field(info, "enabled")));
	g_assert_cmpstr(strfield(info, "url"), ==,
			"https://line.me/R/ti/g/live_Ticket-123");
	g_assert_cmpint(writes, ==, 0);
	expected_method = "updateChat";
	g_autoptr(J) off = line_group_set_link(&s, GROUP, FALSE, 31, NULL);
	g_assert_nonnull(off);
	g_assert_null(field(off, "url"));
	g_assert_cmpint(json_object_get_int(field(sent, "3")), ==, 4);
	J *extra = field(field(field(sent, "2"), "8"), "1");

	g_assert_true(json_object_get_boolean(field(extra, "2")));
	g_assert_cmpint(json_object_object_length(extra), ==, 1);
	link_off = TRUE;
	g_autoptr(J) off_info = line_group_link(&s, GROUP, NULL);
	g_assert_null(field(off_info, "url"));
	g_assert_null(line_group_renew_link(&s, GROUP, 31, NULL));
	g_assert_cmpint(writes, ==, 1);
	g_autoptr(J) on = line_group_set_link(&s, GROUP, TRUE, 31, NULL);
	g_assert_nonnull(on);
	g_assert_null(field(on, "url"));
	extra = field(field(field(sent, "2"), "8"), "1");
	g_assert_false(json_object_get_boolean(field(extra, "2")));
	link_off = FALSE;
	expected_method = "reissueChatTicket";
	g_autoptr(J) renewed_info = line_group_renew_link(&s, GROUP, 31, NULL);
	g_assert_cmpstr(strfield(renewed_info, "url"), ==,
			"https://line.me/R/ti/g/renewed_Ticket-456");
	g_assert_cmpint(json_object_object_length(sent), ==, 2);
	g_assert_cmpstr(strfield(sent, "2"), ==, GROUP);
	renewed = "../bad";
	g_assert_null(line_group_renew_link(&s, GROUP, 31, NULL));
	g_assert_cmpint(writes, ==, 4);
	link_prohibited = TRUE;
	g_autoptr(J) locked = line_group_link(&s, GROUP, NULL);
	g_assert_null(field(locked, "url"));
	g_assert_false(json_object_get_boolean(field(locked, "allowed")));
	g_assert_null(line_group_set_link(&s, GROUP, TRUE, 31, NULL));
	g_assert_null(line_group_renew_link(&s, GROUP, 31, NULL));
	link_prohibited = FALSE;
	ticket = "https://foreign.example";
	g_autoptr(J) malformed = line_group_link(&s, GROUP, NULL);
	g_assert_null(field(malformed, "url"));
	removed_self = TRUE;
	g_assert_null(line_group_link(&s, GROUP, NULL));
	g_assert_cmpint(writes, ==, 4);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/groups/create", create);
	g_test_add_func("/groups/validate", invalid);
	g_test_add_func("/groups/members", members);
	g_test_add_func("/groups/invite", invite);
	g_test_add_func("/groups/respond", respond);
	g_test_add_func("/groups/manage/rename", rename_group);
	g_test_add_func("/groups/manage/membership", manage_members);
	g_test_add_func("/groups/manage/links", links);
	int result = g_test_run();

	g_clear_pointer(&sent, json_object_put);
	return result;
}
