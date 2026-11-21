// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_chat_tools.h"
#include "ui_find.h"
#include "ui_friends.h"
#include "ui_shared_content.h"

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
	Rect field, list, accept, cancel, announcements_button, shared_button,
		search_button, screenshot_button;
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
	if (!tools.pending || tag != tools.pending)
		return TRUE;
	tools.pending = 0;
	if (error) {
		note(error);
		return TRUE;
	}
	if (list) {
		if (!json_object_is_type(result, json_type_array)) {
			note("Could not read the announcement list.");
			return TRUE;
		}
		g_clear_pointer(&tools.announcements, json_object_put);
		tools.announcements = json_object_get(result);
		ui.dirty = TRUE;
	} else if (forward) {
		Json *target = item(tools.selected);
		gboolean current =
			target &&
			!g_strcmp0(ui_string(target, "id", NULL), ui.selected);

		close_tools();
		ui_status("Message forwarded.");
		if (current)
			ui_show_latest();
	} else {
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
			 !key.repeat)
			primary();
		else if (!tools.pending && key.keysym.sym == SDLK_UP)
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
	case SDL_MOUSEWHEEL:
		if (!tools.pending) {
			tools.offset = CLAMP(
				tools.offset - event->wheel.y * 3, 0,
				MAX(0, (int)row_count() - visible_rows()));
			ui.dirty = TRUE;
		}
		return TRUE;
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
		} else if (inside(tools.accept, x, y))
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
		tools.page == MENU ? (ui.photos ? 320 : 270) :
				     240;

	h = MIN(h, ui.height - 32);
	int x = (ui.width - w) / 2, y = (ui.height - h) / 2;
	int foot = y + h - 54;

	ui_scrim();
	ui_draw_box((Rect){ x, y, w, h }, 0x242429);
	const char *title = tools.page == MENU	  ? "Chat tools" :
			    tools.page == FORWARD ? "Forward message" :
			    tools.page == PUBLISH ? "Announce message" :
			    tools.page == REMOVE  ? "Remove announcement" :
						    "Announcements";

	ui_draw_label(title, (Rect){ x + 18, y + 13, w - 36, 35 }, 22, 700,
		      0xefeff1, FALSE);
	tools.field = tools.list = tools.accept = tools.announcements_button =
		tools.shared_button = tools.search_button =
			tools.screenshot_button = (Rect){ 0 };
	tools.cancel = (Rect){ x + 18, foot, 100, 36 };
	ui_draw_button(tools.cancel, "Close", FALSE);

	if (tools.page == MENU) {
		tools.announcements_button =
			(Rect){ x + 18, y + 59, w - 36, 40 };
		ui_draw_button(tools.announcements_button, "Announcements",
			       FALSE);
		tools.shared_button = (Rect){ x + 18, y + 110, w - 36, 40 };
		ui_draw_button(tools.shared_button, "Shared content", FALSE);
		tools.search_button = (Rect){ x + 18, y + 161, w - 36, 40 };
		ui_draw_button(tools.search_button, "Search all chats", FALSE);
		if (ui.photos) {
			tools.screenshot_button =
				(Rect){ x + 18, y + 212, w - 36, 40 };
			ui_draw_button(tools.screenshot_button,
				       "Take screenshot", FALSE);
		}
	} else if (tools.page == PUBLISH || tools.page == REMOVE) {
		const char *description =
			tools.page == PUBLISH ?
				"Share a text preview with this chat. LINE stores announcements without Letter Sealing." :
				"Remove this announcement for everyone in the chat?";

		ui_draw_label(description, (Rect){ x + 18, y + 58, w - 36, 59 },
			      15, 500, 0xefeff1, TRUE);
		if (tools.page == PUBLISH)
			ui_draw_label(ui_string(tools.row, "text", ""),
				      (Rect){ x + 18, y + 115, w - 36, 25 }, 13,
				      500, 0xabacb2, FALSE);
		tools.accept = (Rect){ x + w - 130, foot, 112, 36 };
		ui_draw_button(tools.accept,
			       tools.page == PUBLISH ? "Announce" : "Remove",
			       !tools.pending);
	} else {
		int top = y + 55;

		if (tools.page == FORWARD) {
			tools.field = (Rect){ x + 18, top, w - 36, 34 };
			ui_draw_edit(&tools.filter, tools.field, !tools.pending,
				     "Find a destination chat");
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

			ui_draw_box(area, index == tools.selected ? 0x1d675e :
								    0x202024);
			ui_draw_label(ui_string(row,
						tools.page == FORWARD ? "name" :
									"text",
						""),
				      (Rect){ area.x + 10, area.y + 6,
					      area.w - 20, 30 },
				      15, 550, 0xefeff1, FALSE);
		}
		if (!row_count() && !tools.note)
			ui_draw_label(tools.pending ? "Loading…" :
				      tools.page == FORWARD ?
						      "No matching chats." :
						      "No announcements.",
				      (Rect){ x + 18, top + 15, w - 36, 40 },
				      14, 500, 0xabacb2, FALSE);
		Json *selected = item(tools.selected);

		if (selected && (tools.page == FORWARD ||
				 json_object_get_boolean(
					 ui_field(selected, "canRemove")))) {
			tools.accept = (Rect){ x + w - 130, foot, 112, 36 };
			ui_draw_button(tools.accept,
				       tools.page == FORWARD ? "Forward" :
							       "Remove…",
				       !tools.pending);
		}
	}
	if (tools.note)
		ui_draw_label(tools.note,
			      (Rect){ x + 18, foot - 42, w - 36, 39 }, 13, 500,
			      0xf4acb5, TRUE);
	else if (tools.pending && tools.page != ANNOUNCEMENTS)
		ui_draw_label("Working…",
			      (Rect){ x + 18, foot - 37, w - 36, 26 }, 13, 500,
			      0xabacb2, FALSE);
}
