// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_chat_lists.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include "ui_sounds.h"
#include "ui_motion.h"
#include <string.h>

enum pane {
	CLOSED,
	VIEWS,
	ORGANIZE,
	PREFS,
	CREATE,
	RENAME,
	REMOVE,
	CAPTURE
};

enum action {
	FILTER,
	FOLDER,
	NEW,
	EDIT,
	DELETE,
	PIN,
	MUTE,
	PREF,
	ALERT,
	BIND
};

struct choice {
	enum action what;
	char *label, *key;
	int command;
};

static struct {
	struct chat_lists *store;
	enum pane pane;
	GPtrArray *rows;
	int first, cursor, binding;
	char *chat, *name, *note;
	Editor edit;
	Rect box, rows_box, close, submit, edit_box;
} panel;

static const struct {
	const char *id, *name;
} builtins[] = { { "all", N_("All chats") },
		 { "unread", N_("Unread") },
		 { "pinned", N_("Pinned") },
		 { "people", N_("People") },
		 { "groups", N_("Groups") } };

static const char *const commands[] = { [LIST_FOLDERS] = N_("Chat folders"),
					[LIST_PIN] = N_("Pin or unpin chat"),
					[LIST_SEARCH] = N_("Find a chat"),
					[LIST_FIND] = N_("Find messages"),
					[LIST_REFRESH] = N_("Refresh chats") };

static void free_choice(gpointer value)
{
	struct choice *c = value;

	g_free(c->key);
	g_free(c->label);
	g_free(c);
}

static void clear_panel(void)
{
	g_clear_pointer(&panel.rows, g_ptr_array_unref);
	g_clear_pointer(&panel.chat, g_free);
	g_clear_pointer(&panel.name, g_free);
	g_clear_pointer(&panel.note, g_free);
	if (panel.edit.value)
		g_string_free(panel.edit.value, TRUE);
	panel.edit = (Editor){ 0 };
	panel.pane = CLOSED;
	panel.first = panel.cursor = 0;
}

void ui_lists_reset(void)
{
	clear_panel();
	g_clear_pointer(&panel.store, chat_lists_free);
}

void ui_lists_login(const char *account)
{
	ui_lists_reset();
	g_autoptr(GError) error = NULL;
	panel.store = chat_lists_open(account, &error);
	if (error)
		ui_status(error->message);
	ui_motion_choose(chat_motion(panel.store));
}

gboolean ui_lists_active(void)
{
	return panel.pane != CLOSED;
}

gboolean ui_lists_ready(void)
{
	return panel.store != NULL;
}

gboolean ui_list_pinned(const char *id)
{
	return chat_pinned(panel.store, id);
}

gboolean ui_notifications_enabled(void)
{
	return panel.store && chat_notifications(panel.store);
}

const char *ui_folder_title(void)
{
	const char *view = chat_current_view(panel.store);

	if (g_str_has_prefix(view, "folder:"))
		return view + 7;
	for (guint i = 0; i < G_N_ELEMENTS(builtins); i++)
		if (!strcmp(view, builtins[i].id))
			return _(builtins[i].name);
	return _("All chats");
}

static gboolean matches(Json *row, const char *id)
{
	const char *view = chat_current_view(panel.store);

	if (!strcmp(view, "unread"))
		return json_object_get_int64(ui_field(row, "unread")) > 0;
	if (!strcmp(view, "pinned"))
		return ui_list_pinned(id);
	if (!strcmp(view, "groups"))
		return *id == 'c' || *id == 'r';
	if (!strcmp(view, "people"))
		return *id == 'u';
	if (g_str_has_prefix(view, "folder:"))
		return chat_folder_contains(panel.store, view + 7, id);
	return TRUE;
}

void ui_lists_rebuild(void)
{
	g_array_set_size(ui.visible, 0);
	ui.list_cursor = -1;
	gboolean has_pins = chat_has_pins(panel.store);

	g_autoptr(GArray) others = NULL;
	size_t n = ui.chats ? json_object_array_length(ui.chats) : 0;

	for (size_t i = 0; i < n; i++) {
		Json *r = json_object_array_get_idx(ui.chats, i);
		const char *id = ui_string(r, "id", "");

		if (!*id || !matches(r, id) ||
		    (ui.search.value->len &&
		     !g_str_match_string(ui.search.value->str,
					 ui_string(r, "name", id), TRUE)))
			continue;
		guint at = i;

		if (!has_pins || ui_list_pinned(id))
			g_array_append_val(ui.visible, at);
		else {
			if (!others)
				others = g_array_sized_new(FALSE, FALSE,
							   sizeof(guint), n);
			g_array_append_val(others, at);
		}
	}
	if (others)
		g_array_append_vals(ui.visible, others->data, others->len);
	for (guint i = 0; ui.selected && i < ui.visible->len; i++) {
		Json *row = json_object_array_get_idx(
			ui.chats, g_array_index(ui.visible, guint, i));
		if (!g_strcmp0(ui.selected, ui_string(row, "id", NULL))) {
			ui.list_cursor = i;
			break;
		}
	}
}

/* A friend opened from search can be outside the selected folder. */
void ui_lists_reveal(const char *id)
{
	for (size_t i = 0; ui.chats && i < json_object_array_length(ui.chats);
	     i++) {
		Json *r = json_object_array_get_idx(ui.chats, i);

		if (!g_strcmp0(id, ui_string(r, "id", NULL))) {
			if (!matches(r, id)) {
				g_autoptr(GError) e = NULL;
				if (!chat_view_set(panel.store, "all", &e))
					ui_status(e->message);
			}
			return;
		}
	}
}

static gboolean editor(void)
{
	return panel.pane == CREATE || panel.pane == RENAME;
}

void ui_lists_resume_input(void)
{
	if (editor()) {
		int w = MIN(425, ui.width - 24), h = MIN(460, ui.height - 24);

		panel.edit_box = (Rect){ (ui.width - w) / 2 + 16,
					 (ui.height - h) / 2 + 119, w - 32,
					 38 };
		SDL_Rect r = { panel.edit_box.x, panel.edit_box.y,
			       panel.edit_box.w, panel.edit_box.h };
		SDL_SetTextInputRect(&r);
		SDL_StartTextInput();
	} else
		SDL_StopTextInput();
}

static void add(enum action what, const char *label, const char *key, int cmd)
{
	struct choice *row = g_new0(struct choice, 1);

	row->what = what;
	row->label = g_strdup(label);
	row->key = g_strdup(key);
	row->command = cmd;
	g_ptr_array_add(panel.rows, row);
}

static void choices(void)
{
	g_clear_pointer(&panel.rows, g_ptr_array_unref);
	panel.rows = g_ptr_array_new_with_free_func(free_choice);
	const char *view = chat_current_view(panel.store);

	if (panel.pane == PREFS) {
		add(ALERT,
		    ui_notifications_enabled() ?
			    _("Desktop notifications · On") :
			    _("Desktop notifications · Off"),
		    NULL, 0);
		for (int i = 0; i < LIST_COMMANDS; i++) {
			g_autofree char *label =
				g_strdup_printf("%s    %s", _(commands[i]),
						chat_binding(panel.store, i));
			add(BIND, label, NULL, i);
		}
		return;
	}
	if (panel.pane != VIEWS && panel.pane != ORGANIZE)
		return;
	if (panel.pane == VIEWS) {
		for (guint i = 0; i < G_N_ELEMENTS(builtins); i++)
			add(FILTER, _(builtins[i].name), builtins[i].id, 0);
	} else {
		add(PIN,
		    ui_list_pinned(panel.chat) ? _("Unpin chat") :
						 _("Pin chat"),
		    NULL, 0);
		add(MUTE,
		    chat_muted(panel.store, panel.chat) ?
			    _("Unmute conversation") :
			    _("Mute conversation"),
		    NULL, 0);
	}
	Json *folders = chat_folders(panel.store);

	if (folders) {
		json_object_object_foreach(folders, name, set) {
			(void)set;
			add(FOLDER, name, name, 0);
		}
	}
	add(NEW, _("New folder…"), NULL, 0);
	if (panel.pane == VIEWS) {
		if (g_str_has_prefix(view, "folder:")) {
			add(EDIT, _("Rename current folder…"), NULL, 0);
			add(DELETE, _("Delete current folder…"), NULL, 0);
		}
		add(PREF, _("Preferences & shortcuts…"), NULL, 0);
	}
}

static void show(enum pane pane)
{
	panel.pane = pane;
	g_clear_pointer(&ui.composition, g_free);
	panel.first = panel.cursor = 0;
	g_clear_pointer(&panel.note, g_free);
	choices();
	ui_lists_resume_input();
	ui.dirty = TRUE;
}

static void close_panel(void)
{
	clear_panel();
	ui_restore_input();
	ui.dirty = TRUE;
}

void ui_lists_open(void)
{
	if (panel.store)
		show(VIEWS);
}

static gboolean report(GError *e)
{
	if (!e)
		return TRUE;
	g_free(panel.note);
	panel.note = g_strdup(e->message);
	ui.dirty = TRUE;
	if (!ui_lists_active())
		ui_status(e->message);
	return FALSE;
}

static void activate(struct choice *c)
{
	g_autoptr(GError) e = NULL;
	const char *view = chat_current_view(panel.store);

	switch (c->what) {
	case FOLDER:
		if (panel.pane == ORGANIZE) {
			chat_folder_toggle(panel.store, c->key, panel.chat, &e);
			if (report(e))
				ui_refilter_chats();
			return;
		}
		G_GNUC_FALLTHROUGH;
	case FILTER: {
		g_autofree char *folder =
			c->what == FOLDER ?
				g_strconcat("folder:", c->key, NULL) :
				NULL;
		chat_view_set(panel.store, folder ? folder : c->key, &e);
		if (report(e)) {
			ui.list_offset = 0;
			ui_refilter_chats();
			close_panel();
		}
		return;
	}
	case MUTE:
		chat_mute_toggle(panel.store, panel.chat, &e);
		if (report(e))
			choices();
		ui.dirty = TRUE;
		return;
	case PIN:
		chat_pin_toggle(panel.store, panel.chat, &e);
		if (report(e)) {
			choices();
			ui_refilter_chats();
		}
		return;
	case EDIT:
	case NEW:
		if (!panel.edit.value)
			panel.edit.value = g_string_new("");
		g_string_assign(panel.edit.value,
				c->what == EDIT ? view + 7 : "");
		panel.edit.cursor = panel.edit.anchor = panel.edit.value->len;
		show(c->what == EDIT ? RENAME : CREATE);
		return;
	case DELETE:
		show(REMOVE);
		return;
	case PREF:
		show(PREFS);
		return;
	case ALERT:
		chat_notifications_set(panel.store, !ui_notifications_enabled(),
				       &e);
		if (report(e))
			choices();
		ui.dirty = TRUE;
		return;
	case BIND:
		panel.binding = c->command;
		show(CAPTURE);
		return;
	}
}

static void accept(void)
{
	g_autoptr(GError) e = NULL;
	const char *view = chat_current_view(panel.store);

	if (panel.pane == REMOVE)
		chat_folder_set(panel.store, view + 7, NULL, NULL, &e);
	else if (editor())
		chat_folder_set(panel.store,
				panel.pane == RENAME ? view + 7 : NULL,
				panel.edit.value->str,
				panel.pane == CREATE ? panel.chat : NULL, &e);
	else if (panel.rows && panel.cursor >= 0 &&
		 (guint)panel.cursor < panel.rows->len) {
		activate(g_ptr_array_index(panel.rows, panel.cursor));
		return;
	} else
		return;
	if (report(e)) {
		show(panel.chat ? ORGANIZE : VIEWS);
		ui_refilter_chats();
	}
}

static gboolean hit(Rect r, int x, int y)
{
	return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y &&
	       y < r.y + r.h;
}

static void back(void)
{
	if (panel.pane == CAPTURE)
		show(PREFS);
	else if (editor() || panel.pane == REMOVE)
		show(panel.chat ? ORGANIZE : VIEWS);
	else if (panel.pane == PREFS)
		show(VIEWS);
	else
		close_panel();
}

static int slots(void)
{
	return MAX(1, panel.rows_box.h / 38);
}

static gboolean chord(SDL_KeyboardEvent e, char text[40])
{
	SDL_Keycode code = e.keysym.sym;
	unsigned int mod = e.keysym.mod;
	gboolean letter = (code >= SDLK_a && code <= SDLK_z),
		 digit = (code >= SDLK_0 && code <= SDLK_9),
		 fn = code >= SDLK_F1 && code <= SDLK_F12;
	if (mod & (KMOD_GUI | KMOD_MODE) || !(letter || digit || fn) ||
	    (!fn && !(mod & (KMOD_CTRL | KMOD_ALT))))
		return FALSE;
	g_snprintf(text, 40, "%s%s%s%s", mod & KMOD_CTRL ? "Ctrl+" : "",
		   mod & KMOD_ALT ? "Alt+" : "",
		   mod & KMOD_SHIFT ? "Shift+" : "", SDL_GetKeyName(code));
	return TRUE;
}

int ui_list_shortcut(SDL_KeyboardEvent key)
{
	char value[40];

	if (!chord(key, value))
		return -1;
	for (int i = 0; i < LIST_COMMANDS; i++)
		if (!strcmp(chat_binding(panel.store, i), value))
			return i;
	return -1;
}

static void capture(SDL_KeyboardEvent key)
{
	static const char *const reserved[] = {
		"Ctrl+A", "Ctrl+C", "Ctrl+V", "Ctrl+X", "Ctrl+E", "Ctrl+O",
		"Alt+A",  "Alt+D",  "Alt+M",  "Alt+F4", "Ctrl+2", "Shift+F10"
	};
	char value[40];

	if (!chord(key, value))
		return;
	for (guint i = 0; i < G_N_ELEMENTS(reserved); i++)
		if (!strcmp(value, reserved[i])) {
			g_free(panel.note);
			panel.note = g_strdup(_(
				"This shortcut is used by the editor, calls or desktop."));
			ui.dirty = TRUE;
			return;
		}
	g_autoptr(GError) error = NULL;
	chat_binding_set(panel.store, panel.binding, value, &error);
	if (report(error))
		show(PREFS);
}

static Json *cursor_row(void)
{
	if (ui.focus == F_LIST && ui.visible && ui.list_cursor >= 0 &&
	    (guint)ui.list_cursor < ui.visible->len)
		return json_object_array_get_idx(
			ui.chats,
			g_array_index(ui.visible, guint, ui.list_cursor));
	return ui_selected_chat();
}

static void organize(Json *row)
{
	if (!row)
		return;
	g_free(panel.chat);
	g_free(panel.name);
	panel.chat = g_strdup(ui_string(row, "id", ""));
	panel.name = g_strdup(ui_string(row, "name", "Chat"));
	show(ORGANIZE);
}

gboolean ui_lists_event(SDL_Event *e)
{
	if (!panel.store)
		return FALSE;
	if (panel.pane == CLOSED) {
		if (e->type == SDL_MOUSEBUTTONDOWN && ui_list_visible()) {
			int x = e->button.x, y = e->button.y;

			if (e->button.button == SDL_BUTTON_LEFT &&
			    hit(ui.folders_button, x, y)) {
				ui_lists_open();
				return TRUE;
			}
			if (e->button.button == SDL_BUTTON_RIGHT &&
			    hit(ui.list_box, x, y)) {
				guint i = (y - ui.list_box.y + ui.list_offset) /
					  ui_chat_row_height();
				if (i < ui.visible->len)
					organize(json_object_array_get_idx(
						ui.chats,
						g_array_index(ui.visible, guint,
							      i)));
				return TRUE;
			}
		}
		if (e->type != SDL_KEYDOWN || e->key.repeat ||
		    (ui.composition && *ui.composition))
			return FALSE;
		if ((e->key.keysym.sym == SDLK_F10 &&
		     e->key.keysym.mod & KMOD_SHIFT) ||
		    e->key.keysym.sym == SDLK_APPLICATION) {
			organize(cursor_row());
			return TRUE;
		}
		int cmd = ui_list_shortcut(e->key);

		if (cmd == LIST_FOLDERS) {
			ui_lists_open();
			return TRUE;
		}
		if (cmd == LIST_PIN) {
			Json *r = cursor_row();

			if (r) {
				g_autoptr(GError) err = NULL;
				chat_pin_toggle(panel.store,
						ui_string(r, "id", ""), &err);
				if (report(err))
					ui_refilter_chats();
			}
			return TRUE;
		}
		return FALSE;
	}
	if (e->type == SDL_DROPFILE) {
		SDL_free(e->drop.file);
		return TRUE;
	}
	if (e->type == SDL_KEYDOWN) {
		SDL_Keycode k = e->key.keysym.sym;

		if (k == SDLK_ESCAPE)
			back();
		else if (panel.pane == CAPTURE) {
			if (!e->key.repeat)
				capture(e->key);
		} else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) &&
			   !e->key.repeat &&
			   !(ui.composition && *ui.composition))
			accept();
		else if (editor())
			ui_edit_key(&panel.edit, e->key);
		else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_PAGEUP ||
			 k == SDLK_PAGEDOWN) {
			int step = (k == SDLK_UP || k == SDLK_DOWN) ? 1 :
								      slots();
			panel.cursor = CLAMP(
				panel.cursor +
					(k == SDLK_UP || k == SDLK_PAGEUP ?
						 -step :
						 step),
				0, MAX(0, (int)panel.rows->len - 1));
			panel.first = CLAMP(panel.first,
					    panel.cursor - slots() + 1,
					    panel.cursor);
			ui.dirty = TRUE;
		}
		return TRUE;
	}
	if (e->type == SDL_TEXTINPUT) {
		if (editor() && panel.edit.value->len < 256)
			ui_insert(&panel.edit, e->text.text);
		return TRUE;
	}
	if (e->type == SDL_TEXTEDITING) {
		if (editor()) {
			g_free(ui.composition);
			ui.composition = g_strdup(e->edit.text);
			ui.dirty = TRUE;
		}
		return TRUE;
	}
	if (e->type == SDL_MOUSEWHEEL) {
		static double carry;
		int max = MAX(0, (int)panel.rows->len - slots());
		int rows = ui_wheel_rows(ui_wheel_y(&e->wheel), &carry);

		panel.first = CLAMP(panel.first - 2 * rows, 0, max);
		ui.dirty = TRUE;
		return TRUE;
	}
	if (e->type == SDL_MOUSEBUTTONDOWN) {
		int x = e->button.x, y = e->button.y;

		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (!hit(panel.box, x, y) || hit(panel.close, x, y))
			back();
		else if (hit(panel.submit, x, y))
			accept();
		else if (hit(panel.rows_box, x, y)) {
			int row = panel.first + (y - panel.rows_box.y) / 38;

			if (row < (int)panel.rows->len) {
				panel.cursor = row;
				activate(g_ptr_array_index(panel.rows, row));
			}
		}
		return TRUE;
	}
	return e->type == SDL_MOUSEMOTION;
}

void ui_lists_draw(void)
{
	const guint ink = UI_HEADING, muted = UI_MUTED;
	int w = MIN(425, ui.width - 24), h = MIN(460, ui.height - 24),
	    x = (ui.width - w) / 2, y = (ui.height - h) / 2;
	panel.box = (Rect){ x, y, w, h };
	panel.rows_box = (Rect){ 0 };
	panel.submit = (Rect){ 0 };
	ui_scrim();
	ui_draw_box(panel.box, UI_DIALOG);
	const char *title = panel.pane == ORGANIZE ? panel.name :
			    panel.pane == PREFS	   ? _("Preferences") :
			    panel.pane == CAPTURE  ? _("Keyboard shortcut") :
			    panel.pane == CREATE   ? _("New folder") :
			    panel.pane == RENAME   ? _("Rename folder") :
			    panel.pane == REMOVE   ? _("Delete folder?") :
						     _("Chat folders");
	ui_draw_label(title, (Rect){ x + 16, y + 17, w - 105, 34 }, 21, 650,
		      ink, FALSE);
	panel.close = (Rect){ x + w - 74, y + 12, 58, 32 };
	ui_draw_button(panel.close,
		       (panel.pane == ORGANIZE || panel.pane == VIEWS) ?
			       _("Done") :
			       _("Back"),
		       FALSE);
	if (editor() || panel.pane == REMOVE || panel.pane == CAPTURE) {
		const char *label =
			editor() ?
				_("Folder name") :
			panel.pane == REMOVE ?
				_("Chats in this folder will stay in All chats.") :
				_(commands[panel.binding]);
		ui_draw_label(label, (Rect){ x + 18, y + 73, w - 36, 48 }, 15,
			      500, ink, TRUE);
		if (editor()) {
			panel.edit_box = (Rect){ x + 16, y + 119, w - 32, 38 };
			ui_draw_path_entry(&panel.edit, panel.edit_box);
		} else if (panel.pane == CAPTURE)
			ui_draw_label(
				_("Press Ctrl or Alt with a letter or number, or a function key."),
				(Rect){ x + 18, y + 123, w - 36, 70 }, 15, 400,
				muted, TRUE);
		if (panel.pane != CAPTURE) {
			panel.submit =
				(Rect){ x + w - 122, y + h - 55, 106, 38 };
			ui_draw_button(
				panel.submit,
				panel.pane == REMOVE ? _("Delete folder") :
				panel.pane == CREATE ? _("Create folder") :
						       _("Save name"),
				TRUE);
		}
	} else {
		int rows = MAX(1, (h - 112) / 38);

		panel.rows_box = (Rect){ x + 12, y + 58, w - 24, rows * 38 };
		const char *view = chat_current_view(panel.store);

		for (int i = 0;
		     i < rows && i + panel.first < (int)panel.rows->len; i++) {
			struct choice *c =
				g_ptr_array_index(panel.rows, i + panel.first);
			Rect r = { panel.rows_box.x, panel.rows_box.y + 38 * i,
				   panel.rows_box.w, 36 };
			gboolean checked =
				(c->what == FILTER && !strcmp(c->key, view)) ||
				(c->what == FOLDER &&
				 (panel.pane == ORGANIZE ?
					  chat_folder_contains(panel.store,
							       c->key,
							       panel.chat) :
					  g_str_has_prefix(view, "folder:") &&
						  !strcmp(view + 7, c->key)));
			if (i + panel.first == panel.cursor)
				ui_draw_box(r, UI_SELECTED);
			ui_draw_label(c->label,
				      (Rect){ r.x + 8, r.y + 7, r.w - 43,
					      r.h - 5 },
				      15, 500, ink, FALSE);
			if (checked)
				ui_draw_label("✓",
					      (Rect){ r.x + r.w - 30, r.y + 7,
						      25, 24 },
					      15, 650, UI_ACCENT, FALSE);
		}
	}
	if (!panel.note && panel.pane == VIEWS)
		ui_draw_label(
			_("Right-click a chat to pin it or choose its folders."),
			(Rect){ x + 18, y + h - 40, w - 36, 34 }, 13, 450,
			muted, TRUE);
	if (panel.note)
		ui_draw_label(panel.note,
			      (Rect){ x + 16, y + h - 103, w - 32, 47 }, 13,
			      450, muted, TRUE);
}

gboolean ui_chat_muted(const char *chat)
{
	return chat_muted(panel.store, chat);
}

gboolean ui_notifications_set(gboolean on)
{
	g_autoptr(GError) e = NULL;

	chat_notifications_set(panel.store, on, &e);
	ui.dirty = TRUE;
	return report(e);
}

gboolean ui_sounds_enabled(void)
{
	return panel.store && chat_sounds(panel.store);
}

gboolean ui_sounds_set(gboolean on)
{
	g_autoptr(GError) e = NULL;

	chat_sounds_set(panel.store, on, &e);
	if (!on)
		ui_sound_stop_loop();
	ui.dirty = TRUE;
	return report(e);
}

gboolean ui_motion_save(int reduce)
{
	g_autoptr(GError) e = NULL;

	ui_motion_choose(reduce);
	chat_motion_set(panel.store, reduce, &e);
	ui.dirty = TRUE;
	return report(e);
}

void ui_lists_open_preferences(void)
{
	if (panel.store) {
		g_clear_pointer(&panel.chat, g_free);
		show(PREFS);
	}
}
