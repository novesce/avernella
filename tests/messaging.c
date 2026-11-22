// SPDX-License-Identifier: GPL-3.0-or-later
#include "messaging.h"
#include "sealing.h"
#include <string.h>

#define ALICE "u00000000000000000000000000000001"
#define SELF "u00000000000000000000000000000002"
#define GROUP "c00000000000000000000000000000003"
#define BOB "u00000000000000000000000000000004"

static gboolean reject_key, repeat_cursor, older_contacts_service;
static unsigned int sends, pages, profiles, group_profiles;
static gboolean reject_read;
static unsigned int read_requests;
static const char *expected_body, *expected_chat, *expected_mention;
static J *recent_fixture, *previous_fixture;
static guint previous_requests;

/* All endpoints are fixtures. Only ciphertext preparation is substituted. */
J *line_seal_text(Line *s, const char *chat, const char *text, GError **error)
{
	(void)s;
	(void)chat;
	if (expected_body)
		g_assert_cmpstr(text, ==, expected_body);
	if (reject_key) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Missing fixture key.");
		return NULL;
	}
	return json_tokener_parse(
		"[\"salt\",\"ciphertext\",\"nonce\",\"sender\",\"recipient\"]");
}

J *line_seal_payload(Line *s, const char *chat, int type, J *payload,
		     GError **error)
{
	(void)type;
	return line_seal_text(s, chat, strfield(payload, "text"), error);
}

J *line_open_payload(Line *s, J *message, int type, GError **error)
{
	(void)s;
	(void)message;
	(void)type;
	g_set_error_literal(error, LINE_ERROR, 1, "Missing fixture key.");
	return NULL;
}

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)s;
	if (!strcmp(method, "getMessageBoxes")) {
		g_assert_true(json_object_get_boolean(
			field(field(args, "2:obj"), "5:bool")));
		pages++;
		if (repeat_cursor || !field(field(args, "2:obj"), "1:str")) {
			return json_tokener_parse(
				"{\"1\":[{\"1\":\"" ALICE
				"\",\"4\":{\"1\":20,\"2\":50}}],\"2\":true}");
		}
		g_assert_cmpstr(strfield(field(args, "2:obj"), "1:str"), ==,
				ALICE);
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"" GROUP
			"\",\"4\":{\"1\":22,\"2\":9007199254740993},"
			"\"5\":9007199254740991,\"6\":2,\"8\":9,\"10\":11},"
			"{\"1\":\"" SELF
			"\",\"4\":{\"1\":23,\"2\":61},\"10\":61}],\"2\":false}");
	}
	if (!strcmp(method, "sendChatChecked")) {
		read_requests++;
		g_assert_cmpstr(path, ==, "/S4");
		g_assert_cmpint(json_object_get_int(field(args, "1:i32")), ==,
				71);
		g_assert_cmpstr(strfield(args, "2:str"), ==, GROUP);
		g_assert_cmpstr(strfield(args, "3:str"), ==,
				"9007199254740993");
		g_assert_cmpint(json_object_object_length(args), ==, 3);
		if (reject_read) {
			g_set_error_literal(error, LINE_ERROR, LINE_TIMEOUT,
					    "Receipt timed out.");
			return NULL;
		}
		return obj();
	}
	if (!strcmp(method, "getMessageReadRange")) {
		J *ids = field(args, "2:list:str");

		g_assert_cmpstr(path, ==, "/S4");
		g_assert_cmpuint(json_object_array_length(ids), ==, 1);
		g_assert_cmpstr(json_object_get_string(
					json_object_array_get_idx(ids, 0)),
				==, GROUP);
		g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
				7);
		return json_tokener_parse(
			"[{\"1\":\"" ALICE "\",\"2\":{}},"
			"{\"1\":\"" GROUP "\",\"2\":{\"" ALICE "\":["
			"{\"1\":9007199254740992,\"2\":9007199254740992},"
			"{\"1\":9007199254740996,\"2\":9007199254740994}],"
			"\"" SELF "\":[{\"1\":1,\"2\":9007199254740996}]}}]");
	}
	if (!strcmp(method, "getContactsV3")) {
		profiles++;
		g_assert_cmpstr(path, ==, "/RE4");
		if (older_contacts_service) {
			g_set_error_literal(error, LINE_ERROR,
					    LINE_METHOD_UNAVAILABLE,
					    "Unavailable method.");
			return NULL;
		}
		return json_tokener_parse("{\"1\":[{\"1\":\"" ALICE
					  "\",\"3\":{\"2\":\"Alice\"}}]}");
	}
	if (!strcmp(method, "getContactsV2"))
		return json_tokener_parse(
			"{\"1\":{\"" ALICE
			"\":{\"3\":{\"22\":\"Alice\",\"27\":\"Al\"}}}}");
	if (!strcmp(method, "getChats")) {
		group_profiles++;
		return json_tokener_parse("{\"1\":[{\"2\":\"" GROUP
					  "\",\"6\":\"Tea club\"}]}");
	}
	if (!strcmp(method, "getRecentMessagesV2")) {
		g_assert_cmpstr(strfield(args, "2:str"), ==, ALICE);
		g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
				50);
		if (recent_fixture)
			return json_object_get(recent_fixture);
		return json_tokener_parse(
			"[{\"1\":\"" SELF
			"\",\"4\":\"103\",\"5\":20,\"15\":7,\"23\":2},"
			"{\"1\":\"" ALICE
			"\",\"4\":\"101\",\"5\":10,\"20\":[\"e\"]},"
			"{\"1\":\"" ALICE
			"\",\"4\":\"102\",\"5\":20,\"10\":\"Hello\","
			"\"18\":{\"e2eeVersion\":\"2\"},\"20\":[]}]");
	}
	if (!strcmp(method, "getPreviousMessagesV2WithRequest")) {
		J *req = field(args, "2:obj"), *end = field(req, "2:obj");

		previous_requests++;
		g_assert_cmpstr(path, ==, "/S4");
		g_assert_cmpstr(strfield(req, "1:str"), ==, ALICE);
		g_assert_cmpint(json_object_get_int64(field(end, "1:i64")), ==,
				133000);
		g_assert_cmpint(json_object_get_int64(field(end, "2:i64")), ==,
				G_GINT64_CONSTANT(9007199254740993));
		g_assert_cmpint(json_object_get_int(field(req, "3:i32")), ==,
				50);
		if (previous_fixture)
			return json_object_get(previous_fixture);
		return json_tokener_parse(
			"[{\"4\":\"9007199254740993\",\"6\":133000},"
			"{\"4\":\"9007199254740992\",\"6\":133000,\"10\":\"Older message\"}]");
	}
	if (!strcmp(method, "sendMessage")) {
		sends++;
		g_assert_cmpstr(path, ==, "/S4");
		g_assert_cmpint(json_object_get_int(field(args, "1:i32")), ==,
				42);
		J *msg = field(args, "2:obj");

		g_assert_null(field(
			msg,
			"10:str")); /* plaintext must never be transmitted */
		g_assert_cmpstr(strfield(msg, "2:str"), ==,
				expected_chat ? expected_chat : ALICE);
		g_assert_cmpuint(
			json_object_array_length(field(msg, "20:list:str")), ==,
			5);
		g_assert_cmpstr(strfield(field(msg, "18:map:str:str"),
					 "e2eeVersion"),
				==, "2");
		g_assert_cmpstr(strfield(field(msg, "18:map:str:str"),
					 "MENTION"),
				==, expected_mention);
		return json_tokener_parse("{\"4\":\"321\"}");
	}
	if (!strcmp(method, "getChatRoomAnnouncements")) {
		g_assert_cmpstr(strfield(args, "2:str"), ==, GROUP);
		return json_tokener_parse(
			"[{\"1\":9007199254740993,\"3\":{\"2\":\"Meeting\"},"
			"\"4\":\"" SELF "\",\"6\":1},"
			"{\"1\":11,\"4\":\"" ALICE "\",\"6\":1},{\"1\":0}]");
	}
	if (!strcmp(method, "createChatRoomAnnouncement")) {
		J *body = field(args, "4:obj");

		g_assert_cmpstr(strfield(args, "2:str"), ==, GROUP);
		g_assert_cmpint(json_object_get_int(field(args, "1:i32")), ==,
				43);
		g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
				0);
		g_assert_cmpint(json_object_get_int(field(body, "1:i32")), ==,
				5);
		g_assert_cmpstr(strfield(body, "2:str"), ==, "Meeting");
		g_assert_cmpstr(strfield(body, "3:str"), ==,
				"line://nv/chatMsg?chatId=" GROUP
				"&messageId=901");
		return json_tokener_parse(
			"{\"1\":12,\"3\":{\"2\":\"Meeting\"},\"6\":0}");
	}
	if (!strcmp(method, "removeChatRoomAnnouncement")) {
		g_assert_cmpstr(strfield(args, "2:str"), ==, GROUP);
		g_assert_cmpint(json_object_get_int(field(args, "1:i32")), ==,
				44);
		g_assert_cmpint(json_object_get_int64(field(args, "3:i64")), ==,
				G_GINT64_CONSTANT(9007199254740993));
		return obj();
	}
	g_error("Unexpected fixture RPC: %s", method);
}

static void chat_list(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) chats = line_chats(&s, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(pages, ==, 2);
	g_assert_cmpuint(json_object_array_length(chats), ==, 2);
	g_assert_cmpstr(strfield(json_object_array_get_idx(chats, 0), "name"),
			==, "Tea club");
	J *group = json_object_array_get_idx(chats, 0);

	g_assert_cmpstr(strfield(group, "lastId"), ==, "9007199254740993");
	g_assert_cmpstr(strfield(group, "lastSeenId"), ==, "9007199254740991");
	g_assert_cmpint(json_object_get_int64(field(group, "clearThrough")), ==,
			11);
	g_assert_cmpint(json_object_get_int64(field(group, "unread")), ==, 2);
	g_assert_cmpstr(strfield(json_object_array_get_idx(chats, 1), "name"),
			==, "Alice");
	repeat_cursor = TRUE;
	g_assert_null(line_chats(&s, &error));
	g_assert_nonnull(error);
	repeat_cursor = FALSE;
	g_clear_pointer(&s.profiles, g_hash_table_unref);
}

static void checked_watermark(void)
{
	Line s = { .mid = SELF, .seq = 71 };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) ack =
		line_mark_read(&s, GROUP, "9007199254740993", 71, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(ack, "chat"), ==, GROUP);
	g_assert_cmpstr(strfield(ack, "id"), ==, "9007199254740993");
	const char *const bad[] = {
		"0", "-1", "+9", "1e9", "9223372036854775808", ""
	};
	for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_assert_null(line_mark_read(&s, GROUP, bad[i], 71, &error));
		g_clear_error(&error);
	}
	g_assert_null(line_mark_read(&s, SELF, "9007199254740993", 71, &error));
	g_clear_error(&error);
	g_assert_null(line_mark_read(&s, GROUP, "9007199254740993", 0, &error));
	g_clear_error(&error);
	reject_read = TRUE;
	g_assert_null(
		line_mark_read(&s, GROUP, "9007199254740993", 71, &error));
	g_assert_error(error, LINE_ERROR, LINE_TIMEOUT);
	reject_read = FALSE;
	g_assert_cmpuint(read_requests, ==, 2);
	g_assert_cmpint(s.seq, ==, 71);
}

static void recent_messages(void)
{
	Line s = { .mid = SELF };

	older_contacts_service = TRUE;
	g_autoptr(GError) error = NULL;
	g_autoptr(J) history = line_history(&s, ALICE, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(history), ==, 3);
	J *oldest = json_object_array_get_idx(history, 0);

	g_assert_cmpstr(strfield(oldest, "text"), ==,
			"[Unable to decrypt this message]");
	g_assert_cmpstr(strfield(oldest, "from"), ==, "Al");
	g_assert_cmpstr(strfield(json_object_array_get_idx(history, 1), "text"),
			==, "Hello");
	J *last = json_object_array_get_idx(history, 2);

	g_assert_cmpstr(strfield(last, "text"), ==, "[Sticker]");
	g_assert_true(json_object_get_boolean(field(last, "own")));
	g_assert_cmpint(json_object_get_int(field(last, "readCount")), ==, 2);
	g_assert_null(field(oldest, "readCount"));
	older_contacts_service = FALSE;
	g_clear_pointer(&s.profiles, g_hash_table_unref);
}

static void peer_read_counts(void)
{
	Line s = { .mid = SELF };
	g_autoptr(GError) error = NULL;
	g_autoptr(J) ranges = line_peer_ranges(&s, GROUP, &error);
	g_autoptr(J) notices = obj();
	g_autoptr(J) raw = json_tokener_parse(
		"{\"9007199254740992\":{\"1\":\"" SELF "\"},"
		"\"9007199254740993\":{\"1\":\"" SELF "\"},"
		"\"9007199254740994\":{\"1\":\"" SELF "\",\"23\":3},"
		"\"9007199254740995\":{\"1\":\"" ALICE "\",\"23\":8}}");
	g_autoptr(J) op = json_tokener_parse("{\"3\":55,\"10\":\"" GROUP
					     "\",\"11\":\"" ALICE
					     "\",\"12\":\"9007199254740992\"}");

	g_assert_no_error(error);
	g_assert_nonnull(ranges);
	g_assert_cmpint(line_peer_read_notice(SELF, GROUP, notices, op), ==,
			PEER_NOTICE_ADDED);
	g_assert_cmpint(line_peer_read_notice(SELF, GROUP, notices, op), ==,
			PEER_NOTICE_UNCHANGED);
	json_object_object_add(op, "11", json_object_new_string(BOB));
	g_assert_cmpint(line_peer_read_notice(SELF, GROUP, notices, op), ==,
			PEER_NOTICE_ADDED);
	json_object_object_add(op, "11", json_object_new_string(SELF));
	g_assert_cmpint(line_peer_read_notice(SELF, GROUP, notices, op), ==,
			PEER_NOTICE_OTHER);
	g_assert_cmpint(line_peer_read_notice(SELF, ALICE, notices, op), ==,
			PEER_NOTICE_OTHER);
	json_object_object_add(op, "11", json_object_new_string(BOB));
	g_assert_cmpint(line_peer_read_notice(SELF, ALICE, notices, op), ==,
			PEER_NOTICE_UNCHANGED);

	g_autoptr(J) counts =
		line_peer_read_counts(SELF, GROUP, raw, ranges, notices);
	g_assert_cmpuint(json_object_object_length(counts), ==, 3);
	g_assert_cmpint(json_object_get_int(field(counts, "9007199254740992")),
			==, 2);
	g_assert_nonnull(field(counts, "9007199254740993"));
	g_assert_cmpint(json_object_get_int(field(counts, "9007199254740993")),
			==, 0);
	g_assert_cmpint(json_object_get_int(field(counts, "9007199254740994")),
			==, 3);
	g_assert_null(field(counts, "9007199254740995"));

	/* An early event still applies when the matching outgoing message arrives. */
	json_object_object_add(op, "11", json_object_new_string(BOB));
	json_object_object_add(op, "12",
			       json_object_new_string("9007199254740996"));
	g_assert_cmpint(line_peer_read_notice(SELF, GROUP, notices, op), ==,
			PEER_NOTICE_ADDED);
	json_object_object_add(raw, "9007199254740996",
			       json_tokener_parse("{\"1\":\"" SELF "\"}"));
	g_clear_pointer(&counts, json_object_put);
	counts = line_peer_read_counts(SELF, GROUP, raw, ranges, notices);
	g_assert_cmpint(json_object_get_int(field(counts, "9007199254740996")),
			==, 2);

	/* A single interval may arrive as the map value directly. */
	json_object_object_add(
		ranges, BOB,
		json_tokener_parse(
			"{\"1\":9007199254740993,\"2\":9007199254740993}"));
	g_clear_pointer(&counts, json_object_put);
	counts = line_peer_read_counts(SELF, GROUP, raw, ranges, notices);
	g_assert_cmpint(json_object_get_int(field(counts, "9007199254740993")),
			==, 1);
	g_clear_pointer(&counts, json_object_put);
	counts = line_peer_read_counts(SELF, GROUP, raw, NULL, notices);
	g_assert_cmpint(json_object_get_int(field(counts, "9007199254740992")),
			==, 2);
	g_assert_null(field(counts, "9007199254740993"));
}

static void encrypted_send(void)
{
	Line s = { .mid = SELF, .seq = 41 };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) receipt = line_send(&s, ALICE, "Hello", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(receipt, "id"), ==, "321");
	reject_key = TRUE;
	g_assert_null(line_send(&s, ALICE, "Hello", &error));
	reject_key = FALSE;
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_null(line_send(&s, "not-a-chat", "Hello", &error));
	g_assert_cmpuint(sends, ==, 1);
	g_assert_cmpint(s.seq, ==, 42);
}

static void previous_messages(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) prior = json_tokener_parse(
		"{\"x\":{\"4\":\"9007199254740994\",\"5\":1,\"6\":133002},"
		"\"y\":{\"4\":\"9007199254740993\",\"5\":3,\"6\":133000}}");
	g_autoptr(J) index = NULL;
	gboolean more = TRUE;

	g_autoptr(J) cursor = line_history_cursor(prior);
	g_autoptr(J) rows =
		line_history_older(&s, ALICE, cursor, &index, &more, &error);
	g_assert_no_error(error);
	g_assert_false(more);
	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(rows, 0), "text"),
			==, "Older message");
	g_assert_nonnull(field(index, "9007199254740992"));
	g_assert_null(field(index, "9007199254740993"));
	g_autoptr(J) invalid = obj();
	g_assert_null(
		line_history_older(&s, ALICE, invalid, NULL, &more, &error));
	g_assert_error(error, LINE_ERROR, 1);
}

static void recover_empty_recent(void)
{
	Line s = { .mid = SELF };
	g_autoptr(GError) error = NULL;
	g_autoptr(J) empty = json_object_new_array();
	g_autoptr(J) summary = json_tokener_parse(
		"{\"id\":\"" ALICE "\",\"lastId\":\"9007199254740993\","
		"\"time\":133000,\"clearThrough\":9007199254740990}");
	g_autoptr(J) page = json_tokener_parse(
		"[{\"4\":\"9007199254740993\",\"6\":133000,\"10\":\"Head\"},"
		"{\"4\":\"9007199254740992\",\"6\":132999,\"10\":\"Previous\"},"
		"{\"4\":\"9007199254740990\",\"6\":132998,\"10\":\"Cleared\"},"
		"{\"4\":\"9007199254740994\",\"6\":133000,\"10\":\"Newer\"}]");
	g_autoptr(J) raw = NULL;

	recent_fixture = empty;
	previous_fixture = page;
	previous_requests = 0;
	g_autoptr(J) rows =
		line_history_with_summary(&s, ALICE, summary, &raw, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(rows), ==, 2);
	g_assert_cmpuint(json_object_object_length(raw), ==, 2);
	g_assert_cmpstr(strfield(field(raw, "9007199254740993"), "10"), ==,
			"Head");
	g_assert_cmpstr(strfield(field(raw, "9007199254740992"), "10"), ==,
			"Previous");
	g_assert_cmpuint(previous_requests, ==, 1);

	gboolean more = TRUE;
	g_autoptr(J) boundary =
		json_tokener_parse("{\"4\":\"9007199254740993\",\"6\":133000}");
	g_autoptr(J) older = line_history_older_since(
		&s, ALICE, boundary, 9007199254740990, NULL, &more, &error);

	g_assert_no_error(error);
	g_assert_false(more);
	g_assert_cmpuint(json_object_array_length(older), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(older, 0), "text"),
			==, "Previous");

	previous_fixture = empty;
	g_clear_pointer(&rows, json_object_put);
	rows = line_history_with_summary(&s, ALICE, summary, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(rows), ==, 0);
	g_assert_cmpuint(previous_requests, ==, 3);

	recent_fixture = page;
	g_clear_pointer(&rows, json_object_put);
	rows = line_history_with_summary(&s, ALICE, summary, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(previous_requests, ==, 3);

	recent_fixture = empty;
	previous_fixture = summary; /* malformed page */
	g_assert_null(
		line_history_with_summary(&s, ALICE, summary, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);

	recent_fixture =
		summary; /* malformed recent must not trigger recovery */
	g_assert_null(
		line_history_with_summary(&s, ALICE, summary, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	recent_fixture = empty;
	json_object_object_del(summary, "lastId");
	g_clear_pointer(&rows, json_object_put);
	rows = line_history_with_summary(&s, ALICE, summary, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(rows), ==, 0);
	g_assert_cmpuint(previous_requests, ==, 4);
	recent_fixture = previous_fixture = NULL;
}

static void profile_reuse(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) e = NULL;
	profiles = group_profiles = 0;
	for (guint i = 0; i < 3; i++) {
		g_autoptr(J) chats = line_chats(&s, &e),
			     history = line_history(&s, ALICE, &e);
		g_assert_no_error(e);
		g_assert_nonnull(chats);
		g_assert_nonnull(history);
		g_assert_cmpstr(strfield(json_object_array_get_idx(history, 0),
					 "from"),
				==, "Alice");
	}
	g_assert_cmpuint(profiles, ==, 1);
	g_assert_cmpuint(group_profiles, ==, 1);
	g_autoptr(J) op = obj();
	json_object_object_add(op, "3", json_object_new_int(26));
	line_profile_operation(&s, op);
	g_assert_cmpuint(g_hash_table_size(s.profiles), ==, 2);
	json_object_object_add(op, "3", json_object_new_int(2));
	line_profile_operation(&s, op);
	g_assert_cmpuint(g_hash_table_size(s.profiles), ==, 0);
	g_autoptr(J) refresh = line_chats(&s, &e);
	g_assert_no_error(e);
	g_assert_cmpuint(profiles, ==, 2);
	g_assert_cmpuint(group_profiles, ==, 2);
	g_clear_pointer(&s.profiles, g_hash_table_unref);
}

static void mention_and_forward(void)
{
	Line s = { .mid = SELF, .seq = 41 };

	g_autoptr(J) mentions = json_tokener_parse(
		"[{\"id\":\"" ALICE "\",\"start\":3,\"length\":6}]");
	g_autoptr(GError) error = NULL;

	expected_chat = GROUP;
	expected_body = "🎉 @Alice";
	expected_mention =
		"{\"MENTIONEES\":[{\"S\":\"3\",\"E\":\"9\",\"M\":\"" ALICE
		"\"}]}";
	g_autoptr(J) receipt = line_send_decorated(
		&s, GROUP, expected_body, NULL, NULL, mentions, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(receipt, "id"), ==, "321");
	g_assert_cmpint(s.seq, ==, 42);
	J *range = json_object_array_get_idx(mentions, 0);

	json_object_object_add(range, "start", json_object_new_int(1));
	g_assert_null(line_send_decorated(&s, GROUP, expected_body, NULL, NULL,
					  mentions, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpint(s.seq, ==, 42);
	g_clear_error(&error);
	expected_mention = expected_chat = NULL;
	s.seq = 41;
	g_autoptr(J) raw = json_tokener_parse(
		"{\"10\":\"🎉 @Alice\",\"18\":{\"MENTION\":\"prior\"}}");
	g_autoptr(J) forwarded = line_forward_text(&s, ALICE, raw, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(strfield(forwarded, "id"), ==, "321");
	expected_body = NULL;
}

static void announcements(void)
{
	Line s = { .mid = SELF, .seq = 42 };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) rows = line_announcements(&s, GROUP, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(rows), ==, 2);
	J *mine = json_object_array_get_idx(rows, 0);
	J *theirs = json_object_array_get_idx(rows, 1);

	g_assert_cmpstr(strfield(mine, "id"), ==, "9007199254740993");
	g_assert_true(json_object_get_boolean(field(mine, "canRemove")));
	g_assert_false(json_object_get_boolean(field(theirs, "canRemove")));
	g_autoptr(J) result =
		line_announce_message(&s, GROUP, "901", "Meeting", 43, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(strfield(result, "id"), ==, "12");
	g_autoptr(J) removed = line_remove_announcement(
		&s, GROUP, strfield(mine, "id"), 44, &error);

	g_assert_no_error(error);
	g_assert_nonnull(removed);
	g_assert_cmpint(s.seq, ==, 42);
}

static void append_flex(J *rows, int id, int type, const char *text,
			const char *json, const char *alt)
{
	J *message = obj(), *metadata = obj();

	g_autofree char *sid = g_strdup_printf("%d", id);

	json_object_object_add(message, "1", json_object_new_string(ALICE));
	json_object_object_add(message, "4", json_object_new_string(sid));
	json_object_object_add(message, "5", json_object_new_int(id));
	json_object_object_add(message, "15", json_object_new_int(type));
	json_object_object_add(message, "18", metadata);
	if (text)
		json_object_object_add(message, "10",
				       json_object_new_string(text));
	if (json)
		json_object_object_add(metadata, "FLEX_JSON",
				       json_object_new_string(json));
	if (alt)
		json_object_object_add(metadata, "ALT_TEXT",
				       json_object_new_string(alt));
	json_object_array_add(rows, message);
}

static void flex_messages(void)
{
	Line s = { .mid = SELF };

	g_autoptr(J) input = json_object_new_array();
	g_autoptr(J) index = NULL;
	g_autoptr(GError) error = NULL;
	const char *bubble =
		"{\"type\":\"bubble\",\"body\":{\"type\":\"box\","
		"\"contents\":[{\"type\":\"text\",\"text\":\"รางวัล\"}]}}";
	const char *wrapped =
		"{\"type\":\"flex\",\"altText\":\"Card gallery\",\"contents\":"
		"{\"type\":\"carousel\",\"contents\":[{\"type\":\"bubble\"}]}}";
	const char *wrapped_unknown =
		"{\"type\":\"flex\",\"altText\":\"Coupon summary\","
		"\"contents\":{\"type\":\"future\"}}";
	g_autofree char *spacing = g_strnfill(64 * 1024, ' ');
	g_autofree char *oversized = g_strconcat(spacing, bubble, NULL);

	g_autoptr(GString) too_deep =
		g_string_new("{\"type\":\"bubble\",\"body\":");

	for (int i = 0; i < 30; i++)
		g_string_append_c(too_deep, '[');
	for (int i = 0; i < 30; i++)
		g_string_append_c(too_deep, ']');
	g_string_append_c(too_deep, '}');
	append_flex(input, 201, 22, "generic fallback", bubble, "ข้อเสนอพิเศษ");
	append_flex(input, 202, 22, "generic fallback", wrapped, NULL);
	append_flex(input, 203, 22, NULL, "{broken", "Readable alternative");
	append_flex(input, 204, 22, NULL, oversized, "Large card");
	append_flex(input, 205, 22, NULL, too_deep->str, "Deep card");
	append_flex(input, 206, 22, NULL, NULL, NULL);
	append_flex(input, 207, 0, "Ordinary text", bubble, "Unused");
	append_flex(input, 208, 22, "fallback", wrapped_unknown, "\xff");
	append_flex(input, 209, 22, "fallback", wrapped_unknown,
		    "Metadata wins");
	recent_fixture = input;
	g_autoptr(J) rows = line_history_raw(&s, ALICE, &index, &error);
	recent_fixture = NULL;

	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(rows), ==, 9);
	const char *const text[] = {
		"ข้อเสนอพิเศษ",	 "Card gallery",   "Readable alternative",
		"Large card",	 "Deep card",	   "[Flex message]",
		"Ordinary text", "Coupon summary", "Metadata wins"
	};
	for (size_t i = 0; i < G_N_ELEMENTS(text); i++) {
		J *row = json_object_array_get_idx(rows, i);
		J *flex = field(row, "flex");

		g_assert_cmpstr(strfield(row, "text"), ==, text[i]);
		if (i < 2) {
			g_assert_cmpstr(strfield(row, "type"), ==, "flex");
			g_assert_cmpstr(strfield(flex, "type"), ==,
					i == 0 ? "bubble" : "carousel");
		} else
			g_assert_null(flex);
	}
	g_assert_cmpstr(strfield(field(field(index, "201"), "18"), "FLEX_JSON"),
			==, bubble);
	g_clear_pointer(&s.profiles, g_hash_table_unref);
}

static void rich_messages(void)
{
	Line s = { .mid = SELF };

	g_autoptr(J) input = json_object_new_array();
	g_autoptr(J) index = NULL;
	g_autoptr(GError) error = NULL;
	const char *markup =
		"{\"canvas\":{\"width\":1040,\"height\":520,"
		"\"initialScene\":\"s\"},\"images\":{\"i\":{\"x\":0,\"y\":0,"
		"\"w\":1040,\"h\":520}},\"actions\":{\"a\":{\"type\":\"web\","
		"\"params\":{\"linkUri\":\"https://example.com\"}}},"
		"\"scenes\":{\"s\":{\"draws\":[{\"image\":\"i\",\"x\":0,"
		"\"y\":0,\"w\":1040,\"h\":520}],\"listeners\":[{\"type\":"
		"\"touch\",\"params\":[0,0,1040,520],\"action\":\"a\"}]}}}";

	append_flex(input, 301, 17, NULL, NULL, "Weekend menu");
	append_flex(input, 302, 17, NULL, NULL, NULL);
	J *rich = json_object_array_get_idx(input, 0);
	J *metadata = field(rich, "18");

	json_object_object_add(metadata, "MARKUP_JSON",
			       json_object_new_string(markup));
	json_object_object_add(
		metadata, "DOWNLOAD_URL",
		json_object_new_string("https://obs.line-scdn.net/r/menu"));
	recent_fixture = input;
	g_autoptr(J) rows = line_history_raw(&s, ALICE, &index, &error);
	recent_fixture = NULL;

	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(rows), ==, 2);
	J *row = json_object_array_get_idx(rows, 0);

	g_assert_cmpstr(strfield(row, "type"), ==, "rich");
	g_assert_cmpstr(strfield(row, "text"), ==, "Weekend menu");
	g_assert_cmpint(json_object_get_int(field(field(row, "rich"), "h")), ==,
			520);
	row = json_object_array_get_idx(rows, 1);
	g_assert_null(field(row, "rich"));
	g_assert_cmpstr(strfield(row, "text"), ==, "[Rich message]");
	g_clear_pointer(&s.profiles, g_hash_table_unref);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/messaging/chats", chat_list);
	g_test_add_func("/messaging/read-watermark", checked_watermark);
	g_test_add_func("/messaging/peer-read-counts", peer_read_counts);
	g_test_add_func("/messaging/history", recent_messages);
	g_test_add_func("/messaging/flex-history", flex_messages);
	g_test_add_func("/messaging/rich-messages", rich_messages);
	g_test_add_func("/messaging/previous-messages", previous_messages);
	g_test_add_func("/messaging/recover-empty-recent",
			recover_empty_recent);
	g_test_add_func("/messaging/send", encrypted_send);
	g_test_add_func("/messaging/profile-reuse", profile_reuse);
	g_test_add_func("/messaging/mentions-and-forwarding",
			mention_and_forward);
	g_test_add_func("/messaging/announcements", announcements);
	return g_test_run();
}
