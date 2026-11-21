// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_friends.h"
#include <string.h>

/* Exercise the native form through its public input and worker boundaries. */
Interface ui;
static char *method, *opened;
static Json *params;
static guint tag, calls;

Json *ui_field(Json *obj, const char *key)
{
	Json *v = NULL;

	if (obj)
		json_object_object_get_ex(obj, key, &v);
	return v;
}

const char *ui_string(Json *o, const char *key, const char *fallback)
{
	Json *v = ui_field(o, key);

	return json_object_is_type(v, json_type_string) ?
		       json_object_get_string(v) :
		       fallback;
}

void ui_rpc(const char *name, Json *args, guint serial)
{
	g_free(method);
	method = g_strdup(name);
	json_object_put(params);
	params = args;
	tag = serial;
	calls++;
}

void ui_restore_input(void)
{
}

void ui_update_friend_chat(Json *row)
{
	(void)row;
}

void ui_open_conversation(Json *row)
{
	g_free(opened);
	opened = g_strdup(ui_string(row, "id", ""));
}

void ui_friend_pick_qr(void)
{
}

void ui_friend_scan_file(const char *path)
{
	(void)path;
}

void ui_insert(Editor *e, const char *s)
{
	int begin = MIN(e->cursor, e->anchor), end = MAX(e->cursor, e->anchor);

	g_string_erase(e->value, begin, end - begin);
	g_string_insert(e->value, begin, s);
	e->cursor = e->anchor = begin + strlen(s);
}

void ui_edit_key(Editor *e, SDL_KeyboardEvent key)
{
	if (key.keysym.sym == SDLK_BACKSPACE && e->cursor) {
		e->anchor = e->cursor - 1;
		ui_insert(e, "");
	}
}

void ui_draw_profile(Json *row, Rect r)
{
	(void)row;
	(void)r;
}

void ui_draw_code(QRcode *qr, int c, int t, int b)
{
	(void)qr;
	(void)c;
	(void)t;
	(void)b;
}

void ui_draw_edit(Editor *e, Rect r, gboolean f, const char *p)
{
	(void)e;
	(void)r;
	(void)f;
	(void)p;
}

int ui_edit_hit(Editor *e, Rect r, int x)
{
	(void)r;
	(void)x;
	return e->value->len;
}

void ui_draw_box(Rect r, guint c)
{
	(void)r;
	(void)c;
}

void ui_draw_label(const char *s, Rect r, int z, int w, guint c, gboolean wrap)
{
	(void)s;
	(void)r;
	(void)z;
	(void)w;
	(void)c;
	(void)wrap;
}

void ui_draw_button(Rect r, const char *s, gboolean p)
{
	(void)r;
	(void)s;
	(void)p;
}

/* The Friends pane fills the window in these tests. */
int ui_content_x(void)
{
	return 0;
}

void ui_draw_rounded(Rect r, int radius, guint c)
{
	(void)r;
	(void)radius;
	(void)c;
}

void ui_draw_icon(Rect r, enum ui_icon icon, guint c)
{
	(void)r;
	(void)icon;
	(void)c;
}

void ui_draw_focus(Rect r)
{
	(void)r;
}

void ui_draw_centered(const char *s, Rect r, int z, int w, guint c)
{
	(void)s;
	(void)r;
	(void)z;
	(void)w;
	(void)c;
}

static void reply(const char *expected, const char *body)
{
	g_assert_cmpstr(method, ==, expected);
	Json *result = json_tokener_parse(body);

	g_assert_nonnull(result);
	g_assert_true(ui_friends_reply(expected, tag, result, NULL));
	json_object_put(result);
	ui_friends_draw();
}

static void click(int x, int y, int button)
{
	SDL_Event e = { .button = { .type = SDL_MOUSEBUTTONDOWN,
				    .button = button,
				    .x = x,
				    .y = y } };
	g_assert_true(ui_friends_event(&e));
	ui_friends_draw();
}

static void key(SDL_Keycode k)
{
	SDL_Event e = { .key = { .type = SDL_KEYDOWN,
				 .repeat = 0,
				 .keysym = { .sym = k } } };
	g_assert_true(ui_friends_event(&e));
	ui_friends_draw();
}

static void type(const char *s)
{
	SDL_Event e = { .text = { .type = SDL_TEXTINPUT } };

	g_strlcpy(e.text.text, s, sizeof(e.text.text));
	g_assert_true(ui_friends_event(&e));
	ui_friends_draw();
}

static void setup(void)
{
	ui_friends_reset();
	ui.width = 402;
	ui.height = 700;
	ui.connected = ui.friends = ui.friend_invite = TRUE;
	calls = 0;
	ui_friends_show();
	reply("friends",
	      "{\"friends\":[{\"name\":\"Mina\",\"id\":\"u1\"}],\"next\":\"page2\"}");
}

/* Header tabs at 402 px: Back, All…Suggested, then Profile, QR and Add wrap. */
enum {
	TAB_ROW1 = 20,
	TAB_ROW2 = 52,
	TAB_HIDDEN = 140,
	TAB_BLOCKED = 210,
	TAB_SUGGESTED = 290,
	TAB_PROFILE = 36,
	TAB_QR = 90,
	TAB_ADD = 140
};

static void lookup_add(void)
{
	setup();
	click(TAB_ADD, TAB_ROW2, SDL_BUTTON_LEFT);
	type("+66812345678");
	key(SDLK_RETURN);
	g_assert_cmpstr(ui_string(params, "query", ""), ==, "+66812345678");
	guint stale = tag;

	type("9");
	reply("findFriend",
	      "{\"id\":\"u2\",\"name\":\"Wrong\",\"lookup\":\"old\"}");
	key(SDLK_RETURN);
	g_assert_cmpuint(tag, !=, stale);
	reply("findFriend",
	      "{\"id\":\"u3\",\"name\":\"Ari\",\"lookup\":\"opaque-new\",\"canAdd\":true}");
	guint before = calls;

	key(SDLK_RETURN);
	g_assert_cmpuint(calls, ==, before);
	click(150, 335, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "addFriend");
	g_assert_cmpstr(ui_string(params, "lookup", ""), ==, "opaque-new");
	click(150, 335, SDL_BUTTON_LEFT);
	g_assert_cmpuint(calls, ==, before + 1);
	reply("addFriend", "{\"id\":\"u3\",\"name\":\"Ari\",\"friend\":true}");
	click(150, 335, SDL_BUTTON_LEFT);
	reply("openFriend", "{\"id\":\"u3\",\"name\":\"Ari\"}");
	g_assert_cmpstr(opened, ==, "u3");
	g_assert_false(ui_friends_active());
}

static void roster_profile(void)
{
	setup();
	click(50, 665, SDL_BUTTON_LEFT);
	g_assert_cmpstr(ui_string(params, "cursor", ""), ==, "page2");
	reply("friends",
	      "{\"friends\":[{\"id\":\"u1\",\"name\":\"Mina\"},{\"id\":\"u2\",\"name\":\"Niran\"}]}");
	click(67, 265, SDL_BUTTON_RIGHT);
	click(165, 184, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "setFriendFavorite");
	g_assert_cmpstr(ui_string(params, "id", ""), ==, "u2");
	g_assert_true(json_object_get_boolean(ui_field(params, "favorite")));
	reply("setFriendFavorite",
	      "{\"id\":\"u2\",\"name\":\"Niran\",\"favorite\":true}");
	click(60, 275, SDL_BUTTON_LEFT);
	type("N");
	click(64, 321, SDL_BUTTON_LEFT);
	g_assert_cmpstr(ui_string(params, "name", ""), ==, "N");
	reply("renameFriend",
	      "{\"id\":\"u2\",\"name\":\"N\",\"overrideName\":\"N\",\"profileName\":\"Niran\"}");
	click(60, 275, SDL_BUTTON_LEFT);
	key(SDLK_BACKSPACE);
	click(64, 321, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "renameFriend");
	g_assert_cmpstr(ui_string(params, "name", "missing"), ==, "");
	ui_friends_reset();
	Json *r = json_tokener_parse("{\"id\":\"u2\"}");

	g_assert_true(ui_friends_reply("renameFriend", tag, r, NULL));
	json_object_put(r);
	g_assert_false(ui_friends_active());
}

static void qr_lookup(void)
{
	setup();
	click(TAB_ADD, TAB_ROW2, SDL_BUTTON_LEFT);
	guint before = calls;

	ui_friends_scan_result("https://example.com/unrelated", NULL);
	g_assert_cmpuint(calls, ==, before);
	ui_friends_scan_result("https://line.me/ti/p/test", NULL);
	g_assert_cmpstr(method, ==, "findFriend");
	g_assert_cmpstr(ui_string(params, "query", ""), ==,
			"https://line.me/ti/p/test");
	key(SDLK_ESCAPE);
	click(TAB_QR, TAB_ROW2, SDL_BUTTON_LEFT);
	reply("friendInvite", "{\"url\":\"https://line.me/ti/p/test\"}");
}

static void managed_profiles(void)
{
	setup();
	click(67, 215, SDL_BUTTON_RIGHT);
	guint before = calls;

	click(170, 378, SDL_BUTTON_LEFT);
	g_assert_cmpuint(calls, ==, before);
	key(SDLK_RETURN); /* default confirmation action is Cancel */
	g_assert_cmpuint(calls, ==, before);
	click(170, 378, SDL_BUTTON_LEFT);
	click(170, 305, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "setFriendBlocked");
	g_assert_cmpstr(ui_string(params, "id", ""), ==, "u1");
	g_assert_true(json_object_get_boolean(ui_field(params, "blocked")));
	click(170, 305, SDL_BUTTON_LEFT);
	g_assert_cmpuint(calls, ==, before + 1);
	reply("setFriendBlocked",
	      "{\"id\":\"u1\",\"name\":\"Mina\",\"friend\":true,\"blocked\":true}");
	reply("friends", "{\"friends\":[]}");
	click(TAB_BLOCKED, TAB_ROW1, SDL_BUTTON_LEFT);
	g_assert_cmpstr(ui_string(params, "view", ""), ==, "blocked");
	reply("friends",
	      "{\"friends\":[{\"id\":\"u1\",\"friend\":true,\"name\":\"Mina\",\"blocked\":true}]}");
	click(60, 165,
	      SDL_BUTTON_LEFT); /* blocked rows open their management profile */
	click(65, 185, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "setFriendBlocked");
	g_assert_false(json_object_get_boolean(ui_field(params, "blocked")));
	reply("setFriendBlocked",
	      "{\"id\":\"u1\",\"name\":\"Mina\",\"friend\":true,\"blocked\":false}");
	g_assert_cmpstr(ui_string(params, "view", ""), ==, "blocked");
	reply("friends", "{\"friends\":[]}");
	click(TAB_HIDDEN, TAB_ROW1, SDL_BUTTON_LEFT);
	g_assert_cmpstr(ui_string(params, "view", ""), ==, "hidden");
	reply("friends",
	      "{\"friends\":[{\"id\":\"u2\",\"name\":\"Kai\",\"friend\":true,\"hidden\":true}]}");
	click(67, 165, SDL_BUTTON_LEFT);
	click(60, 185, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "setFriendHidden");
	g_assert_false(json_object_get_boolean(ui_field(params, "hidden")));
	reply("setFriendHidden",
	      "{\"id\":\"u2\",\"name\":\"Kai\",\"friend\":true,\"hidden\":false}");
	reply("friends", "{\"friends\":[]}");
	key(SDLK_ESCAPE); /* other directories return to the ordinary roster */
	g_assert_null(ui_field(params, "view"));
	reply("friends",
	      "{\"friends\":[{\"id\":\"u2\",\"name\":\"Kai\",\"friend\":true}]}");
	click(65, 215, SDL_BUTTON_RIGHT);
	click(65, 378, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "setFriendHidden");
	g_assert_true(json_object_get_boolean(ui_field(params, "hidden")));
}

static void manage_self_and_recommendations(void)
{
	setup();
	ui.self_id = "self";
	click(TAB_PROFILE, TAB_ROW2, SDL_BUTTON_LEFT);
	reply("myProfile",
	      "{\"id\":\"self\",\"name\":\"Me\",\"status\":\"hi\"}");
	click(65, 188, SDL_BUTTON_LEFT);
	type("2");
	guint before = calls;

	key(SDLK_ESCAPE);
	g_assert_cmpuint(calls, ==, before);
	click(65, 188, SDL_BUTTON_LEFT);
	type("2");
	click(65, 181, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "updateMyProfile");
	g_assert_cmpstr(ui_string(params, "name", NULL), ==, "Me2");
	g_assert_null(ui_field(params, "status"));
	reply("updateMyProfile",
	      "{\"id\":\"self\",\"name\":\"Me2\",\"status\":\"hi\"}");
	g_assert_cmpstr(ui.name, ==, "Me2");
	click(65, 366, SDL_BUTTON_LEFT);
	key(SDLK_BACKSPACE);
	key(SDLK_BACKSPACE);
	key(SDLK_RETURN);
	g_assert_cmpstr(ui_string(params, "status", NULL), ==, "");
	g_assert_null(ui_field(params, "name"));
	reply("updateMyProfile",
	      "{\"id\":\"self\",\"name\":\"Me2\",\"status\":\"\"}");
	click(TAB_SUGGESTED, TAB_ROW1, SDL_BUTTON_LEFT);
	g_assert_cmpstr(ui_string(params, "view", ""), ==, "recommendations");
	reply("friends",
	      "{\"friends\":[{\"id\":\"r\",\"name\":\"Ari\",\"recommended\":true,\"canAdd\":true}]}");
	before = calls;
	click(65, 180, SDL_BUTTON_LEFT); /* the first suggestion row */
	g_assert_cmpuint(calls, ==, before);
	click(65, 187, SDL_BUTTON_LEFT);
	g_assert_cmpstr(method, ==, "addRecommendation");
	g_assert_cmpstr(ui_string(params, "id", ""), ==, "r");
	click(65, 187, SDL_BUTTON_LEFT);
	g_assert_cmpuint(calls, ==, before + 1);
	reply("addRecommendation",
	      "{\"id\":\"r\",\"name\":\"Ari\",\"friend\":true}");
	g_assert_cmpstr(method, ==, "friends");
	g_assert_cmpstr(ui_string(params, "view", ""), ==, "recommendations");
	reply("friends", "{\"friends\":[]}");
	ui.self_id = NULL;
	g_clear_pointer(&ui.name, g_free);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ui-friends/lookup-confirm-add", lookup_add);
	g_test_add_func("/ui-friends/page-profile-favorite-rename",
			roster_profile);
	g_test_add_func("/ui-friends/personal-qr", qr_lookup);
	g_test_add_func("/ui-friends/manage-hide-block-confirm-restore",
			managed_profiles);
	g_test_add_func("/ui-friends/own-profile-and-recommendations",
			manage_self_and_recommendations);
	int rc = g_test_run();

	ui_friends_reset();
	g_free(ui.composition);
	g_free(method);
	g_free(opened);
	json_object_put(params);
	return rc;
}
