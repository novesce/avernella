// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_chat_tools.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include "ui_find.h"
#include "ui_friends.h"
#include "ui_shared_content.h"
#include "ui_motion.h"

enum tools_page {
	CLOSED,
	MENU,
	FORWARD,
	PUBLISH,
	ANNOUNCEMENTS,
	REMOVE
};

static struct {
	enum tools_page page;
	char *chat, *message, *note, *remove;
	Editor filter;
	Json *row, *announcements, *destinations;
	GArray *matches;
	guint serial, pending;
	int selected, offset;
	Rect field, list, accept, view, cancel, announcements_button,
		shared_button, search_button, screenshot_button, record_button;
} tools;

static gboolean inside(Rect rect, int x, int y)
{
	return x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
	       y < rect.y + rect.h;
}

void ui_chat_tools_reset(void)
{
	g_clear_pointer(&tools.chat, g_free);
	g_clear_pointer(&tools.message, g_free);
	g_clear_pointer(&tools.note, g_free);
	g_clear_pointer(&tools.remove, g_free);
	g_clear_pointer(&tools.row, json_object_put);
	g_clear_pointer(&tools.announcements, json_object_put);
	g_clear_pointer(&tools.destinations, json_object_put);
	g_clear_pointer(&tools.matches, g_array_unref);
	if (tools.filter.value) {
		g_string_free(tools.filter.value, TRUE);
		tools.filter = (Editor){ 0 };
	}
	tools.pending = 0;
	tools.page = CLOSED;
	ui.dirty = TRUE;
}

gboolean ui_chat_tools_active(void)
{
	return tools.page != CLOSED;
}

void ui_chat_tools_resume_input(void)
{
	if (tools.page == FORWARD)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

static void note(const char *text)
{
	g_free(tools.note);
	tools.note = g_strdup(text);
	ui.dirty = TRUE;
}

static void filter_chats(void)
{
	g_array_set_size(tools.matches, 0);
	for (guint i = 0; tools.destinations &&
			  i < json_object_array_length(tools.destinations);
	     i++) {
		Json *chat = json_object_array_get_idx(tools.destinations, i);
		const char *name = ui_string(chat, "name", "");

		if (!tools.filter.value->len ||
		    g_str_match_string(tools.filter.value->str, name, TRUE))
			g_array_append_val(tools.matches, i);
	}
	tools.selected = -1;
	tools.offset = 0;
	ui.dirty = TRUE;
}

static void open_page(enum tools_page page, Json *row)
{
	ui_chat_tools_reset();
	if (!ui.selected)
		return;
	tools.page = page;
	tools.chat = g_strdup(ui.selected);
	tools.selected = -1;
	if (row) {
		tools.row = json_object_get(row);
		tools.message = g_strdup(ui_string(row, "id", ""));
	}
	if (page == FORWARD) {
		tools.destinations = json_object_get(ui.chats);
		tools.filter.value = g_string_new(NULL);
		tools.matches = g_array_new(FALSE, FALSE, sizeof(guint));
		filter_chats();
	}
	ui_chat_tools_resume_input();
}

void ui_chat_tools_open(void)
{
	open_page(MENU, NULL);
}

void ui_chat_tools_forward(Json *row)
{
	open_page(FORWARD, row);
}

void ui_chat_tools_announce(Json *row)
{
	open_page(PUBLISH, row);
}

static struct {
	char *chat, *collapsed;
	Json *rows;
	Rect box, preview, list, toggle;
	guint pending;
	gboolean changed;
} banner;

void ui_chat_announcements_refresh(const char *chat)
{
	if (!banner.chat || (chat && g_strcmp0(chat, banner.chat)))
		return;
	if (banner.pending) {
		banner.changed = TRUE;
		return;
	}
	Json *params = json_object_new_object();

	json_object_object_add(params, "chat",
			       json_object_new_string(banner.chat));
	banner.pending = ++tools.serial;
	ui_rpc("announcements", params, banner.pending);
}

void ui_chat_announcements_select(const char *chat)
{
	if (!g_strcmp0(chat, banner.chat))
		return;
	g_free(banner.chat);
	banner.chat = g_strdup(chat);
	g_clear_pointer(&banner.collapsed, g_free);
	g_clear_pointer(&banner.rows, json_object_put);
	banner.pending = 0;
	banner.changed = FALSE;
	banner.box = (Rect){ 0 };
	if (chat)
		ui_chat_announcements_refresh(chat);
	ui.dirty = TRUE;
}

static Json *newest_announcement(void)
{
	Json *newest = NULL;
	gint64 time = -1;

	for (size_t i = 0;
	     banner.rows && i < json_object_array_length(banner.rows); i++) {
		Json *row = json_object_array_get_idx(banner.rows, i);
		gint64 date = json_object_get_int64(ui_field(row, "createdAt"));

		if (date > time) {
			newest = row;
			time = date;
		}
	}
	return newest;
}

static gboolean banner_collapsed(void)
{
	return banner.collapsed &&
	       !g_strcmp0(banner.collapsed,
			  ui_string(newest_announcement(), "id", NULL));
}

static void banner_set_rows(Json *rows)
{
	Json *latest = json_object_get(rows);

	g_clear_pointer(&banner.rows, json_object_put);
	banner.rows = latest;
	if (banner.collapsed && !banner_collapsed())
		g_clear_pointer(&banner.collapsed, g_free);
	ui.dirty = TRUE;
}

static void banner_edit(Json *posted, const char *removed)
{
	if (g_strcmp0(banner.chat, tools.chat))
		return;
	const char *id = posted ? ui_string(posted, "id", NULL) : removed;

	if (!id)
		return;
	Json *rows = json_object_new_array();

	if (posted)
		json_object_array_add(rows, json_object_get(posted));
	for (size_t i = 0;
	     banner.rows && i < json_object_array_length(banner.rows); i++) {
		Json *r = json_object_array_get_idx(banner.rows, i);

		if (g_strcmp0(id, ui_string(r, "id", NULL)))
			json_object_array_add(rows, json_object_get(r));
	}
	banner_set_rows(rows);
	json_object_put(rows);
}

int ui_chat_announcements_height(void)
{
	if (!banner.rows || !json_object_array_length(banner.rows) ||
	    g_strcmp0(banner.chat, ui.selected))
		return 0;
	return banner_collapsed() ? 36 : 50;
}

void ui_chat_announcements_place(Rect region)
{
	int height = ui_chat_announcements_height();

	banner.box = height ? (Rect){ region.x, region.y, region.w, height } :
			      (Rect){ 0 };
	banner.preview = banner.list = banner.toggle = (Rect){ 0 };
	if (!height)
		return;
	gboolean collapsed = banner_collapsed();

	banner.toggle = (Rect){ region.x + region.w - 41, region.y + 5, 30,
				height - 10 };
	banner.preview = (Rect){ region.x + 14, region.y,
				 region.w - (collapsed ? 60 : 97), height };
	if (!collapsed)
		banner.list = (Rect){ banner.toggle.x - 39, region.y + 8, 32,
				      height - 16 };
}

void ui_chat_announcements_draw(void)
{
	Rect r = banner.box;

	if (!r.h)
		return;
	gboolean collapsed = banner_collapsed();

	ui_draw_box(r, UI_PANEL);
	ui_draw_button(banner.toggle, collapsed ? "⌄" : "⌃", FALSE);
	if (collapsed) {
		ui_draw_label(_("Announcements"),
			      (Rect){ banner.preview.x, r.y + 8,
				      banner.preview.w, r.h - 8 },
			      13, 550, UI_MUTED, FALSE);
		return;
	}
	ui_draw_button(banner.list, "…", FALSE);
	ui_draw_label(_("Announcements"),
		      (Rect){ banner.preview.x, r.y + 4, banner.preview.w, 17 },
		      11, 650, UI_ACCENT, FALSE);
	const char *text = ui_string(newest_announcement(), "text", "");

	ui_draw_label(text,
		      (Rect){ banner.preview.x, r.y + 21, banner.preview.w,
			      27 },
		      14, 550, UI_HEADING, FALSE);
}

static void close_tools(void)
{
	ui_chat_tools_reset();
	ui_restore_input();
}

static size_t row_count(void)
{
	if (tools.page == FORWARD)
		return tools.matches->len;
	return tools.announcements ?
		       json_object_array_length(tools.announcements) :
		       0;
}

static Json *item(int index)
{
	if (index < 0 || (size_t)index >= row_count())
		return NULL;
	if (tools.page == FORWARD)
		return json_object_array_get_idx(tools.destinations,
						 g_array_index(tools.matches,
							       guint, index));
	return json_object_array_get_idx(tools.announcements, index);
}

static void request(const char *method, const char *id, const char *destination)
{
	Json *params = json_object_new_object();

	json_object_object_add(params, "chat",
			       json_object_new_string(tools.chat));
	if (id)
		json_object_object_add(params, "id",
				       json_object_new_string(id));
	if (destination)
		json_object_object_add(params, "destination",
				       json_object_new_string(destination));
	tools.pending = ++tools.serial;
	ui_rpc(method, params, tools.pending);
	note(NULL);
}

static void open_announcements(void)
{
	tools.page = ANNOUNCEMENTS;
	tools.offset = 0;
	tools.selected = -1;
	request("announcements", NULL, NULL);
}

static void show_message(Json *row)
{
	g_autofree char *id = g_strdup(ui_string(row, "messageId", NULL));

	if (tools.page != CLOSED)
		close_tools();
	if (id)
		ui_jump_to_message(id);
}

gboolean ui_chat_announcements_click(int x, int y)
{
	if (!inside(banner.box, x, y))
		return FALSE;
	if (inside(banner.toggle, x, y) ||
	    (banner_collapsed() && inside(banner.preview, x, y))) {
		if (banner_collapsed())
			g_clear_pointer(&banner.collapsed, g_free);
		else
			banner.collapsed = g_strdup(
				ui_string(newest_announcement(), "id", ""));
		ui.dirty = TRUE;
	} else if (inside(banner.list, x, y) ||
		   (inside(banner.preview, x, y) &&
		    !ui_string(newest_announcement(), "messageId", NULL))) {
		open_page(ANNOUNCEMENTS, NULL);
		open_announcements();
	} else if (inside(banner.preview, x, y)) {
		show_message(newest_announcement());
	}
	return TRUE;
}

static void primary(void)
{
	if (tools.pending)
		return;
	Json *active = item(tools.selected);

	switch (tools.page) {
	case FORWARD:
		if (active)
			request("forwardMessage", tools.message,
				ui_string(active, "id", NULL));
		break;
	case PUBLISH:
		request("announceMessage", tools.message, NULL);
		break;
	case ANNOUNCEMENTS:
		if (active &&
		    json_object_get_boolean(ui_field(active, "canRemove"))) {
			g_free(tools.remove);
			tools.remove = g_strdup(ui_string(active, "id", ""));
			tools.page = REMOVE;
			ui.dirty = TRUE;
		}
		break;
	case REMOVE:
		request("removeAnnouncement", tools.remove, NULL);
		break;
	default:
		break;
	}
}

gboolean ui_chat_tools_reply(const char *method, guint tag, Json *result,
			     const char *error)
{
	gboolean list = !strcmp(method, "announcements");
	gboolean announce = !strcmp(method, "announceMessage");
	gboolean remove = !strcmp(method, "removeAnnouncement");
	gboolean forward = !strcmp(method, "forwardMessage");

	if (!list && !announce && !remove && !forward)
		return FALSE;
	if (list && banner.pending && tag == banner.pending) {
		banner.pending = 0;
		if (!error && json_object_is_type(result, json_type_array))
			banner_set_rows(result);
		if (banner.changed) {
			banner.changed = FALSE;
			ui_chat_announcements_refresh(NULL);
		}
		return TRUE;
	}
	if (!tools.pending || tag != tools.pending)
		return TRUE;
	tools.pending = 0;
	if (error) {
		note(error);
		return TRUE;
	}
	if (list) {
		if (!json_object_is_type(result, json_type_array)) {
			note(_("Could not read the announcement list."));
			return TRUE;
		}
		g_clear_pointer(&tools.announcements, json_object_put);
		tools.announcements = json_object_get(result);
		if (!g_strcmp0(tools.chat, banner.chat))
			banner_set_rows(result);
		ui.dirty = TRUE;
	} else if (forward) {
		Json *target = item(tools.selected);
		gboolean current =
			target &&
			!g_strcmp0(ui_string(target, "id", NULL), ui.selected);

		close_tools();
		ui_status(_("Message forwarded."));
		if (current)
			ui_show_latest();
	} else {
		banner_edit(announce ? result : NULL,
			    remove ? tools.remove : NULL);
		open_announcements();
	}
	return TRUE;
}

static int visible_rows(void)
{
	return MAX(1, tools.list.h / 48);
}

static void step(int direction)
{
	if (!row_count())
		return;
	tools.selected =
		CLAMP(tools.selected + direction, 0, (int)row_count() - 1);
	tools.offset = CLAMP(tools.offset,
			     MAX(0, tools.selected - visible_rows() + 1),
			     tools.selected);
	ui.dirty = TRUE;
}

gboolean ui_chat_tools_event(SDL_Event *event)
{
	if (!ui_chat_tools_active())
		return FALSE;
	if (g_strcmp0(tools.chat, ui.selected)) {
		close_tools();
		return FALSE;
	}
	switch (event->type) {
	case SDL_KEYDOWN: {
		SDL_KeyboardEvent key = event->key;

		if (key.keysym.sym == SDLK_ESCAPE && !tools.pending)
			close_tools();
		else if ((key.keysym.sym == SDLK_RETURN ||
			  key.keysym.sym == SDLK_KP_ENTER) &&
			 !key.repeat) {
			Json *row = item(tools.selected);

			if (!tools.pending && tools.page == ANNOUNCEMENTS &&
			    ui_string(row, "messageId", NULL))
				show_message(row);
			else
				primary();
		} else if (!tools.pending && key.keysym.sym == SDLK_UP)
			step(-1);
		else if (!tools.pending && key.keysym.sym == SDLK_DOWN)
			step(1);
		else if (tools.page == FORWARD && !tools.pending) {
			g_autofree char *previous =
				g_strdup(tools.filter.value->str);

			ui_edit_key(&tools.filter, key);
			if (strcmp(previous, tools.filter.value->str))
				filter_chats();
		}
		return TRUE;
	}
	case SDL_TEXTINPUT:
		if (!tools.pending && tools.page == FORWARD &&
		    tools.filter.value->len < 512) {
			ui_insert(&tools.filter, event->text.text);
			filter_chats();
		}
		return TRUE;
	case SDL_MOUSEWHEEL: {
		static double carry;
		int rows = ui_wheel_rows(ui_wheel_y(&event->wheel), &carry);

		if (!tools.pending) {
			tools.offset = CLAMP(
				tools.offset - rows * 3, 0,
				MAX(0, (int)row_count() - visible_rows()));
			ui.dirty = TRUE;
		}
		return TRUE;
	}
	case SDL_MOUSEBUTTONDOWN: {
		int x = event->button.x, y = event->button.y;

		if (tools.pending || event->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (inside(tools.cancel, x, y))
			close_tools();
		else if (inside(tools.announcements_button, x, y))
			open_announcements();
		else if (inside(tools.shared_button, x, y)) {
			close_tools();
			ui_shared_open();
		} else if (inside(tools.search_button, x, y)) {
			close_tools();
			ui_find_open_all();
		} else if (inside(tools.screenshot_button, x, y)) {
			close_tools();
			ui_media_screenshot();
		} else if (inside(tools.record_button, x, y)) {
			close_tools();
			ui_media_record_video();
		} else if (inside(tools.view, x, y))
			show_message(item(tools.selected));
		else if (inside(tools.accept, x, y))
			primary();
		else if (inside(tools.list, x, y)) {
			int index = (y - tools.list.y) / 48 + tools.offset;

			if (item(index))
				tools.selected = index;
		} else if (tools.page == FORWARD && inside(tools.field, x, y)) {
			int index = ui_edit_hit(&tools.filter, tools.field, x);

			tools.filter.cursor = tools.filter.anchor = index;
		}
		ui.dirty = TRUE;
		return TRUE;
	}
	default:
		return FALSE;
	}
}

void ui_chat_tools_draw(void)
{
	int w = MIN(535, ui.width - 28);
	int h = tools.page == FORWARD || tools.page == ANNOUNCEMENTS ? 450 :
		tools.page == MENU ? (ui.photos || ui.files ? 320 : 270) :
				     240;

	h = MIN(h, ui.height - 32);
	int x = (ui.width - w) / 2, y = (ui.height - h) / 2;
	int foot = y + h - 54;

	ui_scrim();
	ui_draw_box((Rect){ x, y, w, h }, UI_DIALOG);
	const char *title = tools.page == MENU	  ? _("Chat tools") :
			    tools.page == FORWARD ? _("Forward message") :
			    tools.page == PUBLISH ? _("Announce message") :
			    tools.page == REMOVE  ? _("Remove announcement") :
						    _("Announcements");

	ui_draw_label(title, (Rect){ x + 18, y + 13, w - 36, 35 }, 22, 700,
		      UI_HEADING, FALSE);
	tools.field = tools.list = tools.accept = tools.view =
		tools.announcements_button = tools.shared_button =
			tools.search_button = tools.screenshot_button =
				tools.record_button = (Rect){ 0 };
	tools.cancel = (Rect){ x + 18, foot, 100, 36 };
	ui_draw_button(tools.cancel, _("Close"), FALSE);

	if (tools.page == MENU) {
		tools.announcements_button =
			(Rect){ x + 18, y + 59, w - 36, 40 };
		ui_draw_button(tools.announcements_button, _("Announcements"),
			       FALSE);
		tools.shared_button = (Rect){ x + 18, y + 110, w - 36, 40 };
		ui_draw_button(tools.shared_button, _("Shared content"), FALSE);
		tools.search_button = (Rect){ x + 18, y + 161, w - 36, 40 };
		ui_draw_button(tools.search_button, _("Search all chats"),
			       FALSE);
		int capture_w = ui.photos && ui.files ? (w - 44) / 2 : w - 36;

		if (ui.photos) {
			tools.screenshot_button =
				(Rect){ x + 18, y + 212, capture_w, 40 };
			ui_draw_button(tools.screenshot_button,
				       _("Take screenshot"), FALSE);
		}
		if (ui.files) {
			int rx = x + w - 18 - capture_w;

			tools.record_button =
				(Rect){ rx, y + 212, capture_w, 40 };
			ui_draw_button(tools.record_button, _("Record video"),
				       FALSE);
		}
	} else if (tools.page == PUBLISH || tools.page == REMOVE) {
		const char *description =
			tools.page == PUBLISH ?
				_("Share a text preview with this chat. LINE stores announcements without Letter Sealing.") :
				_("Remove this announcement for everyone in the chat?");

		ui_draw_label(description, (Rect){ x + 18, y + 58, w - 36, 59 },
			      15, 500, UI_HEADING, TRUE);
		if (tools.page == PUBLISH)
			ui_draw_label(ui_string(tools.row, "text", ""),
				      (Rect){ x + 18, y + 115, w - 36, 25 }, 13,
				      500, UI_MUTED, FALSE);
		tools.accept = (Rect){ x + w - 130, foot, 112, 36 };
		ui_draw_button(tools.accept,
			       tools.page == PUBLISH ? _("Announce") :
						       _("Remove"),
			       !tools.pending);
	} else {
		int top = y + 55;

		if (tools.page == FORWARD) {
			tools.field = (Rect){ x + 18, top, w - 36, 34 };
			ui_draw_edit(&tools.filter, tools.field, !tools.pending,
				     _("Find a destination chat"));
			top += 44;
		}
		tools.list = (Rect){ x + 18, top, w - 36, foot - top - 42 };
		int visible = visible_rows();

		tools.offset = CLAMP(tools.offset, 0,
				     MAX(0, (int)row_count() - visible));
		for (int i = 0; i < visible; i++) {
			int index = tools.offset + i;
			Json *row = item(index);

			if (!row)
				break;
			Rect area = { tools.list.x, tools.list.y + i * 48,
				      tools.list.w, 44 };

			ui_draw_box(area, index == tools.selected ?
						  UI_SELECTED :
						  UI_RAISED);
			ui_draw_label(ui_string(row,
						tools.page == FORWARD ? "name" :
									"text",
						""),
				      (Rect){ area.x + 10, area.y + 6,
					      area.w - 20, 30 },
				      15, 550, UI_HEADING, FALSE);
		}
		if (!row_count() && !tools.note)
			ui_draw_label(tools.pending ? _("Loading…") :
				      tools.page == FORWARD ?
						      _("No matching chats.") :
						      _("No announcements."),
				      (Rect){ x + 18, top + 15, w - 36, 40 },
				      14, 500, UI_MUTED, FALSE);
		Json *selected = item(tools.selected);

		if (tools.page == ANNOUNCEMENTS &&
		    ui_string(selected, "messageId", NULL)) {
			tools.view = (Rect){ x + w - 224, foot, 84, 36 };
			ui_draw_button(tools.view, _("View"), !tools.pending);
		}
		if (selected && (tools.page == FORWARD ||
				 json_object_get_boolean(
					 ui_field(selected, "canRemove")))) {
			tools.accept = (Rect){ x + w - 130, foot, 112, 36 };
			ui_draw_button(tools.accept,
				       tools.page == FORWARD ? _("Forward") :
							       _("Remove…"),
				       !tools.pending);
		}
	}
	if (tools.note)
		ui_draw_label(tools.note,
			      (Rect){ x + 18, foot - 42, w - 36, 39 }, 13, 500,
			      0xf4acb5, TRUE);
	else if (tools.pending && tools.page != ANNOUNCEMENTS)
		ui_draw_label(_("Working…"),
			      (Rect){ x + 18, foot - 37, w - 36, 26 }, 13, 500,
			      UI_MUTED, FALSE);
}
