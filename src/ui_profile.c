// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_profile.h"
#include "ui_sounds.h"
#include "ui_chat_lists.h"
#include "ui_friends.h"
#include "ui_motion.h"
#include "ui_notifications.h"
#include <string.h>

/* Dark surfaces matching ui_draw.c. */
enum {
	HEADING = 0xf2f3f5,
	INK = 0xdbdee1,
	MUTED = 0x949ba4,
	FAINT = 0x6d6f78,
	POPOUT = 0x162126,
	POPOUT_EDGE = 0x2e2e34,
	INNER = 0x111214,
	FIELD = 0x1e1f22,
	NAV = 0x182429,
	CONTENT = 0x1e2b30,
	NAV_HOVER = 0x29393e,
	NAV_SELECTED = 0x3a3b42,
	ACCENT = 0x267d72,
	GREEN = 0x248046,
	DANGER = 0xda373c,
	CHIP = 0x2b2d31,
	CARD_W = 300,
	INBOX_W = 380,
	TOAST_W = 320,
	TOAST_H = 64,
	TOAST_MS = 5000,
	MAX_CONTROLS = 64,
	/* Tags well above the other modules' serials. */
	TAG_BASE = 0x40000000
};

enum view {
	NONE,
	CARD,
	SETTINGS,
	INBOX
};

enum section {
	MY_ACCOUNT,
	PROFILES,
	PRIVACY,
	NOTIFICATIONS,
	KEYBINDS,
	APPEARANCE,
	N_SECTIONS
};

enum action {
	A_NOTHING,
	A_CLOSE,
	A_MESSAGE,
	A_ADD,
	A_BLOCK,
	A_UNBLOCK,
	A_EDIT_PROFILE,
	A_NAV,
	A_LOGOUT,
	A_FIELD,
	A_PICTURE,
	A_SAVE,
	A_RESET,
	A_VIEW,
	A_OPEN_INBOX,
	A_NOTIFY,
	A_SOUNDS,
	A_MOTION,
	A_MOTION_SYSTEM,
	A_KEYS,
	A_ACCEPT,
	A_DECLINE,
	A_ADD_BACK,
	A_PERSON
};

struct control {
	Rect r;
	enum action action;
	int arg;
};

static const char *const section_names[] = {
	[MY_ACCOUNT] = "My Account",	[PROFILES] = "Profiles",
	[PRIVACY] = "Privacy & Safety", [NOTIFICATIONS] = "Notifications",
	[KEYBINDS] = "Keybinds",	[APPEARANCE] = "Appearance",
};

static struct {
	enum view view;
	guint serial;
	/* Profile popout. */
	Json *card;
	char *chat, *note;
	Rect anchor, panel;
	guint card_tag, action_tag;
	gboolean busy;
	/* Account. */
	Json *me;
	guint me_tag, save_tag, picture_tag;
	enum section section;
	Editor name, status;
	int editing; /* 0 none, 1 name, 2 status */
	char *settings_note;
	int scroll, content_h;
	/* Inbox. */
	Json *requests, *adds;
	guint requests_tag, answer_tag;
	char *answer_id, *inbox_note;
	gboolean requests_loaded;
	int unseen;
	/* Toast. */
	Json *toast;
	gint64 toast_until;
	Rect toast_rect;
	Uint32 wake_event;
	/* Hit targets of the last frame. */
	struct control controls[MAX_CONTROLS];
	int ncontrols, hover;
	Rect self_area, inbox_button, gear_button, user_panel;
} p = { .hover = -1 };

static guint next_tag(void)
{
	return TAG_BASE + ++p.serial;
}

static void set_text(char **dst, const char *s)
{
	char *copy = g_strdup(s);

	g_free(*dst);
	*dst = copy;
	ui.dirty = TRUE;
}

static void set_json(Json **dst, Json *v)
{
	Json *held = v ? json_object_get(v) : NULL;

	json_object_put(*dst);
	*dst = held;
	ui.dirty = TRUE;
}

static gboolean flag(Json *row, const char *key)
{
	return json_object_get_boolean(ui_field(row, key));
}

static gboolean hit(Rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void control(Rect r, enum action action, int arg)
{
	if (p.ncontrols < MAX_CONTROLS)
		p.controls[p.ncontrols++] = (struct control){ r, action, arg };
}

static int control_at(int x, int y)
{
	/* Later controls sit above earlier ones. */
	for (int i = p.ncontrols - 1; i >= 0; i--)
		if (hit(p.controls[i].r, x, y))
			return i;
	return -1;
}

static void editor_text(Editor *e, const char *s)
{
	if (!e->value)
		e->value = g_string_new("");
	g_string_assign(e->value, s ? s : "");
	e->cursor = e->anchor = (int)e->value->len;
	e->scroll = 0;
}

static const char *editor_value(Editor *e)
{
	return e->value ? e->value->str : "";
}

static void stop_editing(void)
{
	if (p.editing)
		SDL_StopTextInput();
	p.editing = 0;
}

static void close_view(void)
{
	stop_editing();
	p.view = NONE;
	p.busy = FALSE;
	p.ncontrols = 0;
	p.hover = -1;
	g_clear_pointer(&p.note, g_free);
	ui_restore_input();
	ui.dirty = TRUE;
}

gboolean ui_profile_active(void)
{
	return p.view != NONE;
}

int ui_profile_layer(void)
{
	return p.view == CARD	  ? UI_LAYER_POPOUT :
	       p.view == SETTINGS ? UI_LAYER_SETTINGS :
	       p.view == INBOX	  ? UI_LAYER_INBOX :
				    0;
}

static Json *params_id(const char *id)
{
	Json *o = json_object_new_object();

	json_object_object_add(o, "id", json_object_new_string(id ? id : ""));
	return o;
}

/* ----- Profile popout ----- */

static void request_card(const char *id)
{
	p.card_tag = next_tag();
	ui_rpc("profileCard", params_id(id), p.card_tag);
}

void ui_profile_open(Json *person, const char *chat, Rect anchor)
{
	const char *id = ui_string(person, "id", NULL);

	if (!id || !*id || !ui.connected)
		return;
	stop_editing();
	Json *seed = json_object_new_object();

	/* Show what the window already knows at once, then fill in. */
	json_object_object_add(seed, "id", json_object_new_string(id));
	json_object_object_add(
		seed, "name",
		json_object_new_string(ui_string(person, "name", id)));
	json_object_object_add(
		seed, "avatarKey",
		json_object_new_string(ui_string(person, "avatarKey", "")));
	if (!g_strcmp0(id, ui.self_id)) {
		json_object_object_add(seed, "self",
				       json_object_new_boolean(TRUE));
		if (p.me)
			json_object_object_add(seed, "status",
					       json_object_new_string(ui_string(
						       p.me, "status", "")));
	}
	set_json(&p.card, seed);
	json_object_put(seed);
	json_object_object_add(p.card, "loading",
			       json_object_new_boolean(TRUE));
	set_text(&p.chat, chat);
	g_clear_pointer(&p.note, g_free);
	p.anchor = anchor;
	p.panel = (Rect){ 0 };
	p.busy = FALSE;
	p.view = CARD;
	SDL_StopTextInput();
	request_card(id);
}

/* Discord tints the banner with the avatar's color. */
static guint banner_color(Json *row)
{
	const char *id = ui_string(row, "id", "");
	const char *key = ui_string(row, "avatarKey", NULL);
	cairo_surface_t *s = *id ? ui_avatar(ui.selected ? ui.selected : id, id,
					     key && *key ? key : NULL) :
				   NULL;

	if (s && cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE &&
	    cairo_image_surface_get_format(s) == CAIRO_FORMAT_ARGB32) {
		cairo_surface_flush(s);
		int w = cairo_image_surface_get_width(s),
		    h = cairo_image_surface_get_height(s),
		    stride = cairo_image_surface_get_stride(s);
		const guint8 *px = cairo_image_surface_get_data(s);
		guint64 sum[3] = { 0 }, n = 0;
		int step = MAX(1, MIN(w, h) / 24);

		for (int y = 0; px && y < h; y += step)
			for (int x = 0; x < w; x += step) {
				guint32 v = *(const guint32 *)(px + y * stride +
							       x * 4);
				if ((v >> 24) < 128)
					continue;
				sum[0] += (v >> 16) & 0xff;
				sum[1] += (v >> 8) & 0xff;
				sum[2] += v & 0xff;
				n++;
			}
		if (n)
			return (guint)((sum[0] / n) << 16 | (sum[1] / n) << 8 |
				       sum[2] / n);
	}
	return ui_avatar_shade(id);
}

static void chip(int *x, int y, const char *label, guint ink)
{
	int tw = ui_text_width(label, 12, 600) + 16;
	Rect r = { *x, y, tw, 20 };

	ui_draw_rounded(r, 4, CHIP);
	ui_draw_centered(label, r, 12, 600, ink);
	*x += tw + 6;
}

static void button(Rect r, const char *label, guint fill, guint ink,
		   enum action action, int arg)
{
	double h = ui_draw_hover(r);

	ui_draw_rounded(r, 4, ui_blend(fill, ui_blend(fill, 0xffffff, .14), h));
	ui_draw_centered(label, r, 14, 500, ink);
	control(r, action, arg);
}

/*
 * The popout and the profile preview share this card. Height depends on
 * the status text; *height reports it for placement.
 */
static int draw_card(Json *row, int x, int y, int w, gboolean actions)
{
	const char *name = ui_string(row, "name", "");
	const char *status = ui_string(row, "status", "");
	gboolean self = flag(row, "self");
	int status_h =
		*status ? MIN(120, ui_text_height(status, w - 56, 14, 400)) : 0;
	const char *alias = ui_string(row, "overrideName", "");
	const char *profile = ui_string(row, "profileName", "");
	const char *line_id = ui_string(row, "lineId", "");
	const char *second = *alias && *profile && g_strcmp0(alias, profile) ?
				     profile :
			     *line_id ? line_id :
					NULL;
	gboolean chips = self || flag(row, "friend") || flag(row, "favorite") ||
			 flag(row, "official") || flag(row, "blocked");
	int info_y = 108 + 32 + (second ? 22 : 0) + (chips ? 28 : 0) + 8;
	int inner_h = 46 + (*status ? status_h : 20);
	int h = info_y + inner_h + 12 + (actions ? 44 : 0) +
		(p.note && actions ? 22 : 0);

	ui_draw_rounded((Rect){ x - 1, y - 1, w + 2, h + 2 }, 9, POPOUT_EDGE);
	ui_draw_rounded((Rect){ x, y, w, h }, 8, POPOUT);
	ui_draw_rounded((Rect){ x, y, w, 60 }, 8, banner_color(row));
	ui_draw_box((Rect){ x, y + 52, w, 8 }, banner_color(row));
	ui_draw_rounded((Rect){ x + 12, y + 16, 88, 88 }, 44, POPOUT);
	ui_draw_profile(row, (Rect){ x + 16, y + 20, 80, 80 });
	if (self) {
		ui_draw_rounded((Rect){ x + 72, y + 76, 24, 24 }, 12, POPOUT);
		ui_draw_rounded((Rect){ x + 76, y + 80, 16, 16 }, 8, 0x23a55a);
	}
	int ty = y + 108;

	ui_draw_label(name, (Rect){ x + 16, ty, w - 32, 28 }, 20, 700, HEADING,
		      FALSE);
	if (second)
		ui_draw_label(second, (Rect){ x + 16, ty + 28, w - 32, 20 }, 14,
			      450, INK, FALSE);
	int cx = x + 16, cy = ty + (second ? 54 : 32);

	if (self)
		chip(&cx, cy, "You", INK);
	if (flag(row, "friend") && !flag(row, "blocked"))
		chip(&cx, cy, "Friend", INK);
	if (flag(row, "favorite"))
		chip(&cx, cy, "Favorite", 0xf0b232);
	if (flag(row, "official"))
		chip(&cx, cy, "Official", 0x267d72);
	if (flag(row, "blocked"))
		chip(&cx, cy, "Blocked", DANGER);
	Rect inner = { x + 12, y + info_y, w - 24, inner_h };

	ui_draw_rounded(inner, 8, INNER);
	ui_draw_label(self ? "STATUS MESSAGE" : "ABOUT ME",
		      (Rect){ inner.x + 12, inner.y + 12, inner.w - 24, 16 },
		      12, 700, HEADING, FALSE);
	if (*status)
		ui_draw_label(status,
			      (Rect){ inner.x + 12, inner.y + 34, inner.w - 24,
				      status_h + 4 },
			      14, 400, INK, TRUE);
	else
		ui_draw_label(flag(row, "loading") ? "Loading profile…" :
						     "No status message yet.",
			      (Rect){ inner.x + 12, inner.y + 34, inner.w - 24,
				      20 },
			      14, 400, MUTED, FALSE);
	if (!actions)
		return h;
	int by = inner.y + inner.h + 12;

	if (p.note) {
		ui_draw_label(p.note, (Rect){ x + 16, by, w - 32, 20 }, 13, 450,
			      0xfa777c, FALSE);
		by += 22;
	}
	int bw = w - 24, bx = x + 12;
	const char *busy = p.busy ? "Working…" : NULL;

	if (self) {
		button((Rect){ bx, by, bw, 32 }, "Edit Profile", ACCENT,
		       0xffffff, A_EDIT_PROFILE, 0);
	} else if (flag(row, "loading")) {
		button((Rect){ bx, by, bw, 32 }, "Message", ACCENT, 0xffffff,
		       A_MESSAGE, 0);
	} else {
		gboolean add = flag(row, "canAdd") && !flag(row, "official");
		gboolean friend = flag(row, "friend");
		int second_w = friend || add ? (bw - 8) / 2 : 0;

		button((Rect){ bx, by, bw - second_w - (second_w ? 8 : 0), 32 },
		       busy ? busy : "Message", ACCENT, 0xffffff, A_MESSAGE, 0);
		if (add)
			button((Rect){ bx + bw - second_w, by, second_w, 32 },
			       "Add Friend", GREEN, 0xffffff, A_ADD, 0);
		else if (friend)
			button((Rect){ bx + bw - second_w, by, second_w, 32 },
			       flag(row, "blocked") ? "Unblock" : "Block", CHIP,
			       flag(row, "blocked") ? INK : 0xfa777c,
			       flag(row, "blocked") ? A_UNBLOCK : A_BLOCK, 0);
	}
	return h;
}

static void draw_popout(void)
{
	int w = MIN(CARD_W, ui.width - 16);
	Rect a = p.anchor;
	int x = a.x + a.w + 12, y = a.y;

	if (a.y < UI_HEADER_H) {
		/* From the chat header: drop below it. */
		x = a.x;
		y = a.y + a.h + 8;
	} else if (x + w > ui.width - 8)
		x = a.x - w - 12;
	x = CLAMP(x, 8, MAX(8, ui.width - w - 8));
	/* Measure once at the anchor, then clamp into the window. */
	int h = p.panel.h ? p.panel.h : 360;

	y = CLAMP(y, 8, MAX(8, ui.height - h - 8));
	p.panel = (Rect){ x, y, w, h };
	p.panel.h = draw_card(p.card, x, y, w, TRUE);
	control(p.panel, A_NOTHING, 0);
	/* Buttons were registered first; keep them above the panel. */
	struct control last = p.controls[p.ncontrols - 1];

	memmove(p.controls + 1, p.controls,
		(p.ncontrols - 1) * sizeof(*p.controls));
	p.controls[0] = last;
}

static void card_action(enum action action)
{
	const char *id = ui_string(p.card, "id", NULL);

	if (!id || p.busy)
		return;
	if (action == A_EDIT_PROFILE) {
		ui_settings_open();
		p.section = PROFILES;
		return;
	}
	Json *params = params_id(id);
	const char *method = "openProfileChat";

	if (action == A_ADD) {
		method = "addProfileFriend";
		if (p.chat)
			json_object_object_add(params, "chat",
					       json_object_new_string(p.chat));
	} else if (action == A_BLOCK || action == A_UNBLOCK) {
		method = "setProfileBlocked";
		json_object_object_add(params, "blocked",
				       json_object_new_boolean(action ==
							       A_BLOCK));
	}
	p.busy = TRUE;
	g_clear_pointer(&p.note, g_free);
	p.action_tag = next_tag();
	ui_rpc(method, params, p.action_tag);
	ui.dirty = TRUE;
}

/* ----- Settings ----- */

static void load_form(void)
{
	editor_text(&p.name, ui_string(p.me, "name", ui.name ? ui.name : ""));
	editor_text(&p.status, ui_string(p.me, "status", ""));
}

static gboolean form_changed(void)
{
	return p.me &&
	       (g_strcmp0(editor_value(&p.name), ui_string(p.me, "name", "")) ||
		g_strcmp0(editor_value(&p.status),
			  ui_string(p.me, "status", "")));
}

static void request_me(void)
{
	p.me_tag = next_tag();
	ui_rpc("myProfile", NULL, p.me_tag);
}

void ui_settings_open(void)
{
	if (!ui.connected)
		return;
	stop_editing();
	p.view = SETTINGS;
	p.scroll = 0;
	g_clear_pointer(&p.settings_note, g_free);
	load_form();
	request_me();
	SDL_StopTextInput();
	ui.dirty = TRUE;
}

static void focus_field(int which, int x, Rect r)
{
	p.editing = which;
	Editor *e = which == 1 ? &p.name : &p.status;

	if (r.w)
		e->cursor = e->anchor = ui_edit_hit(e, r, x);
	SDL_StartTextInput();
	ui.dirty = TRUE;
}

static void save_form(void)
{
	if (!form_changed() || p.save_tag)
		return;
	Json *changes = json_object_new_object();

	if (g_strcmp0(editor_value(&p.name), ui_string(p.me, "name", "")))
		json_object_object_add(
			changes, "name",
			json_object_new_string(editor_value(&p.name)));
	if (g_strcmp0(editor_value(&p.status), ui_string(p.me, "status", "")))
		json_object_object_add(
			changes, "status",
			json_object_new_string(editor_value(&p.status)));
	p.save_tag = next_tag();
	set_text(&p.settings_note, "Saving…");
	ui_rpc("updateMyProfile", changes, p.save_tag);
}

void ui_profile_picture_chosen(const char *path)
{
	if (!path || !ui.connected)
		return;
	Json *params = json_object_new_object();

	json_object_object_add(params, "path", json_object_new_string(path));
	p.picture_tag = next_tag();
	set_text(&p.settings_note, "Uploading your new avatar…");
	ui_rpc("updateMyPicture", params, p.picture_tag);
}

static void heading(const char *s, int x, int y, int w)
{
	ui_draw_label(s, (Rect){ x, y, w, 28 }, 20, 700, HEADING, FALSE);
}

static void caption(const char *s, int x, int y, int w)
{
	g_autofree char *up = g_utf8_strup(s, -1);

	ui_draw_label(up, (Rect){ x, y, w, 16 }, 12, 700, MUTED, FALSE);
}

static int setting_row(int x, int y, int w, const char *title,
		       const char *about, const char *label, enum action action,
		       int arg, gboolean toggle, gboolean on)
{
	int about_h = about ? ui_text_height(about, w - 120, 14, 400) : 0;
	int h = MAX(40, 24 + about_h + 4);

	ui_draw_label(title, (Rect){ x, y, w - 120, 22 }, 16, 500, HEADING,
		      FALSE);
	if (about)
		ui_draw_label(about, (Rect){ x, y + 24, w - 120, about_h + 2 },
			      14, 400, MUTED, TRUE);
	if (toggle) {
		Rect r = { x + w - 40, y + 2, 40, 24 };

		ui_draw_switch(r, on);
		control(r, action, arg);
	} else if (label) {
		int bw = MAX(96, ui_text_width(label, 14, 500) + 32);

		button((Rect){ x + w - bw, y + 2, bw, 32 }, label, CHIP, INK,
		       action, arg);
	}
	ui_draw_box((Rect){ x, y + h + 16, w, 1 }, 0x2e2f35);
	return h + 33;
}

static void field(int which, const char *title, int x, int y, int w)
{
	Editor *e = which == 1 ? &p.name : &p.status;
	Rect r = { x, y + 24, w, 40 };

	caption(title, x, y, w);
	ui_draw_rounded(r, 4, INNER);
	ui_draw_edit(e, (Rect){ r.x + 2, r.y + 2, r.w - 4, r.h - 4 },
		     p.editing == which,
		     which == 1 ? "Display name" : "What's on your mind?");
	control(r, A_FIELD, which);
}

static int account_section(int x, int y, int w)
{
	Json *row = p.me ? p.me : p.card;
	int top = y;

	heading("My Account", x, y, w);
	y += 44;
	Rect card = { x, y, w, 260 };

	ui_draw_rounded(card, 8, INNER);
	guint tint = banner_color(row);

	ui_draw_rounded((Rect){ x, y, w, 100 }, 8, tint);
	ui_draw_box((Rect){ x, y + 92, w, 8 }, tint);
	ui_draw_rounded((Rect){ x + 12, y + 72, 88, 88 }, 44, INNER);
	ui_draw_profile(row, (Rect){ x + 16, y + 76, 80, 80 });
	ui_draw_label(ui_string(row, "name", ui.name ? ui.name : ""),
		      (Rect){ x + 112, y + 110, w - 270, 28 }, 20, 700, HEADING,
		      FALSE);
	button((Rect){ x + w - 150, y + 112, 134, 32 }, "Edit User Profile",
	       ACCENT, 0xffffff, A_NAV, PROFILES);
	Rect info = { x + 16, y + 172, w - 32, 76 };

	ui_draw_rounded(info, 8, FIELD);
	caption("LINE ID", info.x + 16, info.y + 14, info.w / 2 - 24);
	const char *line_id = ui_string(row, "lineId", "");

	ui_draw_label(*line_id ? line_id : "Not set",
		      (Rect){ info.x + 16, info.y + 34, info.w / 2 - 24, 22 },
		      16, 400, *line_id ? INK : MUTED, FALSE);
	caption("Status message", info.x + info.w / 2, info.y + 14,
		info.w / 2 - 16);
	const char *status = ui_string(row, "status", "");

	ui_draw_label(*status ? status : "None",
		      (Rect){ info.x + info.w / 2, info.y + 34, info.w / 2 - 16,
			      22 },
		      16, 400, *status ? INK : MUTED, FALSE);
	y += 260 + 40;
	caption("Sign out", x, y, w);
	y += 24;
	y += setting_row(
		x, y, w, "Log out of this computer",
		"Ends this linked desktop session and removes its keys. Your phone stays signed in.",
		"Log Out", A_LOGOUT, 0, FALSE, FALSE);
	return y - top;
}

static int profiles_section(int x, int y, int w)
{
	int top = y;
	gboolean preview = w >= 620;
	int form_w = preview ? w - CARD_W - 32 : w;

	heading("Profiles", x, y, w);
	y += 44;
	caption("Avatar", x, y, form_w);
	button((Rect){ x, y + 24, 150, 32 }, "Change Avatar", ACCENT, 0xffffff,
	       A_PICTURE, 0);
	ui_draw_label("JPEG, PNG or WebP. LINE shows a centered square.",
		      (Rect){ x, y + 64, form_w, 20 }, 13, 400, MUTED, FALSE);
	y += 104;
	field(1, "Display name", x, y, form_w);
	y += 88;
	field(2, "Status message", x, y, form_w);
	y += 88;
	ui_draw_label("Names up to 20 characters, status up to 500.",
		      (Rect){ x, y - 16, form_w, 20 }, 13, 400, FAINT, FALSE);
	if (preview) {
		Json *row = json_object_new_object();

		json_object_object_add(
			row, "id",
			json_object_new_string(ui.self_id ? ui.self_id : ""));
		json_object_object_add(
			row, "name",
			json_object_new_string(editor_value(&p.name)));
		json_object_object_add(
			row, "status",
			json_object_new_string(editor_value(&p.status)));
		json_object_object_add(
			row, "avatarKey",
			json_object_new_string(
				ui_profile_self_avatar() ?
					ui_profile_self_avatar() :
					""));
		json_object_object_add(row, "self",
				       json_object_new_boolean(TRUE));
		caption("Preview", x + w - CARD_W, top + 44, CARD_W);
		draw_card(row, x + w - CARD_W, top + 68, CARD_W, FALSE);
		json_object_put(row);
	}
	return y - top + 40;
}

static int privacy_section(int x, int y, int w)
{
	int top = y;

	heading("Privacy & Safety", x, y, w);
	y += 52;
	y += setting_row(x, y, w, "Friend requests",
			 "Review incoming requests and people who added you.",
			 "Open Inbox", A_OPEN_INBOX, 0, FALSE, FALSE);
	y += setting_row(x, y, w, "Blocked",
			 "Blocked people can't message or call you on LINE.",
			 "View", A_VIEW, 2, FALSE, FALSE);
	y += setting_row(x, y, w, "Hidden friends",
			 "Hidden friends leave your list but stay friends.",
			 "View", A_VIEW, 1, FALSE, FALSE);
	y += setting_row(
		x, y, w, "Friend suggestions",
		"People LINE recommends, including some who added you.", "View",
		A_VIEW, 3, FALSE, FALSE);
	return y - top;
}

static int notifications_section(int x, int y, int w)
{
	int top = y;

	heading("Notifications", x, y, w);
	y += 52;
	y += setting_row(
		x, y, w, "Enable Desktop Notifications",
		"Messages, calls and friend activity while the window is in the background. Previews stay hidden.",
		NULL, A_NOTIFY, 0, TRUE, ui_notifications_enabled());
	y += setting_row(
		x, y, w, "Sounds",
		"Ringtone, call sounds and a chime for new messages, played on an emulated NES sound chip.",
		NULL, A_SOUNDS, 0, TRUE, ui_sounds_enabled());
	ui_draw_label(
		"To mute one conversation, right-click it in the list and choose Mute conversation.",
		(Rect){ x, y, w, 40 }, 14, 400, MUTED, TRUE);
	return y - top + 40;
}

static const char *const shortcuts[][2] = {
	{ "Open settings", "Ctrl+," },
	{ "Find a conversation", "Ctrl+F" },
	{ "Search this conversation", "Ctrl+Shift+F" },
	{ "Search recent messages", "Ctrl+Alt+F" },
	{ "Refresh", "Ctrl+R" },
	{ "Send message", "Enter" },
	{ "Close a dialog", "Escape" },
	{ "Switch to the call", "Alt+C" },
	{ "Mute or unmute", "Alt+M" },
	{ "Camera on or off", "Alt+V" },
	{ "Share your screen", "Alt+S" },
	{ "Accept a call", "Alt+A" },
	{ "Decline a call", "Alt+D" },
};

static int keybinds_section(int x, int y, int w)
{
	int top = y;

	heading("Keybinds", x, y, w);
	y += 52;
	y += setting_row(x, y, w, "Chat list shortcuts",
			 "Folders, pins, search and refresh can be rebound.",
			 "Customize", A_KEYS, 0, FALSE, FALSE);
	caption("Built in", x, y, w);
	y += 26;
	for (guint i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
		ui_draw_label(shortcuts[i][0], (Rect){ x, y + 6, w - 180, 22 },
			      15, 450, INK, FALSE);
		int kw = ui_text_width(shortcuts[i][1], 13, 600) + 20;
		Rect k = { x + w - kw, y + 2, kw, 28 };

		ui_draw_rounded(k, 4, FIELD);
		ui_draw_outline(k, 4, 0x3a3b42);
		ui_draw_centered(shortcuts[i][1], k, 13, 600, HEADING);
		y += 40;
	}
	return y - top;
}

static int appearance_section(int x, int y, int w)
{
	int top = y;

	heading("Appearance", x, y, w);
	y += 52;
	caption("Theme", x, y, w);
	Rect swatch = { x, y + 26, 60, 60 };

	ui_draw_rounded((Rect){ swatch.x - 3, swatch.y - 3, 66, 66 }, 33,
			ACCENT);
	ui_draw_rounded(swatch, 30, 0x313338);
	ui_draw_rounded((Rect){ swatch.x + 18, swatch.y + 18, 24, 24 }, 12,
			ACCENT);
	ui_draw_icon((Rect){ swatch.x + 18, swatch.y + 18, 24, 24 },
		     UI_ICON_CHECK, 0xffffff);
	ui_draw_label("Dark", (Rect){ x + 76, y + 46, 200, 22 }, 15, 500, INK,
		      FALSE);
	y += 116;
	caption("Accessibility", x, y, w);
	y += 28;
	int choice = ui_motion_choice();

	y += setting_row(
		x, y, w, "Reduce Motion",
		choice < 0 ?
			"Follows your desktop's animation setting." :
			"Set here for this account. Choose Use desktop setting to follow the desktop again.",
		NULL, A_MOTION, 0, TRUE, ui_reduce_motion());
	if (choice >= 0) {
		button((Rect){ x, y, 180, 32 }, "Use desktop setting", CHIP,
		       INK, A_MOTION_SYSTEM, 0);
		y += 48;
	}
	return y - top;
}

static void draw_settings(void)
{
	int W = ui.width, H = ui.height;
	int nav_area = CLAMP(W * 28 / 100, 190, 360);
	int nav_w = MIN(218, nav_area - 20), nav_x = nav_area - nav_w - 8;

	ui_draw_box((Rect){ 0, 0, nav_area, H }, NAV);
	ui_draw_box((Rect){ nav_area, 0, W - nav_area, H }, CONTENT);
	int y = 60;

	for (int s = 0; s < N_SECTIONS; s++) {
		if (s == MY_ACCOUNT || s == NOTIFICATIONS) {
			if (s)
				ui_draw_box((Rect){ nav_x + 10, y + 4,
						    nav_w - 20, 1 },
					    0x2e2f35);
			caption(s ? "App Settings" : "User Settings",
				nav_x + 10, y + (s ? 14 : 0), nav_w - 20);
			y += s ? 36 : 24;
		}
		Rect r = { nav_x, y, nav_w, 32 };
		double h = ui_draw_hover(r);

		if (p.section == (enum section)s)
			ui_draw_rounded(r, 4, NAV_SELECTED);
		else if (h > 0.)
			ui_draw_rounded(r, 4, ui_blend(NAV, NAV_HOVER, h));
		ui_draw_label(section_names[s],
			      (Rect){ r.x + 10, r.y + 6, r.w - 20, 22 }, 16,
			      500,
			      p.section == (enum section)s ? HEADING : MUTED,
			      FALSE);
		control(r, A_NAV, s);
		y += 34;
	}
	ui_draw_box((Rect){ nav_x + 10, y + 6, nav_w - 20, 1 }, 0x2e2f35);
	y += 14;
	Rect out = { nav_x, y, nav_w, 32 };
	double oh = ui_draw_hover(out);

	if (oh > 0.)
		ui_draw_rounded(out, 4, ui_blend(NAV, NAV_HOVER, oh));
	ui_draw_label("Log Out",
		      (Rect){ out.x + 10, out.y + 6, out.w - 20, 22 }, 16, 500,
		      0xfa777c, FALSE);
	control(out, A_LOGOUT, 0);
	y += 48;
	ui_draw_label("Avernella",
		      (Rect){ nav_x + 10, y, nav_w - 20, 16 }, 12, 400, FAINT,
		      FALSE);
	ui_draw_label(ui_build_commit(),
		      (Rect){ nav_x + 10, y + 16, nav_w - 20, 16 }, 12, 400,
		      FAINT, FALSE);

	int cx = nav_area + 40, cw = MIN(740, W - nav_area - 140);
	int top = 60 - p.scroll;

	if (cw < 280)
		cw = W - nav_area - 56;
	switch (p.section) {
	case MY_ACCOUNT:
		p.content_h = account_section(cx, top, cw);
		break;
	case PROFILES:
		p.content_h = profiles_section(cx, top, cw);
		break;
	case PRIVACY:
		p.content_h = privacy_section(cx, top, cw);
		break;
	case NOTIFICATIONS:
		p.content_h = notifications_section(cx, top, cw);
		break;
	case KEYBINDS:
		p.content_h = keybinds_section(cx, top, cw);
		break;
	default:
		p.content_h = appearance_section(cx, top, cw);
		break;
	}
	/* The close button and ESC hint, top right of the content. */
	Rect esc = { MIN(cx + cw + 24, W - 52), 60, 36, 36 };
	double eh = ui_draw_hover(esc);

	ui_draw_rounded(esc, 18, ui_blend(0x72767d, 0xb5bac1, eh));
	ui_draw_rounded((Rect){ esc.x + 2, esc.y + 2, 32, 32 }, 16, CONTENT);
	ui_draw_icon(esc, UI_ICON_CLOSE, ui_blend(0xb5bac1, HEADING, eh));
	ui_draw_centered("ESC", (Rect){ esc.x - 4, esc.y + 40, 44, 16 }, 13,
			 600, MUTED);
	control(esc, A_CLOSE, 0);
	/* Discord's unsaved-changes bar. */
	if (p.section == PROFILES && form_changed()) {
		Rect bar = { cx, H - 72, cw, 52 };

		ui_draw_rounded(bar, 8, INNER);
		ui_draw_label("Careful — you have unsaved changes!",
			      (Rect){ bar.x + 16, bar.y + 15, bar.w - 250, 22 },
			      15, 500, INK, FALSE);
		button((Rect){ bar.x + bar.w - 220, bar.y + 10, 80, 32 },
		       "Reset", INNER, INK, A_RESET, 0);
		button((Rect){ bar.x + bar.w - 132, bar.y + 10, 120, 32 },
		       p.save_tag ? "Saving…" : "Save Changes", GREEN, 0xffffff,
		       A_SAVE, 0);
	}
	if (p.settings_note && *p.settings_note) {
		Rect n = { cx,
			   H - (p.section == PROFILES && form_changed() ? 104 :
									  44),
			   cw, 24 };

		ui_draw_label(p.settings_note, n, 14, 500, INK, FALSE);
	}
}

/* ----- Inbox ----- */

static void request_inbox(void)
{
	p.requests_tag = next_tag();
	ui_rpc("friendRequests", NULL, p.requests_tag);
}

void ui_inbox_open(void)
{
	if (!ui.connected)
		return;
	stop_editing();
	p.view = INBOX;
	p.unseen = 0;
	g_clear_pointer(&p.inbox_note, g_free);
	request_inbox();
	SDL_StopTextInput();
	ui.dirty = TRUE;
}

static int inbox_row(Json *row, int x, int y, int w, int index,
		     gboolean request)
{
	Rect r = { x, y, w, 56 };
	double h = ui_draw_hover(r);

	if (h > 0.)
		ui_draw_rounded(r, 6, ui_blend(POPOUT, 0x232428, h));
	Rect face = { x + 8, y + 10, 36, 36 };

	ui_draw_profile(row, face);
	control((Rect){ x, y, w - 90, 56 }, A_PERSON,
		index + (request ? 0 : 1000));
	ui_draw_label(ui_string(row, "name", ""),
		      (Rect){ x + 54, y + 9, w - 150, 20 }, 15, 600, HEADING,
		      FALSE);
	ui_draw_label(
		request ? "Incoming friend request" : "Added you as a friend",
		(Rect){ x + 54, y + 30, w - 150, 18 }, 13, 400, MUTED, FALSE);
	gboolean busy = p.answer_id &&
			!g_strcmp0(p.answer_id, ui_string(row, "id", ""));

	if (request) {
		Rect yes = { x + w - 84, y + 12, 32, 32 },
		     no = { x + w - 44, y + 12, 32, 32 };

		ui_draw_rounded(yes, 16,
				ui_blend(CHIP, 0x35373c, ui_draw_hover(yes)));
		ui_draw_icon(yes, UI_ICON_CHECK, busy ? MUTED : 0x23a55a);
		ui_draw_rounded(no, 16,
				ui_blend(CHIP, 0x35373c, ui_draw_hover(no)));
		ui_draw_icon(no, UI_ICON_CLOSE, busy ? MUTED : 0xf23f43);
		control(yes, A_ACCEPT, index);
		control(no, A_DECLINE, index);
	} else if (flag(row, "canAdd") && !flag(row, "friend")) {
		button((Rect){ x + w - 84, y + 12, 72, 32 }, busy ? "…" : "Add",
		       GREEN, 0xffffff, A_ADD_BACK, index);
	} else {
		ui_draw_label("Friends", (Rect){ x + w - 80, y + 19, 68, 18 },
			      13, 500, MUTED, FALSE);
	}
	return 58;
}

static void draw_inbox(void)
{
	int w = MIN(INBOX_W, ui.width - 16);
	int h = MIN(480, ui.height - 96);
	Rect anchor = p.inbox_button;
	int x = CLAMP(anchor.x - 40, 8, MAX(8, ui.width - w - 8));
	int y = MAX(8, anchor.y - h - 12);

	p.panel = (Rect){ x, y, w, h };
	control(p.panel, A_NOTHING, 0);
	ui_draw_rounded((Rect){ x - 1, y - 1, w + 2, h + 2 }, 9, POPOUT_EDGE);
	ui_draw_rounded(p.panel, 8, POPOUT);
	ui_draw_icon((Rect){ x + 12, y + 12, 24, 24 }, UI_ICON_INBOX, HEADING);
	ui_draw_label("Inbox", (Rect){ x + 44, y + 13, w - 60, 24 }, 16, 700,
		      HEADING, FALSE);
	ui_draw_box((Rect){ x, y + 48, w, 1 }, POPOUT_EDGE);
	int ry = y + 58, inner = w - 16;
	size_t nreq = p.requests ? json_object_array_length(p.requests) : 0;
	size_t nadd = p.adds ? json_object_array_length(p.adds) : 0;

	if (nreq) {
		caption("Friend requests", x + 16, ry, inner);
		ry += 22;
		for (size_t i = 0; i < nreq && ry + 56 < y + h; i++)
			ry += inbox_row(json_object_array_get_idx(p.requests,
								  i),
					x + 8, ry, inner, (int)i, TRUE);
		ry += 8;
	}
	if (nadd && ry + 80 < y + h) {
		caption("Recent", x + 16, ry, inner);
		ry += 22;
		for (size_t i = nadd; i-- > 0 && ry + 56 < y + h;)
			ry += inbox_row(json_object_array_get_idx(p.adds, i),
					x + 8, ry, inner, (int)i, FALSE);
	}
	if (!nreq && !nadd) {
		const char *empty = p.inbox_note ? p.inbox_note :
				    !p.requests_loaded ?
						   "Loading…" :
						   "You're all caught up.";
		ui_draw_centered(empty, (Rect){ x, y + h / 2 - 12, w, 24 }, 15,
				 500, MUTED);
	} else if (p.inbox_note)
		ui_draw_label(p.inbox_note,
			      (Rect){ x + 16, y + h - 28, w - 32, 20 }, 13, 450,
			      0xfa777c, FALSE);
}

static Json *inbox_item(int arg)
{
	Json *list = arg >= 1000 ? p.adds : p.requests;
	int i = arg >= 1000 ? arg - 1000 : arg;

	return list && i >= 0 && (size_t)i < json_object_array_length(list) ?
		       json_object_array_get_idx(list, i) :
		       NULL;
}

static void answer(Json *row, gboolean accept)
{
	const char *id = ui_string(row, "id", NULL);

	if (!id || p.answer_id)
		return;
	set_text(&p.answer_id, id);
	p.answer_tag = next_tag();
	ui_rpc(accept ? "acceptFriendRequest" : "declineFriendRequest",
	       params_id(id), p.answer_tag);
}

static void add_back(Json *row)
{
	const char *id = ui_string(row, "id", NULL);

	if (!id || p.answer_id)
		return;
	set_text(&p.answer_id, id);
	p.answer_tag = next_tag();
	/* profileCard remembered this person when the event arrived. */
	ui_rpc("addProfileFriend", params_id(id), p.answer_tag);
}

/* ----- Drawing and input ----- */

void ui_profile_draw(void)
{
	p.ncontrols = 0;
	switch (p.view) {
	case CARD:
		draw_popout();
		break;
	case SETTINGS:
		draw_settings();
		break;
	case INBOX:
		draw_inbox();
		break;
	default:
		break;
	}
}

static void activate(struct control c, int x)
{
	switch (c.action) {
	case A_CLOSE:
		close_view();
		break;
	case A_MESSAGE:
	case A_ADD:
	case A_BLOCK:
	case A_UNBLOCK:
	case A_EDIT_PROFILE:
		card_action(c.action);
		break;
	case A_NAV:
		stop_editing();
		p.section = c.arg;
		p.scroll = 0;
		if (p.section == PROFILES && !form_changed())
			load_form();
		break;
	case A_LOGOUT:
		close_view();
		ui_sign_out();
		break;
	case A_FIELD:
		focus_field(c.arg, x, c.r);
		break;
	case A_PICTURE:
		stop_editing();
		ui_media_pick_avatar();
		break;
	case A_SAVE:
		save_form();
		break;
	case A_RESET:
		stop_editing();
		load_form();
		break;
	case A_VIEW:
		close_view();
		ui_friends_show_view(c.arg);
		break;
	case A_OPEN_INBOX:
		ui_inbox_open();
		break;
	case A_NOTIFY:
		ui_notifications_set(!ui_notifications_enabled());
		break;
	case A_SOUNDS:
		ui_sounds_set(!ui_sounds_enabled());
		if (ui_sounds_enabled())
			ui_sound_play(CHIP_MESSAGE);
		break;
	case A_MOTION:
		ui_motion_save(!ui_reduce_motion());
		break;
	case A_MOTION_SYSTEM:
		ui_motion_save(-1);
		break;
	case A_KEYS:
		close_view();
		ui_lists_open_preferences();
		break;
	case A_ACCEPT:
	case A_DECLINE:
		answer(inbox_item(c.arg), c.action == A_ACCEPT);
		break;
	case A_ADD_BACK:
		add_back(inbox_item(c.arg + 1000));
		break;
	case A_PERSON: {
		Json *row = inbox_item(c.arg);

		if (row)
			ui_profile_open(row, NULL, c.r);
		break;
	}
	default:
		break;
	}
	ui.dirty = TRUE;
}

static gboolean key(SDL_KeyboardEvent k)
{
	SDL_Keycode code = k.keysym.sym;

	if (p.editing) {
		Editor *e = p.editing == 1 ? &p.name : &p.status;

		if (code == SDLK_ESCAPE) {
			stop_editing();
		} else if (code == SDLK_RETURN || code == SDLK_KP_ENTER) {
			stop_editing();
			save_form();
		} else if (code == SDLK_TAB) {
			focus_field(p.editing == 1 ? 2 : 1, 0, (Rect){ 0 });
		} else {
			ui_edit_key(e, k);
		}
		ui.dirty = TRUE;
		return TRUE;
	}
	if (code == SDLK_ESCAPE) {
		close_view();
		return TRUE;
	}
	if (p.view == CARD && (code == SDLK_RETURN || code == SDLK_KP_ENTER))
		card_action(flag(p.card, "self") ? A_EDIT_PROFILE : A_MESSAGE);
	if (p.view == SETTINGS && (code == SDLK_UP || code == SDLK_DOWN) &&
	    (k.keysym.mod & KMOD_CTRL)) {
		p.section =
			(p.section + (code == SDLK_UP ? N_SECTIONS - 1 : 1)) %
			N_SECTIONS;
		p.scroll = 0;
		ui.dirty = TRUE;
	}
	return TRUE;
}

static void toast_expired(void)
{
	if (p.toast && g_get_monotonic_time() >= p.toast_until) {
		g_clear_pointer(&p.toast, json_object_put);
		ui.dirty = TRUE;
	}
}

static gboolean pressed_toast(SDL_Event *e)
{
	if (!p.toast || e->type != SDL_MOUSEBUTTONDOWN ||
	    !hit(p.toast_rect, e->button.x, e->button.y))
		return FALSE;
	g_clear_pointer(&p.toast, json_object_put);
	ui_inbox_open();
	return TRUE;
}

gboolean ui_profile_event(SDL_Event *e)
{
	if (p.wake_event && e->type == p.wake_event) {
		toast_expired();
		return TRUE;
	}
	if (p.view == NONE) {
		if (ui.connected && e->type == SDL_KEYDOWN &&
		    e->key.keysym.sym == SDLK_COMMA &&
		    (e->key.keysym.mod & KMOD_CTRL)) {
			ui_settings_open();
			return TRUE;
		}
		return pressed_toast(e);
	}
	if (pressed_toast(e))
		return TRUE;
	switch (e->type) {
	case SDL_KEYDOWN:
		return key(e->key);
	case SDL_TEXTINPUT:
		if (p.editing)
			ui_insert(p.editing == 1 ? &p.name : &p.status,
				  e->text.text);
		ui.dirty = TRUE;
		return TRUE;
	case SDL_MOUSEMOTION: {
		ui.hover_x = e->motion.x;
		ui.hover_y = e->motion.y;
		int now = control_at(e->motion.x, e->motion.y);

		if (now != p.hover) {
			p.hover = now;
			ui.dirty = TRUE;
		}
		return TRUE;
	}
	case SDL_MOUSEBUTTONDOWN: {
		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		int i = control_at(e->button.x, e->button.y);

		if (i < 0) {
			/* Discord closes popovers on an outside click. */
			if (p.view != SETTINGS)
				close_view();
			return TRUE;
		}
		struct control c = p.controls[i];

		if (c.action != A_FIELD && p.editing)
			stop_editing();
		activate(c, e->button.x);
		return TRUE;
	}
	case SDL_MOUSEWHEEL:
		if (p.view == SETTINGS) {
			int max = MAX(0, p.content_h - ui.height + 120);

			p.scroll = CLAMP(p.scroll - e->wheel.y * 48, 0, max);
			ui.dirty = TRUE;
		}
		return TRUE;
	case SDL_MOUSEBUTTONUP:
		return TRUE;
	default:
		return FALSE;
	}
}

/* ----- Replies and live events ----- */

static void apply_self(Json *row)
{
	if (!json_object_is_type(row, json_type_object))
		return;
	gboolean keep_form = form_changed();

	set_json(&p.me, row);
	const char *name = ui_string(row, "name", NULL);

	if (name && g_strcmp0(name, ui.name)) {
		g_free(ui.name);
		ui.name = g_strdup(name);
	}
	if (!keep_form && !p.editing)
		load_form();
	if (p.card && flag(p.card, "self")) {
		Json *card = json_object_get(row);

		json_object_object_add(card, "self",
				       json_object_new_boolean(TRUE));
		set_json(&p.card, card);
		json_object_put(card);
	}
}

static void replace_card(Json *row)
{
	if (!p.card ||
	    g_strcmp0(ui_string(row, "id", ""), ui_string(p.card, "id", "")))
		return;
	set_json(&p.card, row);
}

static gboolean requests_reply(guint tag, Json *result, const char *error)
{
	if (tag != p.requests_tag)
		return FALSE;
	p.requests_tag = 0;
	p.requests_loaded = TRUE;
	if (error) {
		g_clear_pointer(&p.requests, json_object_put);
		if (p.view == INBOX && !p.adds)
			set_text(
				&p.inbox_note,
				"Friend requests aren't available for this account.");
	} else if (json_object_is_type(result, json_type_array)) {
		size_t before =
			p.requests ? json_object_array_length(p.requests) : 0;

		set_json(&p.requests, result);
		size_t now = json_object_array_length(result);

		if (p.view != INBOX && now > before)
			p.unseen += (int)(now - before);
	}
	ui.dirty = TRUE;
	return TRUE;
}

static void drop_request(const char *id)
{
	for (size_t i = 0;
	     p.requests && i < json_object_array_length(p.requests); i++)
		if (!g_strcmp0(
			    ui_string(json_object_array_get_idx(p.requests, i),
				      "id", NULL),
			    id)) {
			json_object_array_del_idx(p.requests, i, 1);
			break;
		}
}

static void mark_added(Json *row)
{
	const char *id = ui_string(row, "id", NULL);

	for (size_t i = 0; id && p.adds && i < json_object_array_length(p.adds);
	     i++) {
		Json *a = json_object_array_get_idx(p.adds, i);

		if (!g_strcmp0(ui_string(a, "id", NULL), id))
			json_object_array_put_idx(p.adds, i,
						  json_object_get(row));
	}
}

gboolean ui_profile_reply(const char *method, guint tag, Json *result,
			  const char *error)
{
	if (tag < TAG_BASE)
		return FALSE;
	if (!strcmp(method, "profileCard") && tag == p.card_tag) {
		p.card_tag = 0;
		if (error) {
			json_object_object_del(p.card, "loading");
			set_text(&p.note, error);
		} else
			replace_card(result);
		return TRUE;
	}
	if (tag == p.action_tag) {
		p.action_tag = 0;
		p.busy = FALSE;
		if (error) {
			set_text(&p.note, error);
		} else if (!strcmp(method, "openProfileChat")) {
			close_view();
			ui_open_conversation(result);
		} else {
			replace_card(result);
			mark_added(result);
		}
		ui.dirty = TRUE;
		return TRUE;
	}
	if (!strcmp(method, "myProfile") && tag == p.me_tag) {
		p.me_tag = 0;
		if (!error)
			apply_self(result);
		return TRUE;
	}
	if (tag == p.save_tag || tag == p.picture_tag) {
		gboolean picture = tag == p.picture_tag;

		if (picture)
			p.picture_tag = 0;
		else
			p.save_tag = 0;
		if (error) {
			set_text(&p.settings_note, error);
		} else {
			if (!picture)
				json_object_object_del(result, "self");
			editor_text(&p.name, ui_string(result, "name", ""));
			editor_text(&p.status, ui_string(result, "status", ""));
			apply_self(result);
			set_text(&p.settings_note,
				 picture ? "Your new avatar is live." :
					   "Saved.");
		}
		return TRUE;
	}
	if (requests_reply(tag, result, error))
		return TRUE;
	if (tag == p.answer_tag) {
		p.answer_tag = 0;
		g_autofree char *id = g_steal_pointer(&p.answer_id);

		if (error) {
			set_text(&p.inbox_note, error);
		} else {
			drop_request(id);
			if (json_object_get_boolean(ui_field(result, "friend")))
				mark_added(result);
			ui_refresh();
		}
		ui.dirty = TRUE;
		return TRUE;
	}
	return FALSE;
}

static Uint32 wake(Uint32 interval, void *data)
{
	(void)interval;
	(void)data;
	SDL_Event e = { .type = p.wake_event };

	SDL_PushEvent(&e);
	return 0;
}

static void show_toast(Json *row)
{
	set_json(&p.toast, row);
	p.toast_until = g_get_monotonic_time() + TOAST_MS * 1000LL;
	if (!p.wake_event) {
		Uint32 type = SDL_RegisterEvents(1);

		p.wake_event = type == (Uint32)-1 ? 0 : type;
	}
	if (p.wake_event)
		SDL_AddTimer(TOAST_MS + 20, wake, NULL);
}

/* Patch every place the window shows this person. */
static void contact_changed(Json *row)
{
	replace_card(row);
	mark_added(row);
	ui_update_friend_chat(row);
}

void ui_profile_packet(Json *packet)
{
	const char *event = ui_string(packet, "event", "");
	Json *row = ui_field(packet, "row");

	if (!strcmp(event, "self")) {
		apply_self(row);
		return;
	}
	if (!strcmp(event, "contact")) {
		if (row)
			contact_changed(row);
		return;
	}
	gboolean added = !strcmp(event, "friend_added");

	if (!added && strcmp(event, "friend_request"))
		return;
	if (added && row) {
		if (!p.adds)
			p.adds = json_object_new_array();
		json_object_array_add(p.adds, json_object_get(row));
		/* Keep the newest few for the Inbox. */
		while (json_object_array_length(p.adds) > 20)
			json_object_array_del_idx(p.adds, 0, 1);
		contact_changed(row);
	}
	if (p.view != INBOX)
		p.unseen++;
	if (!added || p.view == INBOX)
		request_inbox();
	Json *shown = row;
	Json *placeholder = NULL;

	if (!shown) {
		placeholder = json_object_new_object();
		json_object_object_add(placeholder, "name",
				       json_object_new_string("Someone"));
		shown = placeholder;
	}
	json_object_object_add(
		shown, "toast",
		json_object_new_string(added ? "added you as a friend" :
					       "sent you a friend request"));
	show_toast(shown);
	json_object_put(placeholder);
	if (ui_notifications_enabled() &&
	    (!ui.active ||
	     !(SDL_GetWindowFlags(ui.window) & SDL_WINDOW_INPUT_FOCUS))) {
		g_autofree char *body = g_strdup_printf(
			"%s %s", ui_string(shown, "name", "Someone"),
			ui_string(shown, "toast", ""));
		ui_desktop_notify_text("LINE", body);
	}
	ui.dirty = TRUE;
}

void ui_profile_login(void)
{
	request_me();
	request_inbox();
}

void ui_profile_reset(void)
{
	stop_editing();
	p.view = NONE;
	p.ncontrols = 0;
	p.unseen = 0;
	p.requests_loaded = FALSE;
	p.card_tag = p.action_tag = p.me_tag = p.save_tag = p.picture_tag =
		p.requests_tag = p.answer_tag = 0;
	g_clear_pointer(&p.card, json_object_put);
	g_clear_pointer(&p.me, json_object_put);
	g_clear_pointer(&p.requests, json_object_put);
	g_clear_pointer(&p.adds, json_object_put);
	g_clear_pointer(&p.toast, json_object_put);
	g_clear_pointer(&p.chat, g_free);
	g_clear_pointer(&p.note, g_free);
	g_clear_pointer(&p.settings_note, g_free);
	g_clear_pointer(&p.inbox_note, g_free);
	g_clear_pointer(&p.answer_id, g_free);
	if (p.name.value)
		g_string_truncate(p.name.value, 0);
	if (p.status.value)
		g_string_truncate(p.status.value, 0);
}

/* ----- User panel and toast ----- */

Rect ui_profile_self_area(Rect panel)
{
	p.user_panel = panel;
	p.gear_button = (Rect){ ui.logout.x - 34, ui.logout.y, 32, 32 };
	p.inbox_button = (Rect){ ui.logout.x - 68, ui.logout.y, 32, 32 };
	p.self_area = (Rect){ panel.x + 4, panel.y + 6,
			      MAX(40, p.inbox_button.x - panel.x - 8),
			      panel.h - 12 };
	return p.self_area;
}

const char *ui_profile_self_avatar(void)
{
	return p.me ? ui_string(p.me, "avatarKey", NULL) : NULL;
}

const char *ui_profile_self_status(void)
{
	return p.me ? ui_string(p.me, "status", NULL) : NULL;
}

int ui_profile_badge(void)
{
	return p.unseen;
}

static void panel_button(Rect r, enum ui_icon icon, gboolean lit,
			 const char *tip)
{
	double h = ui_draw_hover(r);

	if (h > 0. || lit)
		ui_draw_rounded(r, 6,
				ui_blend(0x232428, 0x2e2f35, lit ? 1. : h));
	ui_draw_icon(r, icon, h > .5 || lit ? HEADING : 0xb5bac1);
	if (h > .5)
		ui_draw_tooltip((Rect){ r.x, r.y - 80, r.w, 34 }, tip);
}

void ui_profile_panel_draw(void)
{
	panel_button(p.inbox_button, UI_ICON_INBOX, p.view == INBOX, "Inbox");
	if (p.unseen > 0) {
		Rect dot = { p.inbox_button.x + 18, p.inbox_button.y + 4, 12,
			     12 };
		ui_draw_rounded((Rect){ dot.x - 2, dot.y - 2, 16, 16 }, 8,
				0x232428);
		ui_draw_rounded(dot, 6, 0xf23f43);
	}
	panel_button(p.gear_button, UI_ICON_GEAR, p.view == SETTINGS,
		     "User Settings");
}

int ui_profile_panel_hover(int x, int y)
{
	if (!ui.connected || !ui_list_visible())
		return 0;
	return hit(p.inbox_button, x, y)	  ? -3001 :
	       hit(p.gear_button, x, y)		  ? -3002 :
	       hit(p.self_area, x, y)		  ? -3003 :
	       hit(p.toast_rect, x, y) && p.toast ? -3004 :
						    0;
}

gboolean ui_profile_panel_click(int x, int y)
{
	if (!ui.connected || !ui_list_visible())
		return FALSE;
	if (hit(p.inbox_button, x, y)) {
		ui_inbox_open();
		return TRUE;
	}
	if (hit(p.gear_button, x, y)) {
		ui_settings_open();
		return TRUE;
	}
	if (hit(p.self_area, x, y) && ui.self_id) {
		Json *me = json_object_new_object();

		json_object_object_add(me, "id",
				       json_object_new_string(ui.self_id));
		json_object_object_add(
			me, "name",
			json_object_new_string(ui.name ? ui.name : ""));
		json_object_object_add(
			me, "avatarKey",
			json_object_new_string(
				ui_profile_self_avatar() ?
					ui_profile_self_avatar() :
					""));
		/* Discord opens your own popout above the panel. */
		ui_profile_open(me, NULL,
				(Rect){ p.self_area.x, p.self_area.y - 420, 0,
					0 });
		json_object_put(me);
		return TRUE;
	}
	return FALSE;
}

gboolean ui_profile_toast_visible(void)
{
	return p.toast != NULL;
}

void ui_profile_toast_draw(void)
{
	p.toast_rect = (Rect){ 0 };
	if (!p.toast)
		return;
	if (g_get_monotonic_time() >= p.toast_until) {
		g_clear_pointer(&p.toast, json_object_put);
		return;
	}
	int w = MIN(TOAST_W, ui.width - 16);
	Rect r = { ui.width - w - 16, ui.height - TOAST_H - 88, w, TOAST_H };

	p.toast_rect = r;
	ui_draw_rounded((Rect){ r.x - 1, r.y - 1, r.w + 2, r.h + 2 }, 9,
			POPOUT_EDGE);
	ui_draw_rounded(r, 8, INNER);
	ui_draw_profile(p.toast, (Rect){ r.x + 12, r.y + 12, 40, 40 });
	ui_draw_label(ui_string(p.toast, "name", ""),
		      (Rect){ r.x + 64, r.y + 12, r.w - 76, 20 }, 15, 600,
		      HEADING, FALSE);
	ui_draw_label(ui_string(p.toast, "toast", ""),
		      (Rect){ r.x + 64, r.y + 33, r.w - 76, 18 }, 13, 400,
		      MUTED, FALSE);
}
