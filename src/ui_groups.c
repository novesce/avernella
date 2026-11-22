// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_groups.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include "ui_friends.h"
#include <string.h>

enum group_pane {
	CLOSED,
	CREATE,
	MEMBERS,
	INVITE,
	REVIEW_CREATE,
	REVIEW_INVITE,
	INBOX,
	OFFER,
	DECLINE
};

enum group_control {
	NAME,
	FILTER,
	ROWS,
	MORE,
	BACK,
	PRIMARY,
	INCOMING,
	REFUSE
};

struct visible_member {
	Json *row;
	gboolean pending;
};

static struct {
	enum group_pane pane;
	enum group_control focus;
	char *chat, *next, *notice;
	Editor name, filter;
	Json *friends, *chosen, *detail, *unavailable, *invitations, *offer;
	GArray *rows;
	guint serial, pending;
	gboolean writing, loaded;
	int first, cursor;
	Rect panel, name_box, filter_box, list, back, primary, more, incoming,
		refuse;
} g;

enum manage_page {
	MG_CLOSED,
	MG_MENU,
	MG_EDIT,
	MG_LINK,
	MG_CONFIRM
};

enum manage_action {
	M_BACK,
	M_RENAME,
	M_SAVE,
	M_LINK,
	M_COPY,
	M_ENABLE,
	M_DISABLE,
	M_RENEW,
	M_LEAVE,
	M_REMOVE,
	M_WITHDRAW,
	M_REFRESH,
	M_CONFIRM
};

static struct {
	enum manage_page page;
	enum manage_action question;
	char *person, *person_name;
	Json *link;
	QRcode *qr;
	int focus, count;

	struct {
		Rect rect;
		enum manage_action action;
	} buttons[6];
} manager;

static void manage_reset(void);

static gboolean form(void)
{
	return g.pane == CREATE || g.pane == INVITE;
}

static gboolean review(void)
{
	return g.pane == REVIEW_CREATE || g.pane == REVIEW_INVITE;
}

static gboolean hit(Rect r, int x, int y)
{
	return r.w && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void set(char **where, const char *s)
{
	char *copy = g_strdup(s);

	g_free(*where);
	*where = copy;
	ui.dirty = TRUE;
}

static gboolean flag(Json *r, const char *key)
{
	return json_object_get_boolean(ui_field(r, key));
}

static guint size(Json *array)
{
	return json_object_is_type(array, json_type_array) ?
		       json_object_array_length(array) :
		       0;
}

static int slots(void)
{
	return MAX(1, g.list.h / 40);
}

void ui_groups_reset(void)
{
	manage_reset();
	if (g.name.value)
		g_string_free(g.name.value, TRUE);
	if (g.filter.value)
		g_string_free(g.filter.value, TRUE);
	g_free(g.chat);
	g_free(g.next);
	g_free(g.notice);
	json_object_put(g.friends);
	json_object_put(g.detail);
	json_object_put(g.chosen);
	json_object_put(g.unavailable);
	json_object_put(g.invitations);
	json_object_put(g.offer);
	g_clear_pointer(&g.rows, g_array_unref);
	guint serial = g.serial;

	memset(&g, 0, sizeof(g));
	g.serial = serial;
	ui.dirty = TRUE;
}

gboolean ui_groups_active(void)
{
	return g.pane != CLOSED;
}

static void close_group(void)
{
	ui_groups_reset();
	set(&ui.composition, "");
	ui_restore_input();
}

static Editor *focused_editor(void)
{
	if (g.pane == CREATE && g.focus == NAME)
		return &g.name;
	return form() && g.focus == FILTER ? &g.filter : NULL;
}

void ui_groups_resume_input(void)
{
	if ((manager.page == MG_EDIT && manager.focus < 0) ||
	    (!manager.page && focused_editor()))
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

static void focus(enum group_control control)
{
	g.focus = control;
	set(&ui.composition, "");
	ui_groups_resume_input();
}

static void rpc(const char *method, Json *params, gboolean write)
{
	if (g.pending) {
		json_object_put(params);
		return;
	}
	g.serial = (g.serial + 1) & 0x7fffffffu;
	g.pending = 0x80000000u | g.serial;
	g.writing = write;
	ui_rpc(method, params, g.pending);
	ui.dirty = TRUE;
}

static void friends_page(void)
{
	Json *p = json_object_new_object();

	if (g.next)
		json_object_object_add(p, "cursor",
				       json_object_new_string(g.next));
	rpc("friends", p, FALSE);
}

static void load_members(void)
{
	Json *p = json_object_new_object();

	json_object_object_add(p, "chat", json_object_new_string(g.chat));
	rpc("groupMembers", p, FALSE);
}

static void init(enum group_pane pane)
{
	ui_groups_reset();
	g.pane = pane;
	g.name.value = g_string_new("");
	g.filter.value = g_string_new("");
	g.chosen = json_object_new_object();
	g.unavailable = json_object_new_object();
	g.friends = json_object_new_array();
	g.rows = g_array_new(FALSE, FALSE, sizeof(struct visible_member));
}

void ui_groups_new(void)
{
	if (!ui.connected || !ui.groups || g.writing)
		return;
	init(CREATE);
	focus(NAME);
	friends_page();
}

void ui_groups_members(void)
{
	if (!ui.connected || !ui.groups || !ui.selected ||
	    ui.selected[0] != 'c' || g.writing)
		return;
	init(MEMBERS);
	g.chat = g_strdup(ui.selected);
	focus(ROWS);
	load_members();
}

static void add_visible(Json *list, gboolean pending)
{
	for (guint i = 0; i < size(list); i++) {
		Json *row = json_object_array_get_idx(list, i);
		const char *id = ui_string(row, "id", "");

		if (!*id)
			continue;
		if (form() &&
		    (flag(row, "blocked") || flag(row, "self") ||
		     flag(row, "hidden") ||
		     (ui_field(row, "friend") && !flag(row, "friend")) ||
		     ui_field(g.unavailable, id) ||
		     (g.filter.value->len &&
		      !g_str_match_string(g.filter.value->str,
					  ui_string(row, "name", id), TRUE))))
			continue;
		struct visible_member item = { row, pending };

		g_array_append_val(g.rows, item);
	}
}

static void rebuild(void)
{
	g_array_set_size(g.rows, 0);
	if (g.pane == MEMBERS) {
		add_visible(ui_field(g.detail, "members"), FALSE);
		add_visible(ui_field(g.detail, "invitees"), TRUE);
	} else if (g.pane == INBOX) {
		add_visible(g.invitations, FALSE);
	} else if (g.pane == OFFER || g.pane == DECLINE) {
		if (g.offer) {
			struct visible_member item = { g.offer, FALSE };

			g_array_append_val(g.rows, item);
		}
	} else if (review()) {
		json_object_object_foreach(g.chosen, id, row) {
			(void)id;
			struct visible_member item = { row, FALSE };

			g_array_append_val(g.rows, item);
		}
	} else
		add_visible(g.friends, FALSE);
	g.cursor = CLAMP(g.cursor, 0, MAX(0, (int)g.rows->len - 1));
	g.first = CLAMP(g.first, 0, MAX(0, (int)g.rows->len - slots()));
	ui.dirty = TRUE;
}

static void switch_to(enum group_pane pane)
{
	g.pane = pane;
	g.first = g.cursor = 0;
	set(&g.notice, NULL);
	rebuild();
	focus(pane == CREATE ? NAME : pane == INVITE ? FILTER : ROWS);
}

static void manage_reset(void)
{
	g_free(manager.person);
	g_free(manager.person_name);
	json_object_put(manager.link);
	g_clear_pointer(&manager.qr, QRcode_free);
	memset(&manager, 0, sizeof(manager));
}

static void manage_page(enum manage_page page)
{
	manager.page = page;
	manager.focus = page == MG_EDIT ? -1 : 0;
	manager.count = 0;
	set(&g.notice, NULL);
	set(&ui.composition, "");
	ui_groups_resume_input();
}

static void manage_question(enum manage_action act, Json *person)
{
	if (person) {
		set(&manager.person, ui_string(person, "id", NULL));
		set(&manager.person_name,
		    ui_string(person, "name", manager.person));
	}
	manager.question = act;
	manage_page(MG_CONFIRM);
}

static Json *manage_params(void)
{
	Json *args = json_object_new_object();

	json_object_object_add(args, "chat", json_object_new_string(g.chat));
	return args;
}

static void manage_link_result(Json *result)
{
	json_object_put(manager.link);
	manager.link = json_object_get(result);
	g_clear_pointer(&manager.qr, QRcode_free);
	const char *url = ui_string(result, "url", NULL);

	if (url && *url)
		manager.qr = QRcode_encodeString8bit(url, 0, QR_ECLEVEL_M);
}

static void manage_back(void)
{
	if (g.writing)
		return;
	g.pending = 0;
	if (manager.page == MG_MENU ||
	    (manager.page == MG_CONFIRM && (manager.question == M_REMOVE ||
					    manager.question == M_WITHDRAW))) {
		manage_reset();
		set(&g.notice, NULL);
		focus(ROWS);
	} else if (manager.page == MG_CONFIRM &&
		   (manager.question == M_ENABLE ||
		    manager.question == M_DISABLE ||
		    manager.question == M_RENEW))
		manage_page(MG_LINK);
	else
		manage_page(MG_MENU);
}

static void manage_action(enum manage_action a)
{
	if (a == M_BACK) {
		manage_back();
		return;
	}
	if (g.pending)
		return;
	if (a == M_RENAME) {
		g_string_assign(g.name.value, ui_string(g.detail, "name", ""));
		g.name.cursor = g.name.value->len;
		g.name.anchor = 0;
		manage_page(MG_EDIT);
	} else if (a == M_LINK || a == M_REFRESH) {
		manage_page(MG_LINK);
		manage_link_result(NULL);
		rpc("groupLink", manage_params(), FALSE);
	} else if (a == M_SAVE) {
		if (!g.name.value->len) {
			set(&g.notice, _("Enter a group name."));
			return;
		}
		Json *args = manage_params();

		json_object_object_add(
			args, "name",
			json_object_new_string(g.name.value->str));
		rpc("renameGroup", args, TRUE);
	} else if (a == M_COPY) {
		const char *url = ui_string(manager.link, "url", NULL);

		if (url)
			set(&g.notice,
			    SDL_SetClipboardText(url) == 0 ?
				    _("Link copied.") :
				    _("Could not copy the group link."));
	} else if (a == M_CONFIRM && manager.page == MG_CONFIRM) {
		Json *args = manage_params();
		enum manage_action q = manager.question;
		const char *method;

		if (q == M_LEAVE)
			method = "leaveGroup";
		else if (q == M_RENEW)
			method = "renewGroupLink";
		else if (q == M_ENABLE || q == M_DISABLE) {
			method = "setGroupLink";
			json_object_object_add(
				args, "enabled",
				json_object_new_boolean(q == M_ENABLE));
		} else if ((q == M_REMOVE || q == M_WITHDRAW) &&
			   manager.person) {
			method = q == M_REMOVE ? "removeGroupMember" :
						 "cancelGroupInvite";
			json_object_object_add(
				args, "id",
				json_object_new_string(manager.person));
		} else {
			json_object_put(args);
			return;
		}
		rpc(method, args, TRUE);
	} else if (a == M_LEAVE || a == M_ENABLE || a == M_DISABLE ||
		   a == M_RENEW)
		manage_question(a, NULL);
}

static gboolean manage_event(SDL_Event *e)
{
	if (e->type == SDL_MOUSEBUTTONDOWN &&
	    e->button.button == SDL_BUTTON_LEFT) {
		if (manager.page == MG_EDIT &&
		    hit(g.name_box, e->button.x, e->button.y)) {
			manager.focus = -1;
			g.name.cursor = g.name.anchor =
				ui_edit_hit(&g.name, g.name_box, e->button.x);
			ui_groups_resume_input();
		} else
			for (int i = 0; i < manager.count; i++)
				if (hit(manager.buttons[i].rect, e->button.x,
					e->button.y)) {
					manager.focus = i;
					manage_action(
						manager.buttons[i].action);
					break;
				}
	} else if (!g.writing && manager.page == MG_EDIT && manager.focus < 0 &&
		   e->type == SDL_TEXTINPUT && g.name.value->len < 512)
		ui_insert(&g.name, e->text.text);
	else if (!g.writing && manager.page == MG_EDIT && manager.focus < 0 &&
		 e->type == SDL_TEXTEDITING)
		set(&ui.composition, e->edit.text);
	else if (e->type == SDL_KEYDOWN) {
		SDL_Keycode k = e->key.keysym.sym;
		gboolean editing = manager.page == MG_EDIT && manager.focus < 0;

		if (editing && ui.composition && *ui.composition)
			return TRUE;
		if (k == SDLK_ESCAPE)
			manage_back();
		else if (k == SDLK_TAB ||
			 (!editing && (k == SDLK_UP || k == SDLK_DOWN))) {
			int first = manager.page == MG_EDIT ? -1 : 0;
			int count = manager.count - first;
			int delta = (k == SDLK_UP ||
				     (k == SDLK_TAB &&
				      (e->key.keysym.mod & KMOD_SHIFT))) ?
					    -1 :
					    1;
			if (count)
				manager.focus = first + (manager.focus - first +
							 count + delta) %
								count;
			ui_groups_resume_input();
		} else if (!e->key.repeat &&
			   (k == SDLK_RETURN || k == SDLK_KP_ENTER ||
			    (!editing && k == SDLK_SPACE))) {
			if (editing)
				manage_action(M_SAVE);
			else if (manager.focus >= 0 &&
				 manager.focus < manager.count)
				manage_action(
					manager.buttons[manager.focus].action);
		} else if (editing && !g.writing)
			ui_edit_key(&g.name, e->key);
	}
	ui.dirty = TRUE;
	return e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP ||
	       e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEWHEEL ||
	       e->type == SDL_KEYDOWN || e->type == SDL_KEYUP ||
	       e->type == SDL_TEXTINPUT || e->type == SDL_TEXTEDITING;
}

static void manage_button(Rect r, const char *name, enum manage_action action,
			  gboolean primary)
{
	int n = manager.count++;

	manager.buttons[n].rect = r;
	manager.buttons[n].action = action;
	if (n == manager.focus)
		ui_draw_box((Rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 },
			    UI_ACCENT);
	ui_draw_button(r, name, primary);
}

static void manage_draw(void)
{
	int w = MIN(505, ui.width - 24), h = MIN(520, ui.height - 24),
	    left = (ui.width - w) / 2, top = (ui.height - h) / 2;
	int x = left + 18, y = top + 18, width = w - 36;
	const char *group = ui_string(g.detail, "name", "Group");

	manager.count = 0;
	ui_scrim();
	ui_draw_box((Rect){ left, top, w, h }, UI_DIALOG);
	const char *title =
		manager.page == MG_MENU	    ? _("Group settings") :
		manager.page == MG_EDIT	    ? _("Rename group") :
		manager.page == MG_LINK	    ? _("Invitation link and QR code") :
		manager.question == M_LEAVE ? _("Leave group?") :
		manager.question == M_REMOVE   ? _("Remove member?") :
		manager.question == M_WITHDRAW ? _("Cancel invitation?") :
		manager.question == M_ENABLE   ? _("Enable invitation link?") :
		manager.question == M_DISABLE  ? _("Disable invitation link?") :
		ui_string(manager.link, "url", NULL) ?
						_("Replace invitation link?") :
						_("Create invitation link?");
	ui_draw_label(title, (Rect){ x, y, width, 34 }, 21, 650, UI_HEADING,
		      FALSE);
	y += 40;
	ui_draw_label(group, (Rect){ x, y, width, 26 }, 15, 550, UI_MUTED,
		      FALSE);
	y += 38;
	manage_button((Rect){ x, top + h - 52, 96, 34 },
		      manager.page == MG_CONFIRM ? _("Cancel") : _("Back"),
		      M_BACK, FALSE);
	if (manager.page == MG_MENU) {
		manage_button((Rect){ x, y, width, 36 }, _("Rename group"),
			      M_RENAME, FALSE);
		manage_button((Rect){ x, y + 48, width, 36 },
			      _("Invitation link and QR code"), M_LINK, FALSE);
		manage_button((Rect){ x, y + 96, width, 36 }, _("Leave group…"),
			      M_LEAVE, FALSE);
	} else if (manager.page == MG_EDIT) {
		g.name_box = (Rect){ x, y, width, 40 };
		ui_draw_edit(&g.name, g.name_box, manager.focus < 0,
			     _("Group name"));
		manage_button((Rect){ x + width - 94, top + h - 52, 94, 34 },
			      _("Save"), M_SAVE, TRUE);
	} else if (manager.page == MG_LINK) {
		const char *url = ui_string(manager.link, "url", NULL);

		if (!manager.link) {
			if (g.pending)
				ui_draw_label(_("Loading…"),
					      (Rect){ x, y, width, 30 }, 15,
					      450, UI_MUTED, FALSE);
			else
				manage_button((Rect){ x, y, width, 36 },
					      _("Reload"), M_REFRESH, FALSE);
		} else if (!flag(manager.link, "allowed"))
			ui_draw_label(
				_("LINE has disabled link invitations for this group."),
				(Rect){ x, y, width, 52 }, 15, 450, UI_MUTED,
				TRUE);
		else if (!flag(manager.link, "enabled"))
			manage_button((Rect){ x, y, width, 36 },
				      _("Enable invitation link…"), M_ENABLE,
				      FALSE);
		else {
			if (url) {
				int bound = MIN(158, h - 285);

				if (manager.qr &&
				    bound >= manager.qr->width + 8) {
					ui_draw_code(manager.qr, left + w / 2,
						     y, bound);
					y += bound + 8;
				}
				ui_draw_label(url, (Rect){ x, y, width, 22 },
					      13, 450, UI_MUTED, FALSE);
				y += 30;
				manage_button((Rect){ x, y, width, 34 },
					      _("Copy link"), M_COPY, FALSE);
				y += 42;
			}
			int half = (width - 8) / 2;

			manage_button((Rect){ x, y, half, 34 },
				      url ? _("Replace link…") :
					    _("Create link…"),
				      M_RENEW, FALSE);
			manage_button((Rect){ x + half + 8, y, width - half - 8,
					      34 },
				      _("Disable link…"), M_DISABLE, FALSE);
		}
	} else {
		const char *verb =
			manager.question == M_LEAVE  ? _("Leave group") :
			manager.question == M_REMOVE ? _("Remove member") :
			manager.question == M_WITHDRAW ?
						       _("Cancel invitation") :
			manager.question == M_ENABLE  ? _("Enable link") :
			manager.question == M_DISABLE ? _("Disable link") :
			ui_string(manager.link, "url", NULL) ? "Replace link" :
							       _("Create link");
		g_autofree char *summary = NULL;

		if (manager.question == M_REMOVE ||
		    manager.question == M_WITHDRAW)
			summary = g_strdup_printf(
				manager.question == M_REMOVE ?
					_("Remove %s from this group?") :
					_("Withdraw the pending invitation for %s?"),
				manager.person_name);
		else if (manager.question == M_LEAVE)
			summary = g_strdup(_(
				"You will leave the conversation and stop receiving new group messages."));
		else if (manager.question == M_DISABLE)
			summary = g_strdup(_(
				"People using this link or QR code will no longer be able to join."));
		else if (manager.question == M_RENEW &&
			 ui_string(manager.link, "url", NULL))
			summary = g_strdup(_(
				"The earlier link and QR code will stop working. Anyone with the new link can join."));
		else
			summary = g_strdup(_(
				"Anyone with the invitation link or QR code can join this group."));
		ui_draw_label(summary, (Rect){ x, y, width, 98 }, 16, 450,
			      UI_BODY, TRUE);
		manage_button((Rect){ x + width - 148, top + h - 52, 148, 34 },
			      g.writing ? _("Updating…") : verb, M_CONFIRM,
			      TRUE);
	}
	if (g.notice)
		ui_draw_label(g.notice, (Rect){ x, top + h - 98, width, 40 },
			      13, 500, 0xf5bab6, TRUE);
}

static void toggle(int index)
{
	if (g.pane == MEMBERS && flag(g.detail, "canManage") && !g.pending &&
	    index >= 0 && (guint)index < g.rows->len) {
		struct visible_member item =
			g_array_index(g.rows, struct visible_member, index);
		if (g_strcmp0(ui.self_id, ui_string(item.row, "id", NULL)))
			manage_question(item.pending ? M_WITHDRAW : M_REMOVE,
					item.row);
		return;
	}
	if (g.pane == INBOX && index >= 0 && (guint)index < g.rows->len) {
		Json *row =
			g_array_index(g.rows, struct visible_member, index).row;
		json_object_put(g.offer);
		g.offer = json_object_get(row);
		switch_to(OFFER);
		return;
	}
	if (!form() || index < 0 || (guint)index >= g.rows->len)
		return;
	Json *row = g_array_index(g.rows, struct visible_member, index).row;
	const char *id = ui_string(row, "id", "");

	if (ui_field(g.chosen, id))
		json_object_object_del(g.chosen, id);
	else if (json_object_object_length(g.chosen) >= 499)
		set(&g.notice, _("Choose up to 499 friends."));
	else
		json_object_object_add(g.chosen, id, json_object_get(row));
	g.cursor = index;
	focus(ROWS);
}

static void primary(void)
{
	if (g.pending)
		return;
	if (g.pane == INBOX) {
		if (g.rows->len)
			toggle(g.cursor);
		else
			rpc("groupInvitations", json_object_new_object(),
			    FALSE);
		return;
	}
	if (g.pane == OFFER || g.pane == DECLINE) {
		Json *args = json_object_new_object();

		json_object_object_add(
			args, "chat",
			json_object_new_string(ui_string(g.offer, "id", "")));
		rpc(g.pane == OFFER ? "joinGroup" : "declineGroup", args, TRUE);
		return;
	}
	if (g.pane == MEMBERS) {
		if (!g.detail) {
			load_members();
			return;
		}
		if (!flag(g.detail, "canInvite"))
			return;
		switch_to(INVITE);
		if (!g.loaded)
			friends_page();
		return;
	}
	if (form()) {
		if (g.pane == CREATE) {
			g_autofree char *name = g_strdup(g.name.value->str);

			if (!*g_strstrip(name)) {
				set(&g.notice, _("Enter a group name."));
				focus(NAME);
				return;
			}
		} else if (!json_object_object_length(g.chosen)) {
			set(&g.notice, _("Choose at least one friend."));
			return;
		}
		switch_to(g.pane == CREATE ? REVIEW_CREATE : REVIEW_INVITE);
		return;
	}
	if (!review())
		return;
	Json *args = json_object_new_object(), *users = json_object_new_array();

	json_object_object_foreach(g.chosen, id, unused) {
		(void)unused;
		json_object_array_add(users, json_object_new_string(id));
	}
	json_object_object_add(args, "users", users);
	if (g.pane == REVIEW_CREATE) {
		json_object_object_add(
			args, "name",
			json_object_new_string(g.name.value->str));
		rpc("createGroup", args, TRUE);
	} else {
		json_object_object_add(args, "chat",
				       json_object_new_string(g.chat));
		rpc("inviteGroup", args, TRUE);
	}
}

static void back(void)
{
	if (g.writing)
		return;
	if (g.pane == OFFER)
		switch_to(INBOX);
	else if (g.pane == DECLINE)
		switch_to(OFFER);
	else if (g.pane == INBOX)
		switch_to(CREATE);
	else if (review())
		switch_to(g.pane == REVIEW_CREATE ? CREATE : INVITE);
	else if (g.pane == INVITE)
		switch_to(MEMBERS);
	else
		close_group();
}

static void move(int delta)
{
	g.cursor = CLAMP(g.cursor + delta, 0, MAX(0, (int)g.rows->len - 1));
	g.first = CLAMP(g.first, MAX(0, g.cursor - slots() + 1), g.cursor);
	ui.dirty = TRUE;
}

gboolean ui_groups_reply(const char *method, guint tag, Json *result,
			 const char *error)
{
	if (!(tag & 0x80000000u))
		return FALSE;
	gboolean management = !strcmp(method, "renameGroup") ||
			      !strcmp(method, "groupLink") ||
			      !strcmp(method, "setGroupLink") ||
			      !strcmp(method, "renewGroupLink") ||
			      !strcmp(method, "leaveGroup") ||
			      !strcmp(method, "removeGroupMember") ||
			      !strcmp(method, "cancelGroupInvite");
	if (!management && strcmp(method, "friends") &&
	    strcmp(method, "groupMembers") && strcmp(method, "createGroup") &&
	    strcmp(method, "inviteGroup") &&
	    strcmp(method, "groupInvitations") && strcmp(method, "joinGroup") &&
	    strcmp(method, "declineGroup"))
		return FALSE;
	if (tag != g.pending || !ui_groups_active())
		return TRUE;
	g.pending = 0;
	g.writing = FALSE;
	if (error) {
		if (management && manager.page == MG_CONFIRM) {
			manage_back();
			/* A lost receipt can follow a completed write. */
			if (manager.page == MG_LINK) {
				manage_link_result(NULL);
				rpc("groupLink", manage_params(), FALSE);
			}
		} else if (review())
			switch_to(g.pane == REVIEW_CREATE ? CREATE : INVITE);
		set(&g.notice, error);
		return TRUE;
	}
	if (management) {
		if (!strcmp(method, "groupLink") ||
		    !strcmp(method, "renewGroupLink")) {
			manage_page(MG_LINK);
			manage_link_result(result);
		} else if (!strcmp(method, "setGroupLink")) {
			manage_page(MG_LINK);
			manage_link_result(result);
			if (flag(result, "enabled"))
				rpc("groupLink", manage_params(), FALSE);
		} else if (!strcmp(method, "leaveGroup")) {
			g_autofree char *id = g_strdup(g.chat);

			close_group();
			ui_group_left(id);
		} else {
			if (!strcmp(method, "renameGroup"))
				ui_update_friend_chat(result);
			manage_reset();
			switch_to(MEMBERS);
			load_members();
			ui_refresh();
		}
		return TRUE;
	}
	if (!strcmp(method, "friends")) {
		Json *list = ui_field(result, "friends");

		for (guint i = 0; i < size(list); i++)
			json_object_array_add(
				g.friends,
				json_object_get(
					json_object_array_get_idx(list, i)));
		g.loaded = TRUE;
		set(&g.next, ui_string(result, "next", NULL));
		rebuild();
	} else if (!strcmp(method, "groupInvitations")) {
		json_object_put(g.invitations);
		g.invitations = json_object_get(result);
		if (g.pane == INBOX)
			rebuild();
	} else if (!strcmp(method, "joinGroup")) {
		Json *row = json_object_get(g.offer);

		close_group();
		ui_open_conversation(row);
		json_object_put(row);
		ui_refresh();
	} else if (!strcmp(method, "declineGroup")) {
		json_object_put(g.offer);
		g.offer = NULL;
		json_object_put(g.invitations);
		g.invitations = NULL;
		switch_to(INBOX);
		rpc("groupInvitations", json_object_new_object(), FALSE);
	} else if (!strcmp(method, "groupMembers")) {
		json_object_put(g.detail);
		g.detail = json_object_get(result);
		json_object_put(g.unavailable);
		g.unavailable = json_object_new_object();
		switch_to(MEMBERS);
		for (guint i = 0; i < g.rows->len; i++) {
			const char *id = ui_string(
				g_array_index(g.rows, struct visible_member, i)
					.row,
				"id", "");
			json_object_object_add(g.unavailable, id,
					       json_object_new_boolean(TRUE));
		}
	} else if (!strcmp(method, "createGroup")) {
		close_group();
		ui_open_conversation(result);
		ui_refresh();
	} else {
		json_object_put(g.chosen);
		g.chosen = json_object_new_object();
		switch_to(MEMBERS);
		load_members();
		ui_refresh();
	}
	return TRUE;
}

static void cycle(int direction)
{
	enum group_control order[8];
	int n = 0;

	if (g.pane == CREATE ||
	    (g.pane == MEMBERS && flag(g.detail, "canManage")))
		order[n++] = INCOMING;
	if (g.pane == OFFER)
		order[n++] = REFUSE;
	if (g.pane == CREATE)
		order[n++] = NAME;
	if (form())
		order[n++] = FILTER;
	order[n++] = ROWS;
	if (form() && (g.next || !g.loaded))
		order[n++] = MORE;
	order[n++] = BACK;
	order[n++] = PRIMARY;
	int pos = 0;

	while (pos < n && order[pos] != g.focus)
		pos++;
	focus(order[(pos + direction + n) % n]);
}

static void show_inbox(void)
{
	if (g.writing)
		return;
	/* Supersede a read-only roster load when switching views. */
	g.pending = 0;
	switch_to(INBOX);
	rpc("groupInvitations", json_object_new_object(), FALSE);
}

gboolean ui_groups_event(SDL_Event *e)
{
	if (!ui_groups_active())
		return FALSE;
	if (manager.page != MG_CLOSED)
		return manage_event(e);
	Editor *editor = focused_editor();

	if (e->type == SDL_KEYDOWN) {
		SDL_Keycode k = e->key.keysym.sym;

		if (ui.composition && *ui.composition && editor)
			return TRUE;
		if (k == SDLK_ESCAPE)
			back();
		else if (k == SDLK_TAB)
			cycle((e->key.keysym.mod & KMOD_SHIFT) ? -1 : 1);
		else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER ||
			  (k == SDLK_SPACE && !editor)) &&
			 !e->key.repeat) {
			if (g.focus == BACK)
				back();
			else if (g.focus == INCOMING && g.pane == MEMBERS &&
				 !g.pending && flag(g.detail, "canManage"))
				manage_page(MG_MENU);
			else if (g.focus == INCOMING && g.pane == CREATE)
				show_inbox();
			else if (g.focus == REFUSE && g.pane == OFFER &&
				 !g.pending)
				switch_to(DECLINE);
			else if (g.focus == MORE && !g.pending)
				friends_page();
			else if (g.focus == PRIMARY)
				primary();
			else if (g.focus == ROWS)
				toggle(g.cursor);
			else
				focus(ROWS);
		} else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_PAGEUP ||
			   k == SDLK_PAGEDOWN) {
			focus(ROWS);
			move(k == SDLK_UP     ? -1 :
			     k == SDLK_DOWN   ? 1 :
			     k == SDLK_PAGEUP ? -slots() :
						slots());
		} else if (editor) {
			ui_edit_key(editor, e->key);
			if (editor == &g.filter) {
				g.first = g.cursor = 0;
				rebuild();
			}
		}
		return TRUE;
	}
	if (e->type == SDL_TEXTINPUT || e->type == SDL_TEXTEDITING) {
		if (editor && e->type == SDL_TEXTEDITING)
			set(&ui.composition, e->edit.text);
		else if (editor && editor->value->len < 512) {
			ui_insert(editor, e->text.text);
			if (editor == &g.filter) {
				g.first = g.cursor = 0;
				rebuild();
			}
		}
		return TRUE;
	}
	if (e->type == SDL_MOUSEWHEEL) {
		int d = e->wheel.y *
			(e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? 1 : -1);
		g.first = CLAMP(g.first + 3 * d, 0,
				MAX(0, (int)g.rows->len - slots()));
		ui.dirty = TRUE;
		return TRUE;
	}
	if (e->type == SDL_MOUSEBUTTONDOWN &&
	    e->button.button == SDL_BUTTON_LEFT) {
		int x = e->button.x, y = e->button.y;

		if (hit(g.back, x, y))
			back();
		else if (g.pane == MEMBERS && flag(g.detail, "canManage") &&
			 !g.pending && hit(g.incoming, x, y))
			manage_page(MG_MENU);
		else if (g.pane == CREATE && hit(g.incoming, x, y))
			show_inbox();
		else if (g.pane == OFFER && hit(g.refuse, x, y) && !g.pending)
			switch_to(DECLINE);
		else if (hit(g.primary, x, y)) {
			focus(PRIMARY);
			primary();
		} else if (form() && hit(g.more, x, y) && !g.pending &&
			   (g.next || !g.loaded))
			friends_page();
		else if (g.pane == CREATE && hit(g.name_box, x, y)) {
			focus(NAME);
			g.name.cursor = g.name.anchor =
				ui_edit_hit(&g.name, g.name_box, x);
		} else if (form() && hit(g.filter_box, x, y)) {
			focus(FILTER);
			g.filter.cursor = g.filter.anchor =
				ui_edit_hit(&g.filter, g.filter_box, x);
		} else if (hit(g.list, x, y)) {
			int local = (y - g.list.y) / 40,
			    index = g.first + local;
			if (local < slots() && (guint)index < g.rows->len) {
				g.cursor = index;
				focus(ROWS);
				toggle(index);
			}
		}
		return TRUE;
	}
	return e->type == SDL_MOUSEBUTTONUP || e->type == SDL_MOUSEMOTION ||
	       e->type == SDL_KEYUP || e->type == SDL_MOUSEBUTTONDOWN;
}

static void button(Rect r, const char *label, enum group_control item,
		   gboolean filled)
{
	if (g.focus == item)
		ui_draw_box((Rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 },
			    UI_ACCENT);
	ui_draw_button(r, label, filled);
}

void ui_groups_overlay(void)
{
	if (!ui_groups_active())
		return;
	if (manager.page != MG_CLOSED) {
		manage_draw();
		return;
	}
	int w = MIN(540, ui.width - 24), h = MIN(580, ui.height - 24);
	int left = (ui.width - w) / 2, top = (ui.height - h) / 2, x = left + 18,
	    y = top + 18, width = w - 36;
	g.panel = (Rect){ left, top, w, h };
	ui_scrim();
	ui_draw_box(g.panel, UI_DIALOG);
	const char *title = g.pane == MEMBERS	    ? _("Group members") :
			    g.pane == CREATE	    ? _("New group") :
			    g.pane == INVITE	    ? _("Invite friends") :
			    g.pane == INBOX	    ? _("Group invitations") :
			    g.pane == OFFER	    ? _("Group invitation") :
			    g.pane == DECLINE	    ? _("Decline invitation?") :
			    g.pane == REVIEW_CREATE ? _("Create this group?") :
						      _("Send invitations?");
	ui_draw_label(
		title,
		(Rect){ x, y,
			(g.pane == CREATE ||
			 (g.pane == MEMBERS && flag(g.detail, "canManage"))) ?
				width - 120 :
				width,
			32 },
		22, 650, UI_HEADING, FALSE);
	if (g.pane == CREATE ||
	    (g.pane == MEMBERS && flag(g.detail, "canManage"))) {
		g.incoming = (Rect){ x + width - 106, y - 2, 106, 31 };
		button(g.incoming,
		       g.pane == CREATE ? _("Invitations") : _("Settings"),
		       INCOMING, FALSE);
	}
	y += 42;
	if (g.pane == CREATE) {
		g.name_box = (Rect){ x, y, width, 38 };
		ui_draw_edit(&g.name, g.name_box, g.focus == NAME,
			     _("Group name"));
		y += 47;
	}
	if (form()) {
		g.filter_box = (Rect){ x, y, width, 36 };
		ui_draw_edit(&g.filter, g.filter_box, g.focus == FILTER,
			     _("Search friends"));
		y += 44;
	}
	int chosen = json_object_object_length(g.chosen);
	g_autofree char *heading = NULL;

	if (g.pane == MEMBERS)
		heading = g_strdup_printf(_("%s · %u members · %u pending"),
					  ui_string(g.detail, "name", "Group"),
					  size(ui_field(g.detail, "members")),
					  size(ui_field(g.detail, "invitees")));
	else if (g.pane == INBOX)
		heading = g_strdup(_("Invitations to join"));
	else if (g.pane == OFFER || g.pane == DECLINE)
		heading = g_strdup(_("Invited group"));
	else if (review())
		heading = g_strdup_printf(_("%s · %d invitation%s"),
					  g.pane == REVIEW_CREATE ?
						  g.name.value->str :
						  ui_string(g.detail, "name",
							    _("Group")),
					  chosen, chosen == 1 ? "" : "s");
	else
		heading = g_strdup_printf(_("%d selected"), chosen);
	ui_draw_label(heading, (Rect){ x, y, width, 24 }, 14, 550, UI_MUTED,
		      FALSE);
	y += 28;
	g.back = (Rect){ x, top + h - 54, 92, 36 };
	g.primary = (Rect){ x + width - 144, top + h - 54, 144, 36 };
	g.more = (Rect){ x, top + h - 96, 122, 30 };
	gboolean more = form() && (g.next || !g.loaded);
	int list_bottom = top + h - (more || g.notice ? 104 : 66);

	g.list = (Rect){ x, y, width, MAX(40, list_bottom - y) };
	if (g.rows->len) {
		for (int i = 0;
		     i < slots() && (guint)(g.first + i) < g.rows->len; i++) {
			struct visible_member item = g_array_index(
				g.rows, struct visible_member, g.first + i);
			const char *id = ui_string(item.row, "id", "");
			gboolean picked = ui_field(g.chosen, id) != NULL;
			Rect r = { x, y + i * 40, width, 38 };

			if (g.focus == ROWS && g.cursor == g.first + i)
				ui_draw_box(r, UI_SELECTED);
			if (form())
				ui_draw_label(picked ? "☑" : "☐",
					      (Rect){ x + 4, r.y + 6, 29, 26 },
					      18, 550,
					      picked ? UI_ACCENT : UI_MUTED,
					      FALSE);
			int inset = form() ? 36 : 6;

			ui_draw_label(
				ui_string(item.row, "name", id),
				(Rect){ x + inset, r.y + 7,
					width - inset - (item.pending ? 70 : 3),
					25 },
				15, 500, UI_HEADING, FALSE);
			if (item.pending)
				ui_draw_label(_("Pending"),
					      (Rect){ x + width - 68, r.y + 7,
						      68, 25 },
					      13, 450, UI_MUTED, FALSE);
		}
	} else {
		const char *empty =
			g.pending	  ? _("Loading…") :
			review()	  ? _("Only you") :
			g.pane == MEMBERS ? _("No members listed") :
			g.pane == INBOX	  ? _("No pending group invitations") :
					    _("No matching friends");
		ui_draw_label(empty, g.list, 15, 450, UI_MUTED, FALSE);
	}
	if (more)
		button(g.more,
		       g.pending ? _("Loading…") :
		       g.loaded	 ? _("More friends") :
				   _("Load friends"),
		       MORE, FALSE);
	if (g.notice)
		ui_draw_label(g.notice,
			      (Rect){ more ? x + 130 : x, top + h - 100,
				      more ? width - 130 : width, 40 },
			      13, 500, 0xf5bab6, TRUE);
	button(g.back,
	       review() || g.pane == INVITE || g.pane == INBOX ||
			       g.pane == OFFER || g.pane == DECLINE ?
		       _("Back") :
		       _("Close"),
	       BACK, FALSE);
	if (g.pane == OFFER) {
		g.refuse = (Rect){ x + 100, g.back.y, 82, 36 };
		button(g.refuse, _("Decline"), REFUSE, FALSE);
		g.primary = (Rect){ x + width - 90, g.back.y, 90, 36 };
	}
	const char *action =
		g.writing ?
			_("Sending…") :
		g.pane == MEMBERS ?
			(g.detail ? _("Invite friends") : _("Reload")) :
		g.pane == INBOX ?
			(g.rows->len ? _("View invitation") : _("Refresh")) :
		g.pane == OFFER		? _("Join") :
		g.pane == DECLINE	? _("Decline") :
		g.pane == REVIEW_CREATE ? _("Create group") :
		g.pane == REVIEW_INVITE ? _("Send invitations") :
					  _("Review");
	if (g.pane != MEMBERS || !g.detail || flag(g.detail, "canInvite"))
		button(g.primary, action, PRIMARY, review());
}
