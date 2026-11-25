// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_shared_content.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include "ui_find.h"
#include "ui_friends.h"
#include "ui_motion.h"
#include <string.h>

enum shared_view {
	ALBUMS,
	NOTES,
	POLLS,
	PHOTOS,
	NOTE_FORM,
	DETAIL
};

enum shared_control {
	FIRST_TAB,
	NOTES_TAB,
	POLLS_TAB,
	BODY,
	NEW,
	MORE,
	CLOSE,
	N_CONTROLS
};

static struct {
	char *chat, *album, *headline, *next, *notice;
	Editor edit;
	Json *rows;
	PangoLayout *detail;
	int detail_width, detail_scroll, detail_height;
	enum shared_view page, return_page;
	enum shared_control focus;
	guint serial, pending;
	gboolean writing;
	int index, first;
	Rect panel, tabs[3], body, extra, more, back;
} view;

enum {
	PANEL = UI_DIALOG,
	INK = UI_HEADING,
	MUTED = UI_MUTED,
	FOCUS = UI_ACCENT
};

static void set(char **slot, const char *s)
{
	char *copy = g_strdup(s);

	g_free(*slot);
	*slot = copy;
	ui.dirty = TRUE;
}

static gboolean hit(Rect r, int x, int y)
{
	return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w &&
	       y < r.y + r.h;
}

void ui_shared_reset(void)
{
	g_free(view.chat);
	g_free(view.album);
	g_free(view.headline);
	g_free(view.next);
	g_free(view.notice);
	json_object_put(view.rows);
	g_clear_object(&view.detail);
	if (view.edit.value)
		g_string_free(view.edit.value, TRUE);
	guint serial = view.serial;

	memset(&view, 0, sizeof(view));
	view.serial = serial;
	ui.dirty = TRUE;
}

gboolean ui_shared_active(void)
{
	return view.chat != NULL;
}

void ui_shared_resume_input(void)
{
	if (view.page == NOTE_FORM && view.focus == BODY)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

static void focus(enum shared_control f)
{
	view.focus = f;
	set(&ui.composition, "");
	ui_shared_resume_input();
}

static void clear_items(void)
{
	json_object_put(view.rows);
	view.rows = json_object_new_array();
	view.index = view.first = 0;
	set(&view.next, NULL);
	set(&view.notice, NULL);
	g_clear_object(&view.detail);
}

static int count(void)
{
	return view.rows ? (int)json_object_array_length(view.rows) : 0;
}

static Json *selected(void)
{
	return view.index >= 0 && view.index < count() ?
		       json_object_array_get_idx(view.rows, view.index) :
		       NULL;
}

static void rpc(const char *method, Json *p)
{
	view.pending = ++view.serial;
	if (!view.pending)
		view.pending = ++view.serial;
	json_object_object_add(p, "chat", json_object_new_string(view.chat));
	ui_rpc(method, p, view.pending);
	ui.dirty = TRUE;
}

static void list(enum shared_view page, gboolean more)
{
	if (view.writing || (more && (view.pending || !view.next)))
		return;
	if (!more)
		clear_items();
	view.page = page;
	focus(BODY);
	Json *p = json_object_new_object();

	if (more)
		json_object_object_add(p, "cursor",
				       json_object_new_string(view.next));
	if (page == PHOTOS) {
		json_object_object_add(p, "album",
				       json_object_new_string(view.album));
		rpc("sharedPhotos", p);
	} else {
		const char *kind = page == ALBUMS ? "albums" :
				   page == NOTES  ? "notes" :
						    "polls";
		json_object_object_add(p, "kind", json_object_new_string(kind));
		rpc("sharedList", p);
	}
}

void ui_shared_open(void)
{
	if (!ui.connected || !ui.selected || !ui_chat_visible())
		return;
	ui_shared_reset();
	view.chat = g_strdup(ui.selected);
	view.edit.value = g_string_new(NULL);
	list(*view.chat == 'c' ? NOTES : ALBUMS, FALSE);
}

static void back(void)
{
	if (view.writing)
		return;
	if (view.page == DETAIL || view.page == NOTE_FORM) {
		view.page = view.return_page;
		g_clear_object(&view.detail);
		focus(BODY);
	} else if (view.page == PHOTOS) {
		list(ALBUMS, FALSE);
	} else {
		ui_shared_reset();
		ui_restore_input();
	}
}

static void open_item(void)
{
	Json *r = selected();

	if (!r || view.writing)
		return;
	const char *id = ui_string(r, "id", "");

	if (view.page == ALBUMS) {
		set(&view.album, id);
		set(&view.headline, ui_string(r, "title", "Album"));
		list(PHOTOS, FALSE);
	} else if (view.page == PHOTOS) {
		if (json_object_get_boolean(ui_field(r, "viewable")))
			ui_media_album_photo(view.album, id);
		else
			set(&view.notice, _("This album item is a video."));
	} else {
		view.return_page = view.page;
		view.page = DETAIL;
		view.detail_scroll = 0;
		focus(BODY);
	}
}

static void new_note(void)
{
	view.return_page = NOTES;
	view.page = NOTE_FORM;
	set(&view.notice, NULL);
	focus(BODY);
}

static void post(void)
{
	if (view.writing || !view.edit.value || !view.edit.value->len)
		return;
	if (view.edit.value->len > 10000) {
		set(&view.notice,
		    _("Shorten this note to 10,000 UTF-8 bytes."));
		return;
	}
	Json *p = json_object_new_object();

	json_object_object_add(p, "text",
			       json_object_new_string(view.edit.value->str));
	view.writing = TRUE;
	rpc("sharedPost", p);
}

gboolean ui_shared_reply(const char *method, guint tag, Json *result,
			 const char *error)
{
	if (strcmp(method, "sharedList") && strcmp(method, "sharedPhotos") &&
	    strcmp(method, "sharedPost"))
		return FALSE;
	if (!ui_shared_active() || view.pending != tag)
		return TRUE;
	view.pending = 0;
	view.writing = FALSE;
	if (error) {
		set(&view.notice, error);
		return TRUE;
	}
	if (!strcmp(method, "sharedPost")) {
		if (!json_object_get_boolean(ui_field(result, "saved"))) {
			set(&view.notice, _("LINE did not confirm the note."));
			return TRUE;
		}
		g_string_truncate(view.edit.value, 0);
		view.edit.cursor = view.edit.anchor = view.edit.scroll = 0;
		list(NOTES, FALSE);
		return TRUE;
	}
	Json *rows = ui_field(result, "items");

	if (!json_object_is_type(rows, json_type_array)) {
		set(&view.notice, _("LINE returned an unreadable page."));
		return TRUE;
	}
	for (size_t i = 0; i < json_object_array_length(rows) && count() < 4096;
	     i++) {
		Json *r = json_object_array_get_idx(rows, i);

		if (json_object_is_type(r, json_type_object))
			json_object_array_add(view.rows, json_object_get(r));
	}
	set(&view.next,
	    count() < 4096 ? ui_string(result, "next", NULL) : NULL);
	set(&view.notice, NULL);
	return TRUE;
}

static void button(Rect r, const char *label, gboolean primary,
		   enum shared_control f)
{
	if (view.focus == f)
		ui_draw_box((Rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 },
			    FOCUS);
	ui_draw_button(r, label, primary);
}

static gboolean is_root(void)
{
	return view.page == ALBUMS || view.page == NOTES || view.page == POLLS;
}

static int slots(void)
{
	return MAX(1, view.body.h / 54);
}

static void position(int index)
{
	view.index = CLAMP(index, 0, MAX(0, count() - 1));
	view.first =
		CLAMP(view.first, MAX(0, view.index - slots() + 1), view.index);
	ui.dirty = TRUE;
}

void ui_shared_draw(void)
{
	if (!ui_shared_active())
		return;
	ui_scrim();
	int w = MIN(596, ui.width - 20), h = MIN(580, ui.height - 26);
	int x = (ui.width - w) / 2, y = (ui.height - h) / 2;

	view.panel = (Rect){ x, y, w, h };
	ui_draw_box(view.panel, PANEL);
	const char *title = view.page == PHOTOS	   ? view.headline :
			    view.page == NOTE_FORM ? _("New group note") :
			    view.page == DETAIL	   ? _("Group note") :
						     _("Shared content");
	ui_draw_label(title, (Rect){ x + 16, y + 16, w - 118, 35 }, 21, 700,
		      INK, FALSE);
	view.back = (Rect){ x + w - 84, y + 13, 70, 33 };
	button(view.back, is_root() ? _("Close") : _("Back"), FALSE, CLOSE);
	int top = y + 65, bottom = y + h - 16;

	view.extra = view.more = (Rect){ 0 };
	memset(view.tabs, 0, sizeof(view.tabs));
	if (is_root()) {
		const char *const labels[] = { _("Albums"), _("Notes"),
					       _("Poll notes") };
		int n = *view.chat == 'c' ? 3 : 1;
		int tab_w = (w - 32 - 6 * (n - 1)) / n;

		for (int i = 0; i < n; i++) {
			view.tabs[i] = (Rect){ x + 16 + i * (tab_w + 6), top,
					       tab_w, 34 };
			button(view.tabs[i], labels[i], (int)view.page == i,
			       FIRST_TAB + i);
		}
		top += 49;
	}
	if (view.page == NOTES || view.page == NOTE_FORM ||
	    (view.notice && !view.writing && !view.pending &&
	     view.page != DETAIL)) {
		const char *label = view.page == NOTE_FORM ? _("Post note") :
				    view.page == NOTES	   ? _("New note") :
							     _("Reload");
		view.extra = (Rect){ x + 16, bottom - 36, 102, 36 };
		button(view.extra, label,
		       view.page == NOTE_FORM || view.page == NOTES, NEW);
	}
	if (view.next && !view.writing && view.page != DETAIL &&
	    view.page != NOTE_FORM) {
		view.more = (Rect){ x + w - 106, bottom - 36, 90, 36 };
		button(view.more, _("More"), FALSE, MORE);
	}
	if (view.more.w || view.extra.w)
		bottom -= 51;
	const char *msg = view.notice;

	if (!msg && view.pending)
		msg = view.writing ? _("Posting…") : _("Loading…");
	if (!msg && view.page == POLLS)
		msg = _("Group notes matching “poll”.");
	if (!msg && view.page == NOTE_FORM)
		msg = _("Everyone in this group can read this note.");
	if (msg) {
		ui_draw_label(msg, (Rect){ x + 16, top, w - 32, 46 }, 14, 450,
			      MUTED, TRUE);
		top += 48;
	}
	view.body = (Rect){ x + 16, top, w - 32, MAX(1, bottom - top) };
	if (view.page == NOTE_FORM) {
		view.body.h = 42;
		ui_draw_edit(&view.edit, view.body, view.focus == BODY,
			     _("Write a group note"));
		return;
	}
	if (view.page == DETAIL) {
		const char *body = ui_string(selected(), "text", "");

		if (!view.detail || view.detail_width != view.body.w) {
			g_clear_object(&view.detail);
			view.detail_width = view.body.w;
			view.detail = ui_result_layout(body, view.body.w - 4);
			pango_layout_get_pixel_size(view.detail, NULL,
						    &view.detail_height);
		}
		view.detail_scroll =
			MIN(view.detail_scroll,
			    MAX(0, view.detail_height - view.body.h));
		ui_result_paint(view.detail, view.body.x + 2,
				view.body.y - view.detail_scroll, view.body);
		return;
	}
	if (!view.pending && !view.notice && !count())
		ui_draw_label(
			view.page == POLLS  ? _("No matching poll notes.") :
			view.page == ALBUMS ? _("No albums on this page.") :
			view.page == PHOTOS ? _("No photos in this album.") :
					      _("No group notes."),
			view.body, 15, 450, MUTED, FALSE);
	for (int i = view.first; i < count() && i < view.first + slots(); i++) {
		Json *r = json_object_array_get_idx(view.rows, i);
		Rect row = { view.body.x, top + (i - view.first) * 54,
			     view.body.w, 49 };
		if (row.y + row.h > bottom)
			break;
		if (i == view.index)
			ui_draw_box(row, UI_SELECTED);
		ui_draw_label(ui_string(r, "title", ""),
			      (Rect){ row.x + 9, row.y + 5, row.w - 18, 22 },
			      15, 650, INK, FALSE);
		ui_draw_label(ui_string(r, "text", ""),
			      (Rect){ row.x + 9, row.y + 27, row.w - 18, 19 },
			      13, 400, MUTED, FALSE);
	}
}

static void activate(void)
{
	switch (view.focus) {
	case FIRST_TAB:
	case NOTES_TAB:
	case POLLS_TAB:
		list((enum shared_view)view.focus, FALSE);
		break;
	case BODY:
		if (view.page != NOTE_FORM && view.page != DETAIL)
			open_item();
		break;
	case NEW:
		if (view.page == NOTE_FORM)
			post();
		else if (view.page == NOTES)
			new_note();
		else
			list(view.page, FALSE);
		break;
	case MORE:
		list(view.page, TRUE);
		break;
	case CLOSE:
		back();
		break;
	default:
		break;
	}
}

static void tab(gboolean reverse)
{
	gboolean enabled[] = { view.tabs[0].w,
			       view.tabs[1].w,
			       view.tabs[2].w,
			       TRUE,
			       view.extra.w,
			       view.more.w,
			       TRUE };
	int i = view.focus;

	do {
		i = (i + N_CONTROLS + (reverse ? -1 : 1)) % N_CONTROLS;
	} while (!enabled[i]);
	focus(i);
}

gboolean ui_shared_event(SDL_Event *e)
{
	if (!ui_shared_active())
		return FALSE;
	switch (e->type) {
	case SDL_KEYDOWN: {
		SDL_Keycode key = e->key.keysym.sym;

		if (view.writing ||
		    (view.page == NOTE_FORM && view.focus == BODY &&
		     ui.composition && *ui.composition))
			return TRUE;
		if (key == SDLK_ESCAPE)
			back();
		else if (key == SDLK_TAB)
			tab(e->key.keysym.mod & KMOD_SHIFT);
		else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) &&
			 !e->key.repeat) {
			if (view.page == NOTE_FORM && view.focus == BODY)
				focus(NEW);
			else
				activate();
		} else if (view.page == NOTE_FORM && view.focus == BODY) {
			ui_edit_key(&view.edit, e->key);
			ui.dirty = TRUE;
		} else if (key == SDLK_UP || key == SDLK_DOWN ||
			   key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) {
			int delta =
				(key == SDLK_UP || key == SDLK_PAGEUP ? -1 :
									1) *
				(key == SDLK_PAGEUP || key == SDLK_PAGEDOWN ?
					 slots() :
					 1);
			if (view.page == DETAIL)
				view.detail_scroll = CLAMP(
					view.detail_scroll + delta * 40, 0,
					MAX(0,
					    view.detail_height - view.body.h));
			else
				position(view.index + delta);
			ui.dirty = TRUE;
		} else if (key == SDLK_SPACE && !e->key.repeat)
			activate();
		return TRUE;
	}
	case SDL_TEXTINPUT:
		if (!view.writing && view.page == NOTE_FORM &&
		    view.focus == BODY) {
			ui_insert(&view.edit, e->text.text);
			ui.dirty = TRUE;
		}
		return TRUE;
	case SDL_TEXTEDITING:
		if (view.page == NOTE_FORM && view.focus == BODY)
			set(&ui.composition, e->edit.text);
		return TRUE;
	case SDL_MOUSEWHEEL: {
		static double carry;
		double steps = ui_wheel_y(&e->wheel);

		if (view.page == DETAIL) {
			view.detail_scroll = CLAMP(
				view.detail_scroll - (int)lround(steps * 70), 0,
				MAX(0, view.detail_height - view.body.h));
			ui.dirty = TRUE;
		} else {
			int rows = ui_wheel_rows(steps, &carry);

			if (rows)
				position(view.index - rows * 2);
		}
		return TRUE;
	}
	case SDL_MOUSEBUTTONDOWN: {
		int x = e->button.x, y = e->button.y;

		if (view.writing || e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (hit(view.back, x, y))
			back();
		else if (hit(view.more, x, y))
			list(view.page, TRUE);
		else if (hit(view.extra, x, y)) {
			focus(NEW);
			activate();
		} else if (hit(view.body, x, y)) {
			focus(BODY);
			if (view.page == NOTE_FORM)
				view.edit.cursor = view.edit.anchor =
					ui_edit_hit(&view.edit, view.body, x);
			else if (view.page != DETAIL) {
				int idx = view.first + (y - view.body.y) / 54;

				if (idx < count() &&
				    idx < view.first + slots()) {
					position(idx);
					open_item();
				}
			}
		} else
			for (int i = 0; i < 3; i++)
				if (hit(view.tabs[i], x, y)) {
					list(i, FALSE);
					break;
				}
		return TRUE;
	}
	case SDL_MOUSEMOTION:
	case SDL_MOUSEBUTTONUP:
	case SDL_KEYUP:
		return TRUE;
	default:
		return FALSE;
	}
}
