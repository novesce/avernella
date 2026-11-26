// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_friends.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include "ui_motion.h"
#include <string.h>

enum {
	INK = UI_HEADING,
	MUTED = UI_MUTED,
	BASE = UI_BG,
	PANEL = UI_PANEL,
	DIVIDER = UI_LINE,
	HEADING = UI_HEADING,
	SUBTLE = UI_SUBTLE,
	TAB_INK = UI_MUTED,
	TAB_HOVER = UI_HOVER,
	TAB_SELECTED = UI_SELECTED,
	/* Discord friend rows: 62 px with a divider between them. */
	ROW_H = 62,
	N_TABS = 8
};

enum friends_page {
	OFF,
	ROSTER,
	FIND,
	INVITE,
	DETAILS,
	MANAGE,
	PROFILE,
	EDIT_NAME,
	EDIT_STATUS
};

enum friends_directory {
	EVERYONE,
	HIDDEN,
	BLOCKED,
	RECOMMENDED
};

enum friend_command {
	LIST,
	LOOKUP,
	ADD,
	OPEN_CHAT,
	TICKET,
	FAVORITE,
	RENAME,
	VISIBILITY,
	BLOCKING,
	GET_PROFILE,
	EDIT_PROFILE,
	ADD_RECOMMENDED,
	DELETE_FRIEND,
	KEEP_MEMO,
	N_REQUESTS
};

enum friends_control {
	FIELD,
	ROWS,
	CHATS,
	ADD_PAGE,
	QR_PAGE,
	MORE,
	SUBMIT,
	SCAN,
	SAVE_NAME,
	STAR,
	MANAGE_PAGE,
	HIDDEN_PAGE,
	BLOCKED_PAGE,
	HIDE_PERSON,
	BLOCK_PERSON,
	DELETE_PERSON,
	CONFIRM,
	CANCEL,
	PROFILE_PAGE,
	RECOMMEND_PAGE,
	EDIT_MY_NAME,
	EDIT_MY_STATUS,
	ALL_PAGE,
	KEEP_MEMO_BTN
};

struct friend_request {
	guint tag, view;
	char *subject, *sent_name;
};

static const char *const methods[] = { "friends",	   "findFriend",
				       "addFriend",	   "openFriend",
				       "friendInvite",	   "setFriendFavorite",
				       "renameFriend",	   "setFriendHidden",
				       "setFriendBlocked", "myProfile",
				       "updateMyProfile",  "addRecommendation",
				       "deleteFriend",	   "openKeepMemo" };

static struct {
	enum friends_page page;
	enum friends_directory directory;
	gboolean confirming, confirming_delete;
	enum friends_control focus;
	guint serial, view;
	struct friend_request pending[N_REQUESTS];
	Json *rows, *person, *profile;
	GHashTable *ids;
	GArray *filtered;
	Editor search, query, alias, own;
	char *next, *note, *url, *open_target;
	QRcode *qr;
	int first, selected, hover, hover_tab, top;
	gboolean loaded, drag;
	/* Header tabs, as in Discord's Friends view. */
	Rect tab[N_TABS];
	enum friends_control tab_id[N_TABS];
	int tabs;
	Rect bounds, back, add, invite, entry, list, more, submit, scan, card,
		fav, save, manage, hidden, blocked, remove_btn, confirm, cancel,
		profile_button, recommendations;
} f;

static void replace(char **at, const char *value)
{
	char *copy = g_strdup(value);

	g_free(*at);
	*at = copy;
	ui.dirty = TRUE;
}

static void drop(Json **v)
{
	json_object_put(*v);
	*v = NULL;
}

static void input(Editor *e, const char *s)
{
	if (!e->value)
		e->value = g_string_new("");
	g_string_assign(e->value, s ? s : "");
	e->cursor = e->anchor = e->value->len;
	e->scroll = 0;
}

static void release_input(Editor *e)
{
	if (e->value)
		g_string_free(e->value, TRUE);
	*e = (Editor){ 0 };
}

static Editor *editor(void)
{
	return f.page == EDIT_NAME || f.page == EDIT_STATUS ? &f.own :
	       f.page == FIND				    ? &f.query :
	       f.page == ROSTER				    ? &f.search :
	       f.page == DETAILS			    ? &f.alias :
							      NULL;
}

static void clear_card(void)
{
	drop(&f.person);
	replace(&f.note, NULL);
}

static gboolean hit(Rect r, int x, int y)
{
	return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w &&
	       y < r.y + r.h;
}

static void update_filter(void)
{
	if (!f.filtered)
		f.filtered = g_array_new(FALSE, FALSE, sizeof(guint));
	g_array_set_size(f.filtered, 0);
	for (guint i = 0; f.rows && i < json_object_array_length(f.rows); i++) {
		Json *r = json_object_array_get_idx(f.rows, i);
		const char *name = ui_string(r, "name", "");

		if (!f.search.value->len ||
		    g_str_match_string(f.search.value->str, name, TRUE))
			g_array_append_val(f.filtered, i);
	}
	f.first = 0;
	f.selected = 0;
	f.hover = f.hover_tab = -1;
	ui.dirty = TRUE;
}

static Json *person_at(guint i)
{
	return f.filtered && i < f.filtered->len ?
		       json_object_array_get_idx(
			       f.rows, g_array_index(f.filtered, guint, i)) :
		       NULL;
}

static void clear_pending(struct friend_request *p)
{
	g_free(p->subject);
	g_free(p->sent_name);
	*p = (struct friend_request){ 0 };
}

void ui_friends_reset(void)
{
	for (int i = 0; i < N_REQUESTS; i++)
		clear_pending(&f.pending[i]);
	drop(&f.rows);
	drop(&f.person);
	drop(&f.profile);
	g_clear_pointer(&f.filtered, g_array_unref);
	g_clear_pointer(&f.ids, g_hash_table_unref);
	release_input(&f.search);
	release_input(&f.query);
	release_input(&f.alias);
	release_input(&f.own);
	g_free(f.next);
	g_free(f.note);
	g_free(f.url);
	g_free(f.open_target);
	g_clear_pointer(&f.qr, QRcode_free);
	guint serial = f.serial;

	memset(&f, 0, sizeof(f));
	f.serial = serial;
	f.hover = f.hover_tab = -1;
}

gboolean ui_friends_active(void)
{
	return f.page != OFF;
}

void ui_friends_resume_input(void)
{
	if (f.page != OFF && f.focus == FIELD && editor())
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

static void focus(enum friends_control c)
{
	f.focus = c;
	replace(&ui.composition, "");
	ui_friends_resume_input();
}

static gboolean rpc(enum friend_command cmd, const char *key, const char *value)
{
	struct friend_request *p = &f.pending[cmd];

	if (cmd == OPEN_CHAT)
		replace(&f.open_target, value);
	if (p->tag)
		return FALSE;
	f.serial = (f.serial + 1) & 0x7fffffffU;
	if (!f.serial)
		f.serial = 1;
	*p = (struct friend_request){
		.tag = f.serial,
		.view = f.view,
		.subject = g_strdup(value),
		.sent_name = cmd == RENAME ? g_strdup(f.alias.value->str) : NULL
	};
	Json *args = json_object_new_object();

	if (key)
		json_object_object_add(args, key,
				       json_object_new_string(value));
	if (cmd == LIST && f.directory != EVERYONE)
		json_object_object_add(
			args, "view",
			json_object_new_string(
				f.directory == HIDDEN	   ? "hidden" :
				f.directory == RECOMMENDED ? "recommendations" :
							     "blocked"));
	if (cmd == VISIBILITY || cmd == BLOCKING) {
		const char *which = cmd == VISIBILITY ? "hidden" : "blocked";

		json_object_object_add(
			args, which,
			json_object_new_boolean(!json_object_get_boolean(
				ui_field(f.person, which))));
	}
	if (cmd == FAVORITE)
		json_object_object_add(
			args, "favorite",
			json_object_new_boolean(!json_object_get_boolean(
				ui_field(f.person, "favorite"))));
	if (cmd == RENAME)
		json_object_object_add(
			args, "name",
			json_object_new_string(f.alias.value->str));
	ui_rpc(methods[cmd], args, p->tag);
	ui.dirty = TRUE;
	return TRUE;
}

static void page(enum friends_page page)
{
	f.view++;
	f.page = page;
	f.confirming = FALSE;
	f.confirming_delete = FALSE;
	g_clear_pointer(&f.open_target, g_free);
	f.drag = FALSE;
	clear_card();
	g_clear_pointer(&f.qr, QRcode_free);
	g_clear_pointer(&f.url, g_free);
	if (page == OFF) {
		ui_restore_input();
		return;
	}
	if (page == ROSTER) {
		if (!f.search.value)
			input(&f.search, "");
		if (!f.filtered)
			update_filter();
		if (!f.loaded)
			rpc(LIST, NULL, NULL);
	} else if (page == FIND)
		input(&f.query, "");
	else if (page == INVITE)
		rpc(TICKET, NULL, NULL);
	else if (page == PROFILE && !f.profile)
		rpc(GET_PROFILE, NULL, NULL);
	else if (page == EDIT_NAME || page == EDIT_STATUS)
		input(&f.own,
		      ui_string(f.profile,
				page == EDIT_NAME ? "name" : "status", ""));
	focus(page == MANAGE  ? HIDDEN_PAGE :
	      page == PROFILE ? EDIT_MY_NAME :
	      page == INVITE  ? SUBMIT :
				FIELD);
}

static void forget_roster(void)
{
	clear_pending(
		&f.pending[LIST]); /* obsolete replies are discarded by tag */
	f.loaded = FALSE;
	drop(&f.rows);
	g_clear_pointer(&f.ids, g_hash_table_unref);
	g_clear_pointer(&f.next, g_free);
	input(&f.search, "");
	update_filter();
}

static void roster(enum friends_directory directory)
{
	if (f.directory != directory) {
		f.directory = directory;
		forget_roster();
	}
	page(ROSTER);
}

void ui_friends_show_view(int directory)
{
	if (ui.friends && ui.connected && directory >= EVERYONE &&
	    directory <= RECOMMENDED)
		roster(directory);
}

void ui_friends_show(void)
{
	if (ui.friends && ui.connected)
		roster(EVERYONE);
}

static gboolean known(Json *row, const char *field)
{
	return json_object_get_boolean(ui_field(row, field));
}

static gboolean can_add(Json *row)
{
	const char *token = ui_string(row, "lookup", "");

	return *token && !known(row, "friend") && !known(row, "self") &&
	       !known(row, "blocked") &&
	       (!ui_field(row, "canAdd") || known(row, "canAdd"));
}

static void find(void)
{
	if (!f.query.value->len || f.pending[LOOKUP].tag || f.pending[ADD].tag)
		return;
	clear_card();
	g_autofree char *query = g_strdup(f.query.value->str);

	g_strstrip(query);
	if (!*query || strlen(query) > 2048) {
		replace(&f.note,
			_("Enter a LINE ID, phone number or personal invite link."));
		return;
	}
	rpc(LOOKUP, "query", query);
}

static void details(Json *row)
{
	if (!row)
		return;
	Json *hold = json_object_get(row);

	page(DETAILS);
	f.person = hold;
	input(&f.alias, ui_string(row, "overrideName", ""));
	focus(f.directory == BLOCKED ? BLOCK_PERSON :
	      f.directory == HIDDEN  ? HIDE_PERSON :
				       SUBMIT);
}

static void activate(void)
{
	if (f.page == DETAILS) {
		if (f.directory == RECOMMENDED) {
			if (known(f.person, "recommended") &&
			    known(f.person, "canAdd"))
				rpc(ADD_RECOMMENDED, "id",
				    ui_string(f.person, "id", ""));
			return;
		}
		if (f.person && !known(f.person, "hidden") &&
		    !known(f.person, "blocked"))
			rpc(OPEN_CHAT, "id", ui_string(f.person, "id", ""));
		return;
	}
	if (f.page == INVITE) {
		if (f.url && SDL_SetClipboardText(f.url) == 0)
			replace(&f.note, _("Invite link copied."));
		else if (!f.url && !f.pending[TICKET].tag)
			rpc(TICKET, NULL, NULL);
	} else if (f.page == FIND) {
		if (!f.person)
			find();
		else if (known(f.person, "friend") &&
			 !known(f.person, "blocked") &&
			 !known(f.person, "hidden"))
			rpc(OPEN_CHAT, "id", ui_string(f.person, "id", ""));
		else if (can_add(f.person))
			rpc(ADD, "lookup", ui_string(f.person, "lookup", ""));
	} else if (f.page == ROSTER) {
		Json *row = person_at(f.selected);

		if (f.directory != EVERYONE)
			details(row);
		else if (row)
			rpc(OPEN_CHAT, "id", ui_string(row, "id", ""));
	}
}

static void store_row(Json *row)
{
	const char *id = ui_string(row, "id", "");

	if (!*id)
		return;
	if (!f.ids) {
		f.ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					      NULL);
		f.rows = json_object_new_array();
	}
	gpointer index = g_hash_table_lookup(f.ids, id);

	if (index)
		json_object_array_put_idx(f.rows, GPOINTER_TO_UINT(index) - 1,
					  json_object_get(row));
	else {
		g_hash_table_insert(
			f.ids, g_strdup(id),
			GUINT_TO_POINTER(
				(guint)json_object_array_length(f.rows) + 1));
		json_object_array_add(f.rows, json_object_get(row));
	}
}

gboolean ui_friends_reply(const char *method, guint tag, Json *result,
			  const char *error)
{
	enum friend_command cmd = LIST;

	while (cmd < N_REQUESTS && strcmp(method, methods[cmd]))
		cmd++;
	if (cmd == N_REQUESTS)
		return FALSE;
	struct friend_request *p = &f.pending[cmd];

	if (!p->tag || p->tag != tag)
		return TRUE;
	gboolean same = p->view == f.view;
	g_autofree char *subject = g_steal_pointer(&p->subject);
	g_autofree char *sent_name = g_steal_pointer(&p->sent_name);

	clear_pending(p);
	ui.dirty = TRUE;
	if (cmd == GET_PROFILE || cmd == EDIT_PROFILE) {
		if (!error && *ui_string(result, "name", "") &&
		    !g_strcmp0(ui.self_id, ui_string(result, "id", NULL))) {
			drop(&f.profile);
			f.profile = json_object_get(result);
			replace(&ui.name, ui_string(result, "name", ""));
			if (same && cmd == EDIT_PROFILE) {
				page(PROFILE);
				replace(&f.note, _("Profile updated."));
			}
		} else if (same)
			replace(&f.note,
				error ? error :
					_("Could not read your profile."));
		return TRUE;
	}
	if (cmd == ADD_RECOMMENDED) {
		if (!error &&
		    !g_strcmp0(subject, ui_string(result, "id", NULL)) &&
		    known(result, "friend")) {
			if (f.directory == RECOMMENDED &&
			    (same || f.page == ROSTER)) {
				forget_roster();
				page(ROSTER);
				replace(&f.note, _("Friend added."));
			} else if (f.directory == EVERYONE && f.loaded) {
				store_row(result);
				update_filter();
			}
		} else if (same)
			replace(&f.note,
				error ? error :
					_("Could not confirm this friend."));
		return TRUE;
	}
	if (cmd == LIST) {
		Json *items = ui_field(result, "friends");

		if (!error && json_object_is_type(items, json_type_array)) {
			if (!subject) {
				drop(&f.rows);
				g_clear_pointer(&f.ids, g_hash_table_unref);
			}
			guint first = f.first, selected = f.selected;

			for (size_t i = 0; i < json_object_array_length(items);
			     i++)
				store_row(json_object_array_get_idx(items, i));
			replace(&f.next, ui_string(result, "next", NULL));
			f.loaded = TRUE;
			update_filter();
			if (subject) {
				f.first = first;
				f.selected = selected;
			}
			if (f.page == ROSTER)
				replace(&f.note, NULL);
		} else if (f.page == ROSTER)
			replace(&f.note,
				error ? error :
					_("Could not read your friends list."));
		return TRUE;
	}
	if (cmd == KEEP_MEMO) {
		if (!error && json_object_is_type(result, json_type_object) &&
		    *ui_string(result, "id", "")) {
			page(OFF);
			ui_open_conversation(result);
		} else if (same)
			replace(&f.note,
				error ? error : _("Could not open Keep Memo."));
		return TRUE;
	}
	if ((cmd == BLOCKING || cmd == VISIBILITY || cmd == DELETE_FRIEND) &&
	    !error && !g_strcmp0(subject, ui_string(result, "id", NULL))) {
		forget_roster();
		if (same || f.page == ROSTER)
			page(ROSTER);
		return TRUE;
	}
	if (f.directory == EVERYONE &&
	    (cmd == ADD || cmd == FAVORITE || cmd == RENAME) && !error &&
	    *ui_string(result, "id", "")) {
		store_row(result);
		if (cmd == RENAME)
			ui_update_friend_chat(result);
		if (f.search.value)
			update_filter();
	}
	if (cmd == OPEN_CHAT && f.open_target &&
	    (!same || g_strcmp0(f.open_target, subject))) {
		g_autofree char *target = g_strdup(f.open_target);

		rpc(OPEN_CHAT, "id", target);
		return TRUE;
	}
	if (!same)
		return TRUE;
	if ((cmd == OPEN_CHAT || cmd == FAVORITE || cmd == RENAME) && !error &&
	    g_strcmp0(subject, ui_string(result, "id", "")))
		return TRUE;
	if (cmd == LOOKUP) {
		g_autofree char *query = g_strdup(f.query.value->str);

		g_strstrip(query);
		if (g_strcmp0(subject, query))
			return TRUE;
	}
	if (error) {
		if (cmd == ADD) {
			drop(&f.person);
			focus(FIELD);
		}
		replace(&f.note, error);
		return TRUE;
	}
	if (cmd == TICKET) {
		const char *url = ui_string(result, "url", "");

		if (*url && strlen(url) <= 2048)
			f.qr = QRcode_encodeString8bit(url, 0, QR_ECLEVEL_M);
		if (f.qr)
			replace(&f.url, url);
		else
			replace(&f.note,
				_("Your account has no personal invite link available."));
	} else if (cmd == FAVORITE || cmd == RENAME) {
		drop(&f.person);
		f.person = json_object_get(result);
		if (cmd == RENAME) {
			if (!g_strcmp0(sent_name, f.alias.value->str))
				input(&f.alias,
				      ui_string(result, "overrideName", ""));
			replace(&f.note, _("Display name updated."));
		} else
			replace(&f.note, known(result, "favorite") ?
						 _("Added to favorites.") :
						 _("Removed from favorites."));
	} else if (!*ui_string(result, "id", ""))
		replace(&f.note, _("LINE returned an incomplete profile."));
	else if (cmd == OPEN_CHAT) {
		page(OFF);
		ui_open_conversation(result);
	} else {
		drop(&f.person);
		f.person = json_object_get(result);
		if (cmd == ADD)
			replace(&f.note, _("Friend added."));
		else if (known(result, "self"))
			replace(&f.note, _("This is your profile."));
		else if (known(result, "blocked"))
			replace(&f.note, _("This person is blocked."));
		else if (known(result, "hidden"))
			replace(&f.note,
				_("This friend is in your hidden list."));
		else if (known(result, "friend"))
			replace(&f.note, _("Already in your friends list."));
		else
			replace(&f.note, NULL);
		/* A second Enter typed while searching must never add a contact. */
		focus(FIELD);
	}
	ui.dirty = TRUE;
	return TRUE;
}

void ui_friends_scan_result(const char *value, const char *error)
{
	if (f.page != FIND)
		return;
	clear_card();
	if (error) {
		replace(&f.note, error);
		return;
	}
	if (!value || (!g_str_has_prefix(value, "https://line.me/ti/p/") &&
		       !g_str_has_prefix(value, "https://line.me/R/ti/p/") &&
		       !g_str_has_prefix(value, "line://ti/p/"))) {
		replace(&f.note,
			_("This QR code does not contain a personal LINE invite link."));
		return;
	}
	input(&f.query, value);
	focus(FIELD);
	find();
}

static void advance(int dir)
{
	int count = f.filtered ? (int)f.filtered->len : 0;

	f.selected = CLAMP(f.selected + dir, 0, MAX(0, count - 1));
	int visible = MAX(1, f.list.h / ROW_H);

	f.first = CLAMP(f.first, MAX(0, f.selected - visible + 1), f.selected);
	ui.dirty = TRUE;
}

static void next_page(void)
{
	if (f.next && *f.next)
		rpc(LIST, "cursor", f.next);
	else if (!f.loaded)
		rpc(LIST, NULL, NULL);
}

static void button(Rect r, const char *label, gboolean primary,
		   enum friends_control id)
{
	if (f.focus == id)
		ui_draw_box((Rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 },
			    UI_ACCENT);
	ui_draw_button(r, label, primary);
}

static enum friends_control current_tab(void)
{
	switch (f.page) {
	case FIND:
		return ADD_PAGE;
	case INVITE:
		return QR_PAGE;
	case PROFILE:
	case EDIT_NAME:
	case EDIT_STATUS:
		return PROFILE_PAGE;
	default:
		return f.directory == HIDDEN	  ? HIDDEN_PAGE :
		       f.directory == BLOCKED	  ? BLOCKED_PAGE :
		       f.directory == RECOMMENDED ? RECOMMEND_PAGE :
						    ALL_PAGE;
	}
}

/* The Friends header keeps Add Friend visible beside the directory tabs. */
static void header(int x0, int w)
{
	gboolean narrow = w - x0 < 760;
	struct {
		enum friends_control id;
		const char *label, *short_label;
		int wide, slim;
	} tabs[] = {
		{ ALL_PAGE, _("All"), _("All"), 44, 36 },
		{ HIDDEN_PAGE, _("Hidden"), _("Hidden"), 70, 58 },
		{ BLOCKED_PAGE, _("Blocked"), _("Blocked"), 76, 64 },
		{ RECOMMEND_PAGE, _("Suggestions"), _("Suggested"), 106, 84 },
		{ PROFILE_PAGE, _("Profile"), _("Profile"), 66, 56 },
		{ QR_PAGE, _("My QR Code"), "QR", 102, 40 },
		{ ADD_PAGE, _("Add Friend"), _("Add"), 98, 52 },
		{ KEEP_MEMO_BTN, _("Keep Memo"), _("Memo"), 92, 54 },
	};
	int x = x0 + 16, y = 12;

	if (f.back.w)
		x = f.back.x + f.back.w + 12;
	if (!narrow) {
		ui_draw_icon((Rect){ x, 12, 24, 24 }, UI_ICON_PEOPLE, SUBTLE);
		ui_draw_label(_("Friends"), (Rect){ x + 32, 13, 70, 24 }, 16,
			      600, HEADING, FALSE);
		ui_draw_box((Rect){ x + 108, 14, 1, 20 }, UI_LINE);
		x += 124;
	}
	enum friends_control on = current_tab();

	f.tabs = 0;
	for (guint i = 0; i < G_N_ELEMENTS(tabs); i++) {
		if (tabs[i].id == QR_PAGE && !ui.friend_invite)
			continue;
		int tw = narrow ? tabs[i].slim : tabs[i].wide;

		/* Tabs that do not fit wrap onto a second row. */
		if (x + tw > w - 8 && y == 12) {
			x = x0 + 8;
			y = 42;
		}
		Rect r = { x, y, tw, 24 };
		gboolean add = tabs[i].id == ADD_PAGE;
		gboolean selected = tabs[i].id == on;
		gboolean hover = f.hover_tab == f.tabs;
		guint ink = selected ? HEADING : hover ? INK : TAB_INK;

		if (add)
			ui_draw_rounded(r, 4, selected ? BASE : UI_ACCENT);
		else if (selected || hover)
			ui_draw_rounded(r, 4,
					selected ? TAB_SELECTED : TAB_HOVER);
		if (f.focus == tabs[i].id)
			ui_draw_focus(r);
		ui_draw_centered(narrow ? tabs[i].short_label : tabs[i].label,
				 r, narrow ? 14 : 15, 500,
				 add ? (selected ? UI_ACCENT : UI_ON_ACCENT) :
				       ink);
		f.tab[f.tabs] = r;
		f.tab_id[f.tabs++] = tabs[i].id;
		x += tw + (narrow ? 6 : 8);
	}
	f.top = y == 12 ? 48 : 72;
	ui_draw_box((Rect){ x0, f.top, w - x0, 1 }, UI_RAIL);
}

void ui_friends_draw(void)
{
	int w = ui.width, h = ui.height, x0 = ui_content_x();
	int pane = w - x0, content = MIN(620, pane - 32),
	    x = x0 + (pane - content) / 2;
	gboolean sub = f.page == DETAILS || f.page == EDIT_NAME ||
		       f.page == EDIT_STATUS || f.page == MANAGE;

	ui_draw_box((Rect){ x0, 0, pane, h }, BASE);
	/* Without the sidebar, or on a sub-page, the header offers Back. */
	f.back = (Rect){ 0 };
	if (ui_compact() || sub)
		f.back = (Rect){ x0 + 8, 10, 58, 28 };
	if (f.back.w)
		button(f.back, ui_compact() && !sub ? _("Chats") : _("Back"),
		       FALSE, CHATS);
	header(x0, w);
	f.add = f.invite = f.entry = f.list = f.more = f.submit = f.scan =
		f.card = f.save = f.fav = (Rect){ 0 };
	f.manage = f.hidden = f.blocked = f.remove_btn = f.confirm = f.cancel =
		(Rect){ 0 };
	f.profile_button = f.recommendations = (Rect){ 0 };
	if (f.page == PROFILE) {
		if (f.profile) {
			ui_draw_profile(f.profile, (Rect){ x, 91, 52, 52 });
			ui_draw_label(ui_string(f.profile, "name", ""),
				      (Rect){ x + 67, 91, content - 67, 31 },
				      21, 650, INK, FALSE);
			const char *id = ui_string(f.profile, "lineId", NULL);

			if (id)
				ui_draw_label(id,
					      (Rect){ x + 67, 126, content - 67,
						      22 },
					      14, 450, MUTED, FALSE);
			f.profile_button =
				(Rect){ x, 171, MIN(188, content), 36 };
			button(f.profile_button, _("Edit display name"), FALSE,
			       EDIT_MY_NAME);
			ui_draw_label(_("Status message"),
				      (Rect){ x, 234, content, 25 }, 15, 550,
				      INK, FALSE);
			const char *status = ui_string(f.profile, "status", "");

			ui_draw_label(*status ? status : _("No status message"),
				      (Rect){ x, 266, content, 69 }, 15, 450,
				      MUTED, TRUE);
			f.save = (Rect){ x, 346, MIN(188, content), 36 };
			button(f.save, _("Edit status message"), FALSE,
			       EDIT_MY_STATUS);
		} else if (!f.pending[GET_PROFILE].tag) {
			f.submit = (Rect){ x, 155, 100, 36 };
			button(f.submit, _("Retry"), FALSE, SUBMIT);
		}
		if (f.note || f.pending[GET_PROFILE].tag)
			ui_draw_label(f.note ? f.note : _("Loading profile…"),
				      (Rect){ x, f.profile ? 401 : 90, content,
					      55 },
				      14, 450, MUTED, TRUE);
	} else if (f.page == EDIT_NAME || f.page == EDIT_STATUS) {
		f.entry = (Rect){ x, 100, content, 42 };
		ui_draw_edit(&f.own, f.entry, f.focus == FIELD,
			     f.page == EDIT_NAME ? _("Display name") :
						   _("Status message"));
		f.save = (Rect){ x, 165, 105, 36 };
		button(f.save,
		       f.pending[EDIT_PROFILE].tag ? _("Saving…") : _("Save"),
		       TRUE, SAVE_NAME);
		if (f.note)
			ui_draw_label(f.note, (Rect){ x, 229, content, 90 }, 14,
				      450, MUTED, TRUE);
	} else if (f.page == MANAGE) {
		f.hidden = (Rect){ x, 94, MIN(content, 195), 38 };
		f.blocked = (Rect){ x, 146, MIN(content, 195), 38 };
		button(f.hidden, _("Hidden friends"), FALSE, HIDDEN_PAGE);
		button(f.blocked, _("Blocked friends"), FALSE, BLOCKED_PAGE);
		f.recommendations = (Rect){ x, 198, MIN(content, 218), 38 };
		f.profile_button = (Rect){ x, 250, MIN(content, 195), 38 };
		button(f.recommendations, _("Friend recommendations"), FALSE,
		       RECOMMEND_PAGE);
		button(f.profile_button, _("My profile"), FALSE, PROFILE_PAGE);
	} else if (f.page == ROSTER) {
		/* The roster spans the pane with Discord's 30 px gutters. */
		int rx = x0 + 30, rw = MAX(120, pane - 60);

		f.entry = (Rect){ rx, f.top + 16, rw, 34 };
		ui_draw_edit(&f.search, f.entry, f.focus == FIELD, _("Search"));
		int bottom = h - 18;

		if (f.note && *f.note)
			bottom -= 44;
		if ((f.next && *f.next) ||
		    (!f.loaded && !f.pending[LIST].tag)) {
			f.more = (Rect){ rx, bottom - 34, MIN(170, rw), 34 };
			button(f.more,
			       f.pending[LIST].tag ? _("Loading…") :
			       f.loaded		   ? _("Load more friends") :
						     _("Retry"),
			       FALSE, MORE);
			bottom -= 42;
		}
		if (f.note && *f.note)
			ui_draw_label(f.note, (Rect){ rx, h - 52, rw, 45 }, 14,
				      450, MUTED, TRUE);
		const char *title = f.directory == HIDDEN  ? "HIDDEN" :
				    f.directory == BLOCKED ? "BLOCKED" :
				    f.directory == RECOMMENDED ?
							     "SUGGESTIONS" :
							     _("ALL FRIENDS");
		g_autofree char *count = g_strdup_printf(
			"%s — %u", title, f.filtered ? f.filtered->len : 0);

		ui_draw_label(count, (Rect){ rx + 2, f.top + 66, rw, 20 }, 12,
			      600, SUBTLE, FALSE);
		int top = f.top + 92;

		f.list = (Rect){ rx, top, rw, MAX(0, bottom - top) };
		int visible = f.list.h / ROW_H;

		f.first = CLAMP(f.first, 0,
				MAX(0, (int)f.filtered->len - visible));
		for (int i = 0; i < visible; i++) {
			int idx = f.first + i;
			Json *row = person_at(idx);

			if (!row)
				break;
			Rect r = { rx, f.list.y + i * ROW_H, rw, ROW_H };
			gboolean lit = (f.focus == ROWS && idx == f.selected) ||
				       idx == f.hover;
			gboolean prev_lit =
				(f.focus == ROWS && idx - 1 == f.selected) ||
				idx - 1 == f.hover;

			if (lit)
				ui_draw_rounded((Rect){ r.x - 10, r.y, r.w + 20,
							r.h },
						8, TAB_HOVER);
			else if (i && !prev_lit)
				ui_draw_box((Rect){ r.x, r.y, r.w, 1 },
					    DIVIDER);
			ui_draw_profile(row, (Rect){ r.x, r.y + 15, 32, 32 });
			const char *sub_line =
				f.directory == HIDDEN ? _("Hidden") :
				f.directory == BLOCKED ?
							_("Blocked") :
				f.directory == RECOMMENDED ?
							_("Suggested for you") :
				known(row, "favorite") ? _("★ Favorite") :
							 NULL;
			int name_y = sub_line ? r.y + 11 : r.y + 19;

			ui_draw_label(ui_string(row, "name", "LINE user"),
				      (Rect){ r.x + 44, name_y, rw - 150, 24 },
				      16, 600, HEADING, FALSE);
			if (sub_line)
				ui_draw_label(sub_line,
					      (Rect){ r.x + 44, r.y + 34,
						      rw - 150, 20 },
					      14, 500, SUBTLE, FALSE);
			/* Round actions: message (opens the chat) and more. */
			guint fill = lit ? UI_RAIL : UI_PANEL;
			Rect more = { r.x + rw - 36, r.y + 13, 36, 36 };

			ui_draw_rounded(more, 18, fill);
			ui_draw_icon(more, UI_ICON_MORE, TAB_INK);
			if (f.directory == EVERYONE) {
				Rect chat = { more.x - 46, more.y, 36, 36 };

				ui_draw_rounded(chat, 18, fill);
				ui_draw_icon(chat, UI_ICON_MESSAGE, TAB_INK);
			}
		}
		if (!f.filtered->len)
			ui_draw_label(
				f.pending[LIST].tag ?
					_("Loading friends…") :
				f.search.value->len ?
					_("No matching names loaded.") :
				f.loaded ?
					(f.directory == RECOMMENDED ?
						 _("No friend recommendations.") :
						 _("No friends to show.")) :
					"",
				(Rect){ rx + 2, f.list.y + 8, rw - 4, 55 }, 15,
				450, MUTED, TRUE);
	} else if (f.page == FIND) {
		/* Discord's Add Friend: title, hint, input with its button. */
		int rx = x0 + 30, rw = MAX(160, pane - 60);

		x = rx;
		content = rw;
		int t = f.top;

		ui_draw_label(_("Add Friend"), (Rect){ rx, t + 18, rw, 28 }, 20,
			      600, HEADING, FALSE);
		ui_draw_label(
			_("You can add friends with their LINE ID, phone number or personal link."),
			(Rect){ rx, t + 50, rw, 22 }, 14, 450, TAB_INK, FALSE);
		/* One dark strip holds both the field and its button. */
		ui_draw_rounded((Rect){ rx, t + 86, rw, 50 }, 8, UI_RAIL);
		f.entry = (Rect){ rx, t + 86, rw - 136, 50 };
		ui_draw_edit(&f.query, f.entry, f.focus == FIELD,
			     _("ID, +country code and number, or link"));
		f.submit = (Rect){ rx + rw - 128, t + 94, 116, 34 };
		if (!f.person)
			button(f.submit,
			       f.pending[LOOKUP].tag ? _("Looking…") :
						       _("Find"),
			       TRUE, SUBMIT);
		f.scan = (Rect){ rx, t + 148, 132, 34 };
		button(f.scan, _("Read QR image"), FALSE, SCAN);
		if (f.person) {
			int y = t + 198;

			f.card = (Rect){ x, y, content, 94 };
			ui_draw_rounded(f.card, 8, PANEL);
			ui_draw_profile(f.person,
					(Rect){ x + 14, y + 17, 48, 48 });
			ui_draw_label(ui_string(f.person, "name", "LINE user"),
				      (Rect){ x + 76, y + 16, content - 90,
					      32 },
				      19, 650, INK, FALSE);
			gboolean open = known(f.person, "friend") &&
					!known(f.person, "blocked") &&
					!known(f.person, "hidden");
			if (open || can_add(f.person)) {
				f.submit = (Rect){ x + 76, y + 53,
						   MIN(144, content - 94), 32 };
				button(f.submit,
				       open ? (f.pending[OPEN_CHAT].tag ?
						       _("Opening…") :
						       _("Open chat")) :
					      (f.pending[ADD].tag ?
						       _("Adding…") :
						       _("Add friend")),
				       TRUE, SUBMIT);
			} else
				f.submit = (Rect){ 0 };
		}
		if (f.note)
			ui_draw_label(f.note,
				      (Rect){ x, t + (f.person ? 308 : 198),
					      content, 80 },
				      14, 500, MUTED, TRUE);
	} else if (f.page == DETAILS && f.person) {
		ui_draw_profile(f.person, (Rect){ x, 91, 52, 52 });
		ui_draw_label(ui_string(f.person, "name", "LINE user"),
			      (Rect){ x + 67, 94, content - 68, 30 }, 21, 650,
			      INK, FALSE);
		const char *original = ui_string(f.person, "profileName", "");

		if (*original &&
		    g_strcmp0(original, ui_string(f.person, "name", "")))
			ui_draw_label(original,
				      (Rect){ x + 68, 128, content - 68, 25 },
				      14, 450, MUTED, FALSE);
		if (f.directory == RECOMMENDED) {
			if (known(f.person, "canAdd")) {
				f.submit = (Rect){ x, 170, 125, 36 };
				button(f.submit,
				       f.pending[ADD_RECOMMENDED].tag ?
					       _("Adding…") :
					       _("Add friend"),
				       TRUE, SUBMIT);
			}
			if (f.note)
				ui_draw_label(f.note,
					      (Rect){ x, 233, content, 70 }, 14,
					      450, MUTED, TRUE);
			return;
		}
		if (f.confirming || f.confirming_delete) {
			ui_draw_label(f.confirming_delete ?
					      _("Delete this friend?") :
					      _("Block this friend?"),
				      (Rect){ x, 174, content, 34 }, 20, 650,
				      INK, FALSE);
			ui_draw_label(
				f.confirming_delete ?
					_("This removes them from your LINE friends list.") :
					_("LINE will stop delivering their messages and calls."),
				(Rect){ x, 219, content, 52 }, 15, 450, MUTED,
				TRUE);
			f.cancel = (Rect){ x, 285, 100, 36 };
			f.confirm = (Rect){ x + 112, 285, 145, 36 };
			button(f.cancel, _("Cancel"), FALSE, CANCEL);
			button(f.confirm,
			       f.confirming_delete ? _("Confirm delete") :
						     _("Confirm block"),
			       TRUE, CONFIRM);
			if (f.note)
				ui_draw_label(f.note,
					      (Rect){ x, 343, content, 56 }, 14,
					      450, MUTED, TRUE);
			return;
		}
		if (known(f.person, "hidden") || known(f.person, "blocked")) {
			if (known(f.person, "blocked")) {
				f.blocked = (Rect){ x, 170, 124, 36 };
				button(f.blocked, _("Unblock"), FALSE,
				       BLOCK_PERSON);
			} else {
				f.hidden = (Rect){ x, 170, 124, 36 };
				button(f.hidden, _("Unhide"), FALSE,
				       HIDE_PERSON);
			}
			f.remove_btn = (Rect){ x + 136, 170, 124, 36 };
			button(f.remove_btn, _("Delete"), FALSE, DELETE_PERSON);
			if (f.note)
				ui_draw_label(f.note,
					      (Rect){ x, 230, content, 60 }, 14,
					      450, MUTED, TRUE);
			return;
		}
		f.submit = (Rect){ x, 165, 114, 36 };
		button(f.submit, _("Open chat"), TRUE, SUBMIT);
		f.fav = (Rect){ x + 127, 165, MIN(content - 127, 182), 36 };
		button(f.fav,
		       known(f.person, "favorite") ? _("Remove favorite") :
						     _("Add to favorites"),
		       FALSE, STAR);
		ui_draw_label(_("Display name"), (Rect){ x, 230, content, 24 },
			      15, 550, INK, FALSE);
		f.entry = (Rect){ x, 260, content, 38 };
		ui_draw_edit(&f.alias, f.entry, f.focus == FIELD,
			     _("Use their LINE profile name"));
		f.save = (Rect){ x, 311, 115, 34 };
		button(f.save,
		       f.pending[RENAME].tag ? _("Saving…") : _("Save name"),
		       FALSE, SAVE_NAME);
		f.hidden = (Rect){ x, 363, 115, 34 };
		f.blocked = (Rect){ x + 128, 363, 115, 34 };
		f.remove_btn = (Rect){ x + 256, 363, 115, 34 };
		button(f.hidden, _("Hide"), FALSE, HIDE_PERSON);
		button(f.blocked, _("Block"), FALSE, BLOCK_PERSON);
		button(f.remove_btn, _("Delete"), FALSE, DELETE_PERSON);
		if (f.note)
			ui_draw_label(f.note, (Rect){ x, 412, content, 56 }, 14,
				      450, MUTED, TRUE);
	} else if (f.page == INVITE) {
		int bound = MIN(244, MIN(content, h - 233)), top = 95;

		if (f.qr) {
			int step = MAX(1, bound / (f.qr->width + 8));
			int actual = (f.qr->width + 8) * step;

			ui_draw_code(f.qr, w / 2, top, bound);
			ui_draw_label(f.url,
				      (Rect){ x, top + actual + 13, content,
					      48 },
				      14, 500, INK, TRUE);
			f.submit = (Rect){ x, top + actual + 67, 122, 36 };
			button(f.submit, _("Copy link"), TRUE, SUBMIT);
		} else if (!f.pending[TICKET].tag) {
			f.submit = (Rect){ x, 162, 92, 36 };
			button(f.submit, _("Retry"), FALSE, SUBMIT);
		}
		if (f.note || f.pending[TICKET].tag)
			ui_draw_label(f.note ? f.note :
					       _("Loading your invite link…"),
				      (Rect){ x, f.qr ? f.submit.y + 48 : 100,
					      content, 58 },
				      14, 500, MUTED, TRUE);
	}
}

static void leave(void)
{
	if (f.page == EDIT_NAME || f.page == EDIT_STATUS) {
		if (!f.pending[EDIT_PROFILE].tag)
			page(PROFILE);
	} else if (f.page == PROFILE)
		roster(EVERYONE);
	else if ((f.confirming || f.confirming_delete) &&
		 !f.pending[BLOCKING].tag && !f.pending[DELETE_FRIEND].tag) {
		f.confirming = FALSE;
		f.confirming_delete = FALSE;
		focus(BLOCK_PERSON);
		ui.dirty = TRUE;
	} else if (f.page == MANAGE)
		roster(EVERYONE);
	else if (f.page == ROSTER)
		f.directory == EVERYONE ? page(OFF) : roster(EVERYONE);
	else
		page(ROSTER);
}

static void press(enum friends_control c)
{
	gboolean busy = f.pending[FAVORITE].tag || f.pending[RENAME].tag ||
			f.pending[VISIBILITY].tag || f.pending[BLOCKING].tag ||
			f.pending[DELETE_FRIEND].tag;
	if (f.page == DETAILS && busy && c != CHATS)
		return;
	switch (c) {
	case ALL_PAGE:
		roster(EVERYONE);
		break;
	case RECOMMEND_PAGE:
		roster(RECOMMENDED);
		break;
	case PROFILE_PAGE:
		drop(&f.profile);
		page(PROFILE);
		break;
	case EDIT_MY_NAME:
	case EDIT_MY_STATUS:
		if (f.profile)
			page(c == EDIT_MY_NAME ? EDIT_NAME : EDIT_STATUS);
		break;
	case MANAGE_PAGE:
		page(MANAGE);
		break;
	case HIDDEN_PAGE:
		roster(HIDDEN);
		break;
	case BLOCKED_PAGE:
		roster(BLOCKED);
		break;
	case HIDE_PERSON:
		if (f.person)
			rpc(VISIBILITY, "id", ui_string(f.person, "id", ""));
		break;
	case BLOCK_PERSON:
		if (!f.person)
			break;
		if (known(f.person, "blocked"))
			rpc(BLOCKING, "id", ui_string(f.person, "id", ""));
		else {
			f.confirming = TRUE;
			f.confirming_delete = FALSE;
			focus(CANCEL);
			ui.dirty = TRUE;
		}
		break;
	case DELETE_PERSON:
		if (!f.person)
			break;
		f.confirming = FALSE;
		f.confirming_delete = TRUE;
		focus(CANCEL);
		ui.dirty = TRUE;
		break;
	case KEEP_MEMO_BTN:
		rpc(KEEP_MEMO, NULL, NULL);
		break;
	case CANCEL:
		f.confirming = FALSE;
		f.confirming_delete = FALSE;
		focus(BLOCK_PERSON);
		ui.dirty = TRUE;
		break;
	case CONFIRM:
		if (f.confirming_delete && f.person)
			rpc(DELETE_FRIEND, "id", ui_string(f.person, "id", ""));
		else if (f.confirming && f.person &&
			 !known(f.person, "blocked"))
			rpc(BLOCKING, "id", ui_string(f.person, "id", ""));
		break;
	case CHATS:
		leave();
		break;
	case ADD_PAGE:
		page(FIND);
		break;
	case QR_PAGE:
		page(INVITE);
		break;
	case SCAN:
		ui_friend_pick_qr();
		break;
	case MORE:
		next_page();
		break;
	case STAR:
		if (f.person && !f.pending[RENAME].tag)
			rpc(FAVORITE, "id", ui_string(f.person, "id", ""));
		break;
	case SAVE_NAME:
		if (f.page == EDIT_NAME || f.page == EDIT_STATUS) {
			const char *key = f.page == EDIT_NAME ? "name" :
								"status";
			if (strcmp(f.own.value->str,
				   ui_string(f.profile, key, "")))
				rpc(EDIT_PROFILE, key, f.own.value->str);
			else
				page(PROFILE);
		} else if (f.person && !f.pending[FAVORITE].tag &&
			   strcmp(f.alias.value->str,
				  ui_string(f.person, "overrideName", "")))
			rpc(RENAME, "id", ui_string(f.person, "id", ""));
		break;
	case FIELD:
		if (f.page == DETAILS || f.page == EDIT_NAME ||
		    f.page == EDIT_STATUS) {
			press(SAVE_NAME);
			break;
		}
		if (f.page == FIND) {
			if (!f.person)
				find();
		} else if (f.page == ROSTER) {
			focus(ROWS);
			activate();
		}
		break;
	default:
		if (f.page == PROFILE && !f.profile)
			rpc(GET_PROFILE, NULL, NULL);
		else
			activate();
		break;
	}
}

static void tab(gboolean back)
{
	enum friends_control list[24];
	guint n = 0;

	if (f.back.w)
		list[n++] = CHATS;
	for (int t = 0; t < f.tabs; t++)
		list[n++] = f.tab_id[t];
	if (f.page == ROSTER) {
		list[n++] = FIELD;
		if (f.filtered && f.filtered->len)
			list[n++] = ROWS;
		if (f.more.w)
			list[n++] = MORE;
	} else if (f.page == MANAGE) {
		list[n++] = HIDDEN_PAGE;
		list[n++] = BLOCKED_PAGE;
		list[n++] = RECOMMEND_PAGE;
		list[n++] = PROFILE_PAGE;
	} else if (f.page == PROFILE) {
		if (f.profile) {
			list[n++] = EDIT_MY_NAME;
			list[n++] = EDIT_MY_STATUS;
		} else if (f.submit.w)
			list[n++] = SUBMIT;
	} else if (f.page == EDIT_NAME || f.page == EDIT_STATUS) {
		list[n++] = FIELD;
		list[n++] = SAVE_NAME;
	} else if (f.page == DETAILS) {
		if (f.directory == RECOMMENDED) {
			if (f.submit.w)
				list[n++] = SUBMIT;
		} else if (f.confirming || f.confirming_delete) {
			list[n++] = CANCEL;
			list[n++] = CONFIRM;
		} else {
			if (f.submit.w) {
				list[n++] = SUBMIT;
				list[n++] = STAR;
				list[n++] = FIELD;
				list[n++] = SAVE_NAME;
			}
			if (f.hidden.w)
				list[n++] = HIDE_PERSON;
			if (f.blocked.w)
				list[n++] = BLOCK_PERSON;
			if (f.remove_btn.w)
				list[n++] = DELETE_PERSON;
		}
	} else if (f.page == FIND) {
		list[n++] = FIELD;
		list[n++] = SCAN;
		if (f.submit.w)
			list[n++] = SUBMIT;
	} else
		list[n++] = SUBMIT;
	guint i = 0;

	while (i < n && list[i] != f.focus)
		i++;
	focus(list[(i + n + (back ? -1 : 1)) % n]);
}

static void changed(const char *before)
{
	Editor *edit = editor();

	if (!edit || !strcmp(before, edit->value->str))
		return;
	if (f.page == ROSTER)
		update_filter();
	else if (f.page == FIND) {
		f.view++;
		clear_card();
	}
}

gboolean ui_friends_event(SDL_Event *e)
{
	if (!ui.connected || !ui.friends)
		return FALSE;
	if (!f.page) {
		if (e->type == SDL_KEYDOWN && !e->key.repeat &&
		    (e->key.keysym.mod & KMOD_CTRL) &&
		    e->key.keysym.sym == SDLK_2) {
			ui_friends_show();
			return TRUE;
		}
		return FALSE;
	}
	Editor *ed = editor();

	switch (e->type) {
	case SDL_KEYDOWN: {
		SDL_Keycode code = e->key.keysym.sym;
		gboolean ctrl = e->key.keysym.mod & KMOD_CTRL;

		if (ui.composition && *ui.composition && f.focus == FIELD)
			return TRUE;
		if (code == SDLK_ESCAPE) {
			leave();
			return TRUE;
		}
		if (ctrl && code == SDLK_1) {
			page(OFF);
			return TRUE;
		}
		if (ctrl && code == SDLK_r && f.page == ROSTER) {
			rpc(LIST, NULL, NULL);
			return TRUE;
		}
		if (f.page == ROSTER &&
		    (code == SDLK_MENU ||
		     (code == SDLK_F10 && (e->key.keysym.mod & KMOD_SHIFT)))) {
			details(person_at(f.selected));
			return TRUE;
		}
		if (code == SDLK_TAB) {
			tab(e->key.keysym.mod & KMOD_SHIFT);
			return TRUE;
		}
		if (f.page == ROSTER &&
		    (code == SDLK_DOWN || code == SDLK_UP ||
		     code == SDLK_PAGEDOWN || code == SDLK_PAGEUP)) {
			int dist =
				(code == SDLK_PAGEDOWN || code == SDLK_PAGEUP) ?
					MAX(1, f.list.h / ROW_H) :
					1;
			focus(ROWS);
			advance(code == SDLK_DOWN || code == SDLK_PAGEDOWN ?
					dist :
					-dist);
			return TRUE;
		}
		if (code == SDLK_RETURN || code == SDLK_KP_ENTER) {
			if (!e->key.repeat)
				press(f.focus);
			return TRUE;
		}
		if (code == SDLK_SPACE && f.focus != FIELD) {
			if (!e->key.repeat)
				press(f.focus);
			return TRUE;
		}
		if (ed && f.focus == FIELD && !f.pending[EDIT_PROFILE].tag) {
			g_autofree char *before = g_strdup(ed->value->str);

			ui_edit_key(ed, e->key);
			changed(before);
		}
		return TRUE;
	}
	case SDL_TEXTINPUT:
		if (ed && f.focus == FIELD && ed->value->len < 2048 &&
		    !f.pending[EDIT_PROFILE].tag) {
			g_autofree char *before = g_strdup(ed->value->str);

			ui_insert(ed, e->text.text);
			changed(before);
		}
		return TRUE;
	case SDL_TEXTEDITING:
		if (ed && f.focus == FIELD)
			replace(&ui.composition, e->edit.text);
		return TRUE;
	case SDL_MOUSEBUTTONDOWN: {
		int x = e->button.x, y = e->button.y;

		/* The rail and list stay live: picking a chat leaves Friends. */
		if (x < ui_content_x()) {
			page(OFF);
			return FALSE;
		}
		if (e->button.button == SDL_BUTTON_LEFT)
			for (int t = 0; t < f.tabs; t++)
				if (hit(f.tab[t], x, y)) {
					press(f.tab_id[t]);
					return TRUE;
				}
		if (e->button.button == SDL_BUTTON_RIGHT && hit(f.list, x, y)) {
			details(person_at(f.first + (y - f.list.y) / ROW_H));
			return TRUE;
		}
		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (hit(f.back, x, y))
			leave();
		else if (hit(f.add, x, y))
			page(FIND);
		else if (hit(f.invite, x, y))
			page(INVITE);
		else if (hit(f.manage, x, y))
			press(MANAGE_PAGE);
		else if (hit(f.recommendations, x, y))
			press(RECOMMEND_PAGE);
		else if (hit(f.profile_button, x, y))
			press(f.page == PROFILE ? EDIT_MY_NAME : PROFILE_PAGE);
		else if (hit(f.hidden, x, y))
			press(f.page == MANAGE ? HIDDEN_PAGE : HIDE_PERSON);
		else if (hit(f.blocked, x, y))
			press(f.page == MANAGE ? BLOCKED_PAGE : BLOCK_PERSON);
		else if (hit(f.remove_btn, x, y))
			press(DELETE_PERSON);
		else if (hit(f.cancel, x, y))
			press(CANCEL);
		else if (hit(f.confirm, x, y))
			press(CONFIRM);
		else if (hit(f.scan, x, y))
			ui_friend_pick_qr();
		else if (hit(f.more, x, y))
			next_page();
		else if (hit(f.fav, x, y))
			press(STAR);
		else if (hit(f.save, x, y))
			press(f.page == PROFILE ? EDIT_MY_STATUS : SAVE_NAME);
		else if (hit(f.submit, x, y))
			press(SUBMIT);
		else if (ed && hit(f.entry, x, y)) {
			focus(FIELD);
			ed->cursor = ed->anchor = ui_edit_hit(ed, f.entry, x);
			f.drag = TRUE;
		} else if (hit(f.list, x, y)) {
			int idx = f.first + (y - f.list.y) / ROW_H;

			if (person_at(idx)) {
				if (x >= f.list.x + f.list.w - 56) {
					details(person_at(idx));
					return TRUE;
				}
				f.selected = idx;
				focus(ROWS);
				activate();
			}
		}
		return TRUE;
	}
	case SDL_MOUSEMOTION: {
		int x = e->motion.x, y = e->motion.y;
		int hover = hit(f.list, x, y) ?
				    f.first + (y - f.list.y) / ROW_H :
				    -1;
		int hover_tab = -1;

		for (int t = 0; t < f.tabs; t++)
			if (hit(f.tab[t], x, y))
				hover_tab = t;
		if (f.hover != hover || f.hover_tab != hover_tab) {
			f.hover = hover;
			f.hover_tab = hover_tab;
			ui.dirty = TRUE;
		}
		/* Let the sidebar track its own hover states. */
		if (x < ui_content_x() && !f.drag)
			return FALSE;
		if (f.drag && ed && (e->motion.state & SDL_BUTTON_LMASK)) {
			int pos = ui_edit_hit(ed, f.entry, x);

			if (pos != ed->cursor) {
				ed->cursor = pos;
				ui.dirty = TRUE;
			}
		}
		return TRUE;
	}
	case SDL_MOUSEBUTTONUP:
		f.drag = FALSE;
		return TRUE;
	case SDL_MOUSEWHEEL: {
		static double carry;
		int rows = ui_wheel_rows(ui_wheel_y(&e->wheel), &carry);

		if (ui.hover_x < ui_content_x())
			return FALSE;
		if (f.page == ROSTER && f.filtered) {
			f.first = CLAMP(f.first - 3 * rows, 0,
					MAX(0, (int)f.filtered->len -
						       f.list.h / ROW_H));
			ui.dirty = TRUE;
		}
		return TRUE;
	}
	case SDL_DROPFILE:
		if (f.page == FIND)
			ui_friend_scan_file(e->drop.file);
		SDL_free(e->drop.file);
		return TRUE;
	default:
		return FALSE;
	}
}

void ui_friends_open_link(const char *url)
{
	if (!ui.connected || !ui.friends || !url || !*url)
		return;
	f.directory = EVERYONE;
	page(FIND);
	input(&f.query, url);
	focus(FIELD);
	find();
}
