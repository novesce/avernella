// SPDX-License-Identifier: GPL-3.0-or-later
#include "friends.h"
#include <string.h>

#define ALICE "u00000000000000000000000000000001"
#define SELF "u00000000000000000000000000000002"
#define BLOCK "u00000000000000000000000000000003"
#define HIDE "u00000000000000000000000000000004"
#define OTHER "u00000000000000000000000000000005"

struct rpc_step {
	const char *path, *method, *request, *reply;
};
static const struct rpc_step *script;
static guint calls, nsteps;

static void use(const struct rpc_step *steps, guint n)
{
	script = steps;
	nsteps = n;
	calls = 0;
}

/*
 * Only HTTP is substituted. Compare Thrift-decoded argument fields and
 * route both success and service errors through the real reply decoder.
 */
J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	g_assert_cmpuint(calls, <, nsteps);
	const struct rpc_step *step = &script[calls++];

	g_assert_cmpstr(path, ==, step->path);
	g_assert_cmpstr(method, ==, step->method);
	g_autoptr(GError) validation = NULL;
	g_autoptr(GBytes) call = line_encode_call(method, args, &validation);
	g_assert_no_error(validation);
	gsize bytes;
	const guint8 *wire = g_bytes_get_data(call, &bytes);
	gsize header =
		4 + strlen(method); /* method byte length fits one varint */
	g_assert_cmpuint(bytes, >, header);
	g_autoptr(J) request =
		line_decode_struct(wire + header, bytes - header, &validation);
	g_assert_no_error(validation);
	g_autoptr(J) expected = json_tokener_parse(step->request);
	if (!json_object_equal(expected, request)) {
		g_error("Unexpected arguments for %s: %s", method,
			json_object_to_json_string(request));
	}
	s->service_code = -1;
	if (!strcmp(step->reply, "timeout")) {
		g_set_error_literal(error, LINE_ERROR, LINE_TIMEOUT,
				    "Fixture timed out.");
		return NULL;
	}
	gboolean unavailable = !strcmp(step->reply, "unavailable");

	g_autoptr(J) synthetic =
		json_tokener_parse(unavailable ? "{\"2:i32\":1}" : step->reply);
	g_autoptr(GBytes) result =
		line_encode_call(method, synthetic, &validation);
	g_assert_no_error(validation);
	wire = g_bytes_get_data(result, &bytes);
	g_autofree guint8 *response = g_memdup2(wire, bytes);

	response[1] = unavailable ? 0x61 :
				    0x41; /* application exception or reply */
	return line_decode_reply(method, response, bytes, &s->service_code,
				 error);
}

#define FIND_ID "{\"1\":{\"1\":{\"1\":\"alice.1\"}}}"
#define FIND_TICKET "{\"1\":{\"1\":{\"2\":\"AbC_1-xy\"}}}"
#define FIND_PHONE "{\"1\":{\"1\":\"+12025550123\"}}"
#define VISIBLE                                           \
	"{\"1:str\":\"" ALICE                             \
	"\",\"3:obj\":{\"2:str\":\"Alice\",\"3:str\":\"/" \
	"photo\"},\"4:obj\":{\"3:obj\":{}},\"5:obj\":{\"3:obj\":{}}}"
#define ADD_CORE "{\"1\":{\"1\":42,\"2\":\"" ALICE "\",\"3\":"
#define ADD_ID                                                                                       \
	ADD_CORE                                                                                     \
	"{\"1\":\"{\\\"screen\\\":\\\"friendAdd:idSearch\\\",\\\"spec\\\":\\\"native\\\"}\",\"2\":{" \
	"\"3\":{\"1\":\"alice.1\"}}}}}"
#define ADD_TICKET ADD_CORE "{\"2\":{\"4\":{\"1\":\"AbC_1-xy\"}}}}}"
#define ADD_PHONE                                                                                       \
	ADD_CORE                                                                                        \
	"{\"1\":\"{\\\"screen\\\":\\\"friendAdd:phoneSearch\\\",\\\"spec\\\":\\\"native\\\"}\",\"2\":{" \
	"\"2\":{\"1\":\"+12025550123\"}}}}}"

static void find_add(void)
{
	const struct {
		const char *kind, *value, *method, *find, *add;
	} inputs[] = {
		{ "id", "alice.1", "findContactBySearchIdOrTicketV3", FIND_ID,
		  ADD_ID },
		{ "link", "https://line.me/R/ti/p/AbC_1-xy?from=qr",
		  "findContactBySearchIdOrTicketV3", FIND_TICKET, ADD_TICKET },
		{ "link", "line://ti/p/~alice.1",
		  "findContactBySearchIdOrTicketV3", FIND_ID, ADD_ID },
		{ "link", "AbC_1-xy", "findContactBySearchIdOrTicketV3",
		  FIND_TICKET, ADD_TICKET },
		{ "phone", "+12025550123", "findContactByPhoneV3", FIND_PHONE,
		  ADD_PHONE },
	};

	for (guint i = 0; i < G_N_ELEMENTS(inputs); i++) {
		struct rpc_step steps[] = {
			{ "/RE4", inputs[i].method, inputs[i].find,
			  "{\"0:obj\":" VISIBLE "}" },
			{ "/RE4", "addFriendByMid", inputs[i].add,
			  "{\"0:obj\":{}}" }
		};
		use(steps, G_N_ELEMENTS(steps));
		Line s = { .mid = SELF, .seq = 42 };

		g_autoptr(GError) error = NULL;
		g_autoptr(J) who = line_friend_lookup(&s, inputs[i].kind,
						      inputs[i].value, &error);
		g_assert_no_error(error);
		g_assert_nonnull(who);
		g_assert_cmpuint(calls, ==, 1);
		g_assert_cmpstr(strfield(who, "name"), ==, "Alice");
		g_assert_cmpstr(strfield(s.pictures, ALICE), ==, "/photo");
		g_assert_true(json_object_get_boolean(field(who, "canAdd")));
		g_assert_false(json_object_get_boolean(field(who, "friend")));
		g_autoptr(J) added = line_friend_add(
			&s, who, inputs[i].kind, inputs[i].value, 42, &error);
		g_assert_no_error(error);
		g_assert_nonnull(added);
		g_assert_true(json_object_get_boolean(field(added, "friend")));
		g_assert_false(json_object_get_boolean(field(added, "canAdd")));
		g_assert_false(json_object_get_boolean(field(who, "friend")));
		g_assert_cmpuint(calls, ==, 2);
		g_assert_cmpint(s.seq, ==, 42);
		g_clear_pointer(&s.pictures, json_object_put);
	}
}

static void local_and_remote_rejections(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	use(NULL, 0);
	const char *const links[] = { "https://line.me.example/ti/p/123",
				      "https://line.me/ti/g/xxx",
				      "https://bad@line.me/ti/p/test",
				      "https://line.me/ti/p/%2fother",
				      "https://line.me/ti/p/",
				      "https://line.me/ti/p/yes/extra" };
	for (guint i = 0; i < G_N_ELEMENTS(links); i++) {
		g_assert_null(line_friend_lookup(&s, "link", links[i], &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	g_assert_null(line_friend_lookup(&s, "id", "a*", &error));
	g_clear_error(&error);
	g_assert_null(line_friend_lookup(&s, "phone", "+0", &error));
	g_clear_error(&error);
	g_assert_null(line_friend_lookup(&s, "phone", "+1 2025550123", &error));
	g_clear_error(&error);
	const char *const replies[] = {
		"{\"1:obj\":{\"1:i32\":1}}",
		"{\"1:obj\":{\"1:i32\":2}}",
		"{\"1:obj\":{\"1:i32\":4}}",
		"timeout",
		"{\"0:obj\":{}}",
	};
	const char *const messages[] = { "available", "age verification",
					 "temporarily", "timed out",
					 "available" };
	for (guint i = 0; i < G_N_ELEMENTS(replies); i++) {
		struct rpc_step step = { "/RE4",
					 "findContactBySearchIdOrTicketV3",
					 FIND_ID, replies[i] };
		use(&step, 1);
		g_assert_null(line_friend_lookup(&s, "id", "alice.1", &error));
		g_assert_nonnull(error);
		g_assert_nonnull(strstr(error->message, messages[i]));
		/* Only LINE's own limit (reason 4) pauses further searches. */
		g_assert_true(g_error_matches(error, LINE_ERROR,
					      LINE_RATE_LIMITED) == (i == 2));
		g_clear_error(&error);
		g_assert_cmpuint(calls, ==, 1);
		g_assert_cmpint(s.service_code, ==, -1);
	}
}

/* Lookups are spaced, bursts capped, and LINE's own limit starts a pause. */
static void pacing(void)
{
	const gint64 second = G_USEC_PER_SEC, start = 1000 * second;
	struct line_pace pace = { 0 };
	const struct line_pace_rule *rule = &line_lookup_pace;

	g_assert_cmpint(line_pace_wait(&pace, rule, start), ==, 0);
	line_pace_record(&pace, rule, start, FALSE);
	g_assert_cmpint(line_pace_wait(&pace, rule, start + second), ==, 2);
	g_assert_cmpint(line_pace_wait(&pace, rule, start + 3 * second), ==, 0);

	/* Ten lookups well spaced, then the eleventh waits out the window. */
	gint64 now = start;

	for (guint i = 1; i < rule->burst; i++) {
		now += 10 * second;
		g_assert_cmpint(line_pace_wait(&pace, rule, now), ==, 0);
		line_pace_record(&pace, rule, now, FALSE);
	}
	now += 10 * second;
	gint64 wait = line_pace_wait(&pace, rule, now);

	g_assert_cmpint(wait, ==, (start + rule->window_us - now) / second);
	g_assert_cmpint(line_pace_wait(&pace, rule, start + rule->window_us),
			==, 0);

	/* After LINE reports a limit, nothing is attempted for an hour. */
	struct line_pace limited = { 0 };

	line_pace_record(&limited, rule, start, TRUE);
	g_assert_cmpint(line_pace_wait(&limited, rule,
				       start + 30 * 60 * second),
			==, 30 * 60);
	g_assert_cmpint(line_pace_wait(&limited, rule,
				       start + rule->cooldown_us),
			==, 0);

	/* Adds have no gap: Add right after a search is allowed. */
	struct line_pace adds = { 0 };

	line_pace_record(&adds, &line_add_pace, start, FALSE);
	g_assert_cmpint(line_pace_wait(&adds, &line_add_pace, start), ==, 0);
}

static void protected_relationships(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	const char *const replies[] = { "{\"0:obj\":{\"1:str\":\"" ALICE
					"\",\"4:obj\":{\"1:obj\":{}}}}",
					"{\"0:obj\":{\"1:str\":\"" ALICE
					"\",\"5:obj\":{\"1:obj\":{}}}}",
					"{\"0:obj\":{\"1:str\":\"" SELF
					"\"}}" };
	const char *const flags[] = { "friend", "blocked", "self" };

	for (guint i = 0; i < G_N_ELEMENTS(replies); i++) {
		struct rpc_step step = { "/RE4",
					 "findContactBySearchIdOrTicketV3",
					 FIND_ID, replies[i] };
		use(&step, 1);
		g_autoptr(J) who =
			line_friend_lookup(&s, "id", "alice.1", &error);
		g_assert_no_error(error);
		g_assert_true(json_object_get_boolean(field(who, flags[i])));
		g_assert_false(json_object_get_boolean(field(who, "canAdd")));
		g_assert_null(
			line_friend_add(&s, who, "id", "alice.1", 42, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_cmpuint(calls, ==, 1);
	}
	struct rpc_step rejected[] = { { "/RE4",
					 "findContactBySearchIdOrTicketV3",
					 FIND_ID, "{\"0:obj\":" VISIBLE "}" },
				       { "/RE4", "addFriendByMid", ADD_ID,
					 "{\"1:obj\":{\"1:i32\":3}}" } };
	use(rejected, 2);
	g_autoptr(J) who = line_friend_lookup(&s, "id", "alice.1", &error);
	g_assert_no_error(error);
	g_assert_null(line_friend_add(&s, who, "id", "alice.1", 42, &error));
	g_assert_nonnull(strstr(error->message, "friend limit"));
	g_assert_cmpuint(calls, ==, 2);
	g_assert_false(json_object_get_boolean(field(who, "friend")));
	g_clear_pointer(&s.pictures, json_object_put);
}

#define PAGE1 "{\"1\":{\"2\":2}}"
#define PAGE2 "{\"1\":{\"1\":\"next-token\",\"2\":2}}"
#define IDS                                                              \
	"{\"0:obj\":{\"1:set:str\":[\"" ALICE "\",\"" ALICE "\",\"" SELF \
	"\",\"" BLOCK "\",\"" HIDE "\"],\"2:str\":\"next-token\"}}"
#define DETAILS                                                \
	"{\"1\":{\"1\":[{\"1\":\"" ALICE "\"},{\"1\":\"" BLOCK \
	"\"},{\"1\":\"" HIDE "\"}],\"2\":5}}"

static void paging(void)
{
	struct rpc_step steps[] = {
		{ "/RE4", "getUserFriendIds", PAGE1, IDS },
		{ "/RE4", "getContactsV3", DETAILS,
		  "{\"0:obj\":{\"1:list:obj\":["
		  "{\"1:str\":\"" OTHER
		  "\",\"3:obj\":{\"3:str\":\"/unrequested\"}},"
		  "{\"1:str\":\"" HIDE
		  "\",\"4:obj\":{\"1:obj\":{\"6:bool\":true}}},"
		  "{\"1:str\":\"" BLOCK "\",\"5:obj\":{\"1:obj\":{}}},"
		  "{\"1:str\":\"" ALICE
		  "\",\"3:obj\":{\"2:str\":\"Alice\",\"3:str\":\"/photo\"},"
		  "\"4:obj\":{\"1:obj\":{\"3:str\":\"Al\",\"4:i64\":123}}}]}}" },
		{ "/RE4", "getUserFriendIds", PAGE2,
		  "{\"0:obj\":{\"1:set:str\":[],\"2:str\":\"\"}}" },
		{ "/RE4", "getUserFriendIds", PAGE2, IDS }
	};
	use(steps, 4);
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) page = line_friends_page(&s, NULL, &error);
	g_assert_no_error(error);
	J *rows = field(page, "friends");

	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	J *alice = json_object_array_get_idx(rows, 0);

	g_assert_cmpstr(strfield(alice, "id"), ==, ALICE);
	g_assert_cmpstr(strfield(alice, "name"), ==, "Al");
	g_assert_true(json_object_get_boolean(field(alice, "favorite")));
	g_assert_null(field(s.pictures, OTHER));
	const char *next = strfield(page, "next");

	g_assert_cmpstr(next, ==, "next-token");
	g_autoptr(J) last = line_friends_page(&s, next, &error);
	g_assert_no_error(error);
	g_assert_null(field(last, "next"));
	g_assert_cmpuint(json_object_array_length(field(last, "friends")), ==,
			 0);
	g_assert_null(line_friends_page(&s, next, &error));
	g_assert_nonnull(error);
	g_assert_cmpuint(calls, ==, 4);
	g_clear_pointer(&s.pictures, json_object_put);
}

static void compatibility(void)
{
	struct rpc_step steps[] = {
		{ "/RE4", "getUserFriendIds", PAGE1, IDS },
		{ "/RE4", "getContactsV3", DETAILS, "unavailable" },
		{ "/S4", "getContactsV2",
		  "{\"1\":{\"1\":[\"" ALICE "\",\"" BLOCK "\",\"" HIDE "\"]}}",
		  "{\"0:obj\":{\"1:map:str:obj\":{"
		  "\"" ALICE
		  "\":{\"3:obj\":{\"27:str\":\"Al\",\"37:str\":\"/photo\"}},"
		  "\"" BLOCK "\":{\"3:obj\":{\"11:i32\":2}},"
		  "\"" HIDE "\":{\"3:obj\":{\"11:i32\":1,\"36:i64\":4}}}}}" },
	};
	use(steps, 3);
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) page = line_friends_page(&s, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(field(page, "friends")), ==,
			 1);
	g_assert_cmpstr(
		strfield(json_object_array_get_idx(field(page, "friends"), 0),
			 "id"),
		==, ALICE);
	g_assert_cmpuint(calls, ==, 3);
	g_clear_pointer(&s.pictures, json_object_put);
}

static void invite(void)
{
	struct rpc_step steps[] = {
		{ "/S4", "getUserTicket", "{}",
		  "{\"0:obj\":{\"1:str\":\"AbC_x-9\"}}" },
		{ "/S4", "getUserTicket", "{}",
		  "{\"0:obj\":{\"1:str\":\"AbC_x-9\",\"10:i64\":1}}" },
		{ "/S4", "getUserTicket", "{}", "{\"0:obj\":{}}" }
	};
	use(steps, 3);
	Line s = { 0 };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) details = line_friend_invite(&s, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(details, "url"), ==,
			"https://line.me/ti/p/AbC_x-9");
	for (int i = 0; i < 2; i++) {
		g_assert_null(line_friend_invite(&s, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	g_assert_cmpuint(calls, ==, 3); /* no reissue */
}

static void edit_friend(void)
{
	Line s = { .mid = SELF, .seq = 46 };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) row = json_tokener_parse(
		"{\"id\":\"" ALICE
		"\",\"name\":\"Alice\",\"profileName\":\"Alice\","
		"\"friend\":true,\"favorite\":false,\"hidden\":false,\"blocked\":false}");
	struct rpc_step steps[] = {
		{ "/RE4", "updateFriendFavorite",
		  "{\"1\":{\"1\":42,\"2\":\"" ALICE "\",\"3\":true}}",
		  "{\"0:obj\":{}}" },
		{ "/RE4", "updateUserFriendOverridenName",
		  "{\"1\":{\"1\":43,\"2\":\"" ALICE "\",\"3\":\"อาลิซ\"}}",
		  "{\"0:obj\":{}}" },
		{ "/RE4", "updateUserFriendOverridenName",
		  "{\"1\":{\"1\":44,\"2\":\"" ALICE "\",\"3\":\"\"}}",
		  "{\"0:obj\":{}}" },
		{ "/RE4", "updateFriendFavorite",
		  "{\"1\":{\"1\":45,\"2\":\"" ALICE "\",\"3\":false}}",
		  "timeout" },
		{ "/RE4", "updateFriendFavorite",
		  "{\"1\":{\"1\":46,\"2\":\"" ALICE "\",\"3\":false}}",
		  "{\"0:obj\":{}}" }
	};
	use(steps, G_N_ELEMENTS(steps));
	s.profiles = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(s.profiles, ALICE, "Alice");
	g_autoptr(J) favorite = line_friend_favorite(&s, row, TRUE, 42, &error);
	g_assert_no_error(error);
	g_assert_false(json_object_get_boolean(field(row, "favorite")));
	g_assert_true(json_object_get_boolean(field(favorite, "favorite")));
	g_autoptr(J) renamed =
		line_friend_rename(&s, favorite, "อาลิซ", 43, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(renamed, "name"), ==, "อาลิซ");
	g_assert_cmpstr(strfield(renamed, "profileName"), ==, "Alice");
	g_assert_true(json_object_get_boolean(field(renamed, "favorite")));
	g_assert_false(g_hash_table_contains(s.profiles, ALICE));
	g_autoptr(J) cleared = line_friend_rename(&s, renamed, "", 44, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(cleared, "name"), ==, "Alice");
	g_assert_cmpstr(strfield(cleared, "overrideName"), ==, "");
	g_assert_null(line_friend_favorite(&s, cleared, FALSE, 45, &error));
	g_assert_error(error, LINE_ERROR, LINE_TIMEOUT);
	g_clear_error(&error);
	g_assert_true(json_object_get_boolean(field(cleared, "favorite")));
	g_autoptr(J) unfavorite =
		line_friend_favorite(&s, cleared, FALSE, 46, &error);
	g_assert_no_error(error);
	g_assert_false(json_object_get_boolean(field(unfavorite, "favorite")));
	g_assert_null(line_friend_rename(&s, row, "a\nb", 47, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_object_add(row, "blocked", json_object_new_boolean(TRUE));
	g_assert_null(line_friend_rename(&s, row, "Al", 47, &error));
	g_assert_nonnull(error);
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
	g_assert_cmpint(s.seq, ==, 46);
	g_clear_pointer(&s.profiles, g_hash_table_unref);
}

static void manage_relationship(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	const char *friend =
		"{\"id\":\"" ALICE
		"\",\"name\":\"Alice\",\"friend\":true,\"favorite\":true}";
	g_autoptr(J) active = json_tokener_parse(friend);
	struct rpc_step steps[] = {
		{ "/RE4", "updateFriendHidden",
		  "{\"1\":{\"1\":5,\"2\":\"" ALICE "\",\"3\":true}}",
		  "{\"0:obj\":{}}" },
		{ "/RE4", "updateFriendHidden",
		  "{\"1\":{\"1\":6,\"2\":\"" ALICE "\",\"3\":false}}",
		  "{\"0:obj\":{}}" },
		{ "/RE4", "blockRelation",
		  "{\"1\":{\"1\":7,\"2\":\"" ALICE "\"}}", "{\"0:obj\":{}}" },
		{ "/RE4", "unblockRelation",
		  "{\"1\":{\"1\":8,\"2\":\"" ALICE "\"}}", "timeout" },
		{ "/RE4", "unblockRelation",
		  "{\"1\":{\"1\":9,\"2\":\"" ALICE "\"}}", "{\"0:obj\":{}}" },
	};
	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(J) hidden =
		line_friend_visibility(&s, active, TRUE, 5, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(hidden, "hidden")));
	g_assert_null(line_friend_visibility(&s, hidden, TRUE, 5, &error));
	g_clear_error(&error);
	g_autoptr(J) visible =
		line_friend_visibility(&s, hidden, FALSE, 6, &error);
	g_assert_no_error(error);
	g_assert_false(json_object_get_boolean(field(visible, "hidden")));
	g_autoptr(J) blocked = line_friend_block(&s, visible, TRUE, 7, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(blocked, "blocked")));
	g_assert_false(json_object_get_boolean(field(visible, "blocked")));
	g_assert_null(line_friend_block(&s, blocked, TRUE, 8, &error));
	g_clear_error(&error);
	g_assert_null(line_friend_visibility(&s, blocked, TRUE, 8, &error));
	g_clear_error(&error);
	g_assert_null(line_friend_block(&s, blocked, FALSE, 8, &error));
	g_assert_error(error, LINE_ERROR, LINE_TIMEOUT);
	g_clear_error(&error);
	g_assert_true(json_object_get_boolean(field(blocked, "blocked")));
	g_autoptr(J) unblocked =
		line_friend_block(&s, blocked, FALSE, 9, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(unblocked, "friend")));
	g_assert_true(json_object_get_boolean(field(unblocked, "favorite")));
	g_assert_false(json_object_get_boolean(field(unblocked, "blocked")));
	json_object_object_add(active, "official",
			       json_object_new_boolean(TRUE));
	g_assert_null(line_friend_block(&s, active, TRUE, 10, &error));
	g_clear_error(&error);
	json_object_object_add(active, "friend",
			       json_object_new_boolean(FALSE));
	g_assert_null(line_friend_visibility(&s, active, TRUE, 10, &error));
	g_clear_error(&error);
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
}

static void managed_directory(void)
{
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	const char *contacts =
		"{\"0:obj\":{\"1:list:obj\":["
		"{\"1:str\":\"" ALICE
		"\",\"4:obj\":{\"1:obj\":{}},\"5:obj\":{\"3:obj\":{}}},"
		"{\"1:str\":\"" BLOCK
		"\",\"4:obj\":{\"1:obj\":{}},\"5:obj\":{\"1:obj\":{}}},"
		"{\"1:str\":\"" HIDE
		"\",\"4:obj\":{\"1:obj\":{\"6:bool\":true}},\"5:obj\":{\"3:obj\":{}}}]}}";
	struct rpc_step steps[] = {
		{ "/RE4", "getUserFriendIds", PAGE1, IDS },
		{ "/RE4", "getContactsV3", DETAILS, contacts },
		{ "/RE4", "getUserFriendIds", "{\"1\":{\"2\":1}}", IDS },
		{ "/RE4", "getContactsV3", DETAILS, contacts },
		{ "/RE4", "getUserFriendIds",
		  "{\"1\":{\"2\":1,\"1\":\"next-token\"}}", "{\"0:obj\":{}}" }
	};
	use(steps, G_N_ELEMENTS(steps));
	g_assert_null(line_managed_friends_page(&s, "other", NULL, &error));
	g_clear_error(&error);
	g_autoptr(J) hidden =
		line_managed_friends_page(&s, "hidden", NULL, &error);
	g_assert_no_error(error);
	J *rows = field(hidden, "friends");

	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(rows, 0), "id"), ==,
			HIDE);
	g_autoptr(J) blocked =
		line_managed_friends_page(&s, "blocked", NULL, &error);
	g_assert_no_error(error);
	rows = field(blocked, "friends");
	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(rows, 0), "id"), ==,
			BLOCK);
	g_autoptr(J) end = line_managed_friends_page(
		&s, "blocked", strfield(blocked, "next"), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(field(end, "friends")), ==,
			 0);
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
	g_clear_pointer(&s.pictures, json_object_put);
}

static void own_profile(void)
{
#define MY_PROFILE                      \
	"{\"0:obj\":{\"1:str\":\"" SELF \
	"\",\"3:str\":\"user.name\",\"20:str\":\"Me\",\"24:str\":\"hello\",\"33:str\":\"/portrait\",\"10:str\":\"private phone\"}}"
	struct rpc_step steps[] = {
		{ "/S4", "getProfile", "{\"1\":8}", MY_PROFILE },
		{ "/S4", "getProfile", "{\"1\":8}", MY_PROFILE },
		{ "/S4", "updateProfileAttributes",
		  "{\"1\":12,\"2\":{\"1\":{\"2\":{\"1\":\"Alex\",\"2\":{}},\"16\":{\"1\":\"\",\"2\":{}}}}}",
		  "{}" },
		{ "/S4", "getProfile", "{\"1\":8}", MY_PROFILE },
		{ "/S4", "getProfile", "{\"1\":8}",
		  "{\"0:obj\":{\"1:str\":\"" OTHER "\",\"20:str\":\"Other\"}}" }
	};
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(J) row = line_own_profile(&s, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(row, "lineId"), ==, "user.name");
	g_assert_null(strstr(json_object_get_string(row), "private phone"));
	g_assert_cmpstr(strfield(s.pictures, SELF), ==, "/portrait");
	g_autoptr(J) changes =
		json_tokener_parse("{\"name\":\"Alex\",\"status\":\"\"}");
	g_autoptr(J) edited = line_own_profile_update(&s, changes, 12, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(edited, "name"), ==, "Alex");
	g_assert_cmpstr(strfield(edited, "status"), ==, "");
	g_autoptr(J) same = json_tokener_parse("{\"name\":\"Me\"}");
	g_autoptr(J) unchanged = line_own_profile_update(&s, same, 13, &error);
	g_assert_nonnull(unchanged);
	g_assert_null(line_own_profile_update(&s, changes, 14, &error));
	g_clear_error(&error);
	const char *const invalid[] = { "{}",
					"{\"status\":null}",
					"{\"name\":\"\"}",
					"{\"name\":\"12345678901234567890😊\"}",
					"{\"name\":\"abc\\u0000def\"}",
					"{\"phone\":\"123\"}" };
	for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_autoptr(J) bad = json_tokener_parse(invalid[i]);
		g_assert_null(line_own_profile_update(&s, bad, 15, &error));
		g_clear_error(&error);
	}
	/* The Android display-name editor counts Unicode code points. */
	struct rpc_step unicode[] = {
		{ "/S4", "getProfile", "{\"1\":8}", MY_PROFILE },
		{ "/S4", "updateProfileAttributes",
		  "{\"1\":17,\"2\":{\"1\":{\"2\":{\"1\":\"1234567890123456789😊\",\"2\":{}}}}}",
		  "{}" }
	};
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
	use(unicode, G_N_ELEMENTS(unicode));
	g_autoptr(J) boundary =
		json_tokener_parse("{\"name\":\"1234567890123456789😊\"}");
	g_autoptr(J) updated =
		line_own_profile_update(&s, boundary, 17, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(updated, "name"), ==,
			strfield(boundary, "name"));
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(unicode));
	g_clear_pointer(&s.pictures, json_object_put);
}

static void recommendations(void)
{
#define REC_PERSON            \
	"{\"1:str\":\"" ALICE \
	"\",\"2:i32\":1,\"3:obj\":{\"2:str\":\"Alice\"},\"4:obj\":{\"3:obj\":{}},\"5:obj\":{\"3:obj\":{}},\"6:obj\":{\"1:obj\":{}}}"
#define REC_DETAILS "{\"1\":{\"1\":[{\"1\":\"" ALICE "\"}],\"2\":5}}"
#define REC_REPLY "{\"0:obj\":{\"1:list:obj\":[" REC_PERSON "]}}"
	struct rpc_step steps[] = {
		{ "/RE4", "getUserRecommendIds", "{\"1\":{\"2\":2}}",
		  "{\"0:obj\":{\"1:set:str\":[\"" ALICE "\",\"" SELF
		  "\"],\"2:str\":\"next-page\"}}" },
		{ "/RE4", "getContactsV3", REC_DETAILS, REC_REPLY },
		{ "/RE4", "getUserRecommendIds",
		  "{\"1\":{\"1\":\"next-page\",\"2\":2}}", "{\"0:obj\":{}}" },
		{ "/RE4", "getContactsV3", REC_DETAILS,
		  "{\"0:obj\":{\"1:list:obj\":[]}}" },
		{ "/RE4", "getContactsV3", REC_DETAILS, REC_REPLY },
		{ "/RE4", "addFriendByMid",
		  ADD_CORE
		  "{\"1\":\"{\\\"screen\\\":\\\"friendAdd:recommend\\\",\\\"spec\\\":\\\"native\\\"}\",\"2\":{\"10\":{}}}}}",
		  "{\"0:obj\":{}}" }
	};
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(J) p =
		line_managed_friends_page(&s, "recommendations", NULL, &error);
	g_assert_no_error(error);
	J *rows = field(p, "friends");

	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	J *r = json_object_array_get_idx(rows, 0);

	g_assert_true(json_object_get_boolean(field(r, "recommended")));
	g_assert_false(json_object_get_boolean(field(r, "friend")));
	g_autoptr(J) end = line_managed_friends_page(
		&s, "recommendations", strfield(p, "next"), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(field(end, "friends")), ==,
			 0);
	g_assert_null(line_add_recommendation(&s, ALICE, 41, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_autoptr(J) added = line_add_recommendation(&s, ALICE, 42, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(added, "friend")));
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
}

static void profile_card(void)
{
#define CARD_PERSON                                                      \
	"{\"1:str\":\"" OTHER                                            \
	"\",\"2:i32\":1,\"3:obj\":{\"2:str\":\"Dana\",\"3:str\":\"/d\"," \
	"\"4:obj\":{\"1:str\":\"Out walking\\nback soon\"}}}"
#define CARD_ASK "{\"1\":{\"1\":[{\"1\":\"" OTHER "\"}],\"2\":8}}"
	struct rpc_step steps[] = {
		{ "/RE4", "getContactsV3", CARD_ASK,
		  "{\"0:obj\":{\"1:list:obj\":[" CARD_PERSON "]}}" },
		{ "/RE4", "getContactsV3", CARD_ASK, "unavailable" },
		{ "/S4", "getContactsV2", "{\"1\":{\"1\":[\"" OTHER "\"]}}",
		  "{\"0:obj\":{\"1:map:str:obj\":{\"" OTHER
		  "\":{\"3:obj\":{\"1:str\":\"" OTHER
		  "\",\"11:i32\":1,\"22:str\":\"Dana\",\"26:str\":\"Hi\"}}}}}" },
		{ "/RE4", "getContactsV3", CARD_ASK,
		  "{\"0:obj\":{\"1:list:obj\":[]}}" },
		{ "/RE4", "addFriendByMid",
		  "{\"1\":{\"1\":7,\"2\":\"" OTHER
		  "\",\"3\":{\"2\":{\"5\":{}}}}}",
		  "{\"0:obj\":{}}" },
		{ "/RE4", "addFriendByMid",
		  "{\"1\":{\"1\":8,\"2\":\"" OTHER
		  "\",\"3\":{\"2\":{\"13\":{}}}}}",
		  "{\"0:obj\":{}}" }
	};
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(J) card = line_friend_card(&s, OTHER, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(card, "name"), ==, "Dana");
	g_assert_cmpstr(strfield(card, "status"), ==, "Out walking\nback soon");
	g_assert_false(json_object_get_boolean(field(card, "friend")));
	g_assert_true(json_object_get_boolean(field(card, "canAdd")));
	g_assert_cmpstr(strfield(s.pictures, OTHER), ==, "/d");
	g_autoptr(J) legacy = line_friend_card(&s, OTHER, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(legacy, "status"), ==, "Hi");
	g_assert_true(json_object_get_boolean(field(legacy, "friend")));
	g_assert_null(line_friend_card(&s, OTHER, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_null(line_friend_card(&s, "not-a-mid", &error));
	g_clear_error(&error);
	/* Only sources LINE defines, and never a friend or self. */
	g_assert_null(
		line_friend_add_from(&s, legacy, LINE_FROM_GROUP, 6, &error));
	g_clear_error(&error);
	g_assert_null(line_friend_add_from(&s, card, 99, 6, &error));
	g_clear_error(&error);
	g_autoptr(J) group =
		line_friend_add_from(&s, card, LINE_FROM_GROUP, 7, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(group, "friend")));
	g_assert_cmpstr(strfield(group, "status"), ==,
			"Out walking\nback soon");
	g_autoptr(J) stranger =
		line_friend_add_from(&s, card, LINE_FROM_STRANGER, 8, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
	g_clear_pointer(&s.pictures, json_object_put);
}

static void friend_requests(void)
{
#define REQUEST(mid, dir)                                               \
	"{\"1:str\":\"e-" mid "\",\"2:str\":\"" mid "\",\"3:i32\":" dir \
	",\"6:i64\":1700000000000,\"7:i64\":3,\"10:str\":\"Req\",\"11:str\":\"/r\"}"
	struct rpc_step steps[] = {
		{ "/S4", "getFriendRequests", "{\"1\":1,\"2\":0}",
		  "{\"0:list:obj\":[" REQUEST(ALICE, "1") "," REQUEST(
			  OTHER, "2") "," REQUEST(SELF,
						  "1") ",{\"1:str\":\"x\"}]}" },
		{ "/RE4", "addFriendByMid",
		  "{\"1\":{\"1\":9,\"2\":\"" ALICE
		  "\",\"3\":{\"2\":{\"21\":{}}}}}",
		  "{\"0:obj\":{}}" },
		{ "/S4", "removeFriendRequest",
		  "{\"1\":1,\"2\":\"e-" ALICE "\"}", "{}" }
	};
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;
	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(J) rows = line_friend_requests(&s, &error);
	g_assert_no_error(error);
	/* Outgoing, own and MID-less requests are not offered. */
	g_assert_cmpuint(json_object_array_length(rows), ==, 1);
	J *row = json_object_array_get_idx(rows, 0);

	g_assert_cmpstr(strfield(row, "id"), ==, ALICE);
	g_assert_cmpstr(strfield(row, "name"), ==, "Req");
	g_assert_cmpstr(strfield(row, "eMid"), ==, "e-" ALICE);
	g_assert_cmpint(json_object_get_int64(field(row, "time")), ==,
			1700000000000);
	g_assert_true(json_object_get_boolean(field(row, "canAdd")));
	g_autoptr(J) friend =
		line_friend_add_from(&s, row, LINE_FROM_REQUEST, 9, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(friend, "friend")));
	g_autoptr(J) gone = line_friend_request_decline(&s, row, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(gone, "id"), ==, ALICE);
	g_assert_null(line_friend_request_decline(&s, friend, &error));
	g_clear_error(&error);
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
	g_clear_pointer(&s.pictures, json_object_put);
}

static void delete_and_settings(void)
{
	struct rpc_step steps[] = {
		{ "/RE4", "deleteFriend",
		  "{\"1\":{\"1\":12,\"2\":\"" ALICE "\"}}", "{\"0:obj\":{}}" },
		{ "/RE4", "deleteFromBlockList",
		  "{\"1\":{\"1\":13,\"2\":\"" BLOCK "\"}}", "{\"0:obj\":{}}" },
		{ "/S4", "getSettings", "{\"2\":8}",
		  "{\"0:obj\":{\"10:bool\":true,\"14:bool\":false,"
		  "\"21:bool\":false,\"22:bool\":true,\"24:bool\":true,"
		  "\"26:bool\":false,\"61:bool\":true,\"68:bool\":true,"
		  "\"95:bool\":false}}" },
		{ "/S4", "updateSettingsAttributes2",
		  "{\"1\":14,\"3\":{\"22\":false},\"4\":[22]}",
		  "{\"0:i32\":1}" },
	};
	Line s = { .mid = SELF };

	g_autoptr(GError) error = NULL;

	use(steps, G_N_ELEMENTS(steps));

	g_autoptr(J) reg =
		json_tokener_parse("{\"id\":\"" ALICE "\",\"friend\":true}");
	g_autoptr(J) d1 = line_friend_delete(&s, reg, 12, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(d1, "deleted")));

	g_autoptr(J) blk = json_tokener_parse(
		"{\"id\":\"" BLOCK "\",\"friend\":true,\"blocked\":true}");
	g_autoptr(J) d2 = line_friend_delete(&s, blk, 13, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(d2, "deleted")));

	/* Official accounts cannot be deleted through deleteFriend. */
	g_autoptr(J) off = json_tokener_parse(
		"{\"id\":\"" OTHER "\",\"friend\":true,\"official\":true}");
	g_assert_null(line_friend_delete(&s, off, 14, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);

	g_autoptr(J) cfg = line_account_settings(&s, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(cfg, "notifyEnable")));
	g_assert_false(
		json_object_get_boolean(field(cfg, "notifyShowPreview")));
	g_assert_true(json_object_get_boolean(field(cfg, "searchById")));
	g_assert_false(json_object_get_boolean(field(cfg, "searchByPhone")));
	g_assert_true(json_object_get_boolean(field(cfg, "e2eeEnable")));

	g_autoptr(J) upd = line_account_settings_update(&s, "searchById", FALSE,
							14, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(upd, "key"), ==, "searchById");
	g_assert_false(json_object_get_boolean(field(upd, "value")));

	/* Unknown setting keys are rejected locally. */
	g_assert_null(
		line_account_settings_update(&s, "bogusKey", TRUE, 15, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_cmpuint(calls, ==, G_N_ELEMENTS(steps));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/friends/exact-lookup-explicit-add", find_add);
	g_test_add_func("/friends/input-and-service-limits",
			local_and_remote_rejections);
	g_test_add_func("/friends/relationship-and-rejection",
			protected_relationships);
	g_test_add_func("/friends/directory-pagination", paging);
	g_test_add_func("/friends/legacy-profile-list", compatibility);
	g_test_add_func("/friends/existing-personal-invite", invite);
	g_test_add_func("/friends/favorite-and-personal-name", edit_friend);
	g_test_add_func("/friends/hidden-and-blocked-rosters",
			managed_directory);
	g_test_add_func("/friends/hide-block-restore", manage_relationship);
	g_test_add_func("/friends/own-profile", own_profile);
	g_test_add_func("/friends/recommendations", recommendations);
	g_test_add_func("/friends/profile-card", profile_card);
	g_test_add_func("/friends/friend-requests", friend_requests);
	g_test_add_func("/friends/delete-and-account-settings",
			delete_and_settings);
	g_test_add_func("/friends/pacing", pacing);
	return g_test_run();
}
