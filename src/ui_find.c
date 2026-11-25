// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_find.h"
#include "ui_lang.h"
#include "ui_theme.h"
#include "ui_friends.h"
#include "ui_motion.h"
#include <string.h>

enum {
	INK = UI_HEADING,
	MUTED = UI_MUTED,
	PANEL = UI_DIALOG
};

enum search_control {
	INPUT,
	SEARCH,
	EARLIER,
	CLOSE
};

struct search_result {
	Json *item;
	PangoLayout *body;
	char *stamp;
	int y, height;
};

static struct {
	char *chat, *applied, *next, *error, *queued;
	Editor edit;
	GArray *results;
	GHashTable *ids;
	GRegex *pattern;
	guint seq, pending, revision, sent_revision;
	int scroll, pixel_height, width;
	guint measured, skipped;
	gboolean done, drag, all;
	enum search_control focus;
	Rect modal, entry, search, more, close, area;
} f;

static void set(char **p, const char *value)
{
	char *copy = g_strdup(value);

	g_free(*p);
	*p = copy;
	ui.dirty = TRUE;
}

static gboolean hit(Rect r, int x, int y)
{
	return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w &&
	       y < r.y + r.h;
}

static void free_result(gpointer ptr)
{
	struct search_result *r = ptr;

	json_object_put(r->item);
	g_clear_object(&r->body);
	g_free(r->stamp);
}

static void clear_results(void)
{
	if (f.results)
		g_array_set_size(f.results, 0);
	if (f.ids)
		g_hash_table_remove_all(f.ids);
	g_clear_pointer(&f.pattern, g_regex_unref);
	g_clear_pointer(&f.next, g_free);
	f.pixel_height = f.scroll = f.width = 0;
	f.measured = 0;
	f.skipped = 0;
	f.done = FALSE;
}

void ui_find_reset(void)
{
	clear_results();
	g_clear_pointer(&f.ids, g_hash_table_unref);
	g_clear_pointer(&f.results, g_array_unref);
	g_free(f.chat);
	g_free(f.applied);
	g_free(f.error);
	g_free(f.queued);
	if (f.edit.value)
		g_string_free(f.edit.value, TRUE);
	guint seq = f.seq;

	memset(&f, 0, sizeof(f));
	f.seq = seq;
}

gboolean ui_find_active(void)
{
	return f.chat != NULL;
}

void ui_find_resume_input(void)
{
	if (f.focus == INPUT)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

static void focus(enum search_control item)
{
	f.focus = item;
	set(&ui.composition, "");
	ui_find_resume_input();
}

static void open_find(gboolean all)
{
	if (!ui.connected || !ui.find_messages ||
	    (!all && (!ui.selected || !ui_chat_visible())))
		return;
	ui_find_reset();
	f.all = all;
	f.chat = g_strdup(all ? "" : ui.selected);
	f.edit.value = g_string_new("");
	f.results = g_array_new(FALSE, FALSE, sizeof(struct search_result));
	g_array_set_clear_func(f.results, free_result);
	f.ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	focus(INPUT);
}

void ui_find_open(void)
{
	open_find(FALSE);
}

void ui_find_open_all(void)
{
	open_find(TRUE);
}

static void close_find(void)
{
	ui_find_reset();
	ui_restore_input();
	ui.dirty = TRUE;
}

static void request(gboolean more)
{
	g_autofree char *query = g_strdup(f.edit.value->str);

	g_strstrip(query);
	if (!*query) {
		set(&f.error, _("Enter text to search for."));
		return;
	}
	if (strlen(query) > 1024) {
		set(&f.error, _("Enter a search term of up to 1,024 bytes."));
		return;
	}
	if (f.pending) {
		if (!more)
			set(&f.queued, query);
		return;
	}
	if (more && (!f.next || !*f.next || g_strcmp0(f.applied, query)))
		return;
	if (!more) {
		clear_results();
		set(&f.applied, query);
		g_autofree char *escaped = g_regex_escape_string(query, -1);

		f.pattern = g_regex_new(
			escaped, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
	}
	set(&f.error, NULL);
	g_free(f.queued);
	f.queued = NULL;
	Json *p = json_object_new_object();

	if (!f.all)
		json_object_object_add(p, "chat",
				       json_object_new_string(f.chat));
	json_object_object_add(p, "query", json_object_new_string(query));
	if (more)
		json_object_object_add(p, "cursor",
				       json_object_new_string(f.next));
	if (!++f.seq)
		f.seq++;
	f.sent_revision = f.revision;
	f.pending = f.seq;
	ui_rpc(f.all ? "findAllMessages" : "findMessages", p, f.pending);
}

static void edited(const char *previous)
{
	if (!strcmp(previous, f.edit.value->str))
		return;
	f.revision++;
	clear_results();
	set(&f.applied, NULL);
	set(&f.error, NULL);
	set(&f.queued, NULL);
}

gboolean ui_find_reply(const char *method, guint tag, Json *result,
		       const char *error)
{
	gboolean all = !strcmp(method, "findAllMessages");

	if (!all && strcmp(method, "findMessages"))
		return FALSE;
	if (!ui_find_active() || !f.pending || f.all != all || tag != f.pending)
		return TRUE;
	f.pending = 0;
	ui.dirty = TRUE;
	if (f.sent_revision != f.revision) {
		if (f.queued)
			request(FALSE);
		return TRUE;
	}
	g_clear_pointer(&f.queued, g_free);
	Json *matches = ui_field(result, "matches");

	if (error || !json_object_is_type(matches, json_type_array)) {
		set(&f.error,
		    error ? error : _("Could not read the search results."));
		return TRUE;
	}
	for (size_t i = 0; i < json_object_array_length(matches); i++) {
		Json *row = json_object_array_get_idx(matches, i);
		const char *id = ui_string(row, "id", "");
		const char *chat =
			ui_string(ui_field(row, "conversation"), "id", "");
		char *key = f.all ? g_strdup_printf("%s/%s", chat, id) :
				    g_strdup(id);

		if (!*id || (f.all && !*chat)) {
			g_free(key);
			continue;
		}
		if (!g_hash_table_add(f.ids, key))
			continue;
		struct search_result r = { .item = json_object_get(row) };
		gint64 when = json_object_get_int64(ui_field(row, "time"));

		g_autoptr(GDateTime) time =
			when > 0 ?
				g_date_time_new_from_unix_local(when / 1000) :
				NULL;
		if (time)
			r.stamp = ui_lang_date(time, "%e %b %Y · %H:%M");
		g_array_append_val(f.results, r);
	}
	f.skipped += MAX(0, json_object_get_int(ui_field(result, "skipped")));
	set(&f.next, ui_string(result, "next", NULL));
	f.done = TRUE;
	return TRUE;
}

static void highlight(PangoLayout *layout, const char *text)
{
	if (!f.pattern)
		return;
	GMatchInfo *info = NULL;
	gboolean found = g_regex_match(f.pattern, text, 0, &info);

	if (found) {
		PangoAttrList *attrs = pango_attr_list_new();

		do {
			int start, end;

			if (!g_match_info_fetch_pos(info, 0, &start, &end))
				break;
			PangoAttribute *bg = pango_attr_background_new(
				0x5151, 0x5656, 0x9292);
			bg->start_index = start;
			bg->end_index = end;
			pango_attr_list_insert(attrs, bg);
		} while (g_match_info_next(info, NULL));
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
	}
	if (info)
		g_match_info_free(info);
}

static void prepare(int width)
{
	if (f.width != width) {
		f.width = width;
		f.measured = 0;
		f.pixel_height = 0;
	}
	while (f.measured < f.results->len) {
		struct search_result *r = &g_array_index(
			f.results, struct search_result, f.measured++);
		g_clear_object(&r->body);
		const char *text = ui_string(r->item, "text", "");

		if (!*text)
			text = ui_string(r->item, "filename",
					 ui_string(r->item, "type", "Message"));
		r->body = ui_result_layout(text, width - 22);
		highlight(r->body, text);
		int h;

		pango_layout_get_pixel_size(r->body, NULL, &h);
		r->height = 43 + h;
		r->y = f.pixel_height;
		f.pixel_height += r->height;
	}
	f.scroll = CLAMP(f.scroll, 0, MAX(0, f.pixel_height - f.area.h));
}

static guint first_visible(void)
{
	guint lo = 0, hi = f.results->len;

	while (lo < hi) {
		guint mid = lo + (hi - lo) / 2;
		struct search_result *r =
			&g_array_index(f.results, struct search_result, mid);
		if (r->y + r->height <= f.scroll)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void button(Rect r, const char *label, gboolean primary,
		   enum search_control item)
{
	if (f.focus == item)
		ui_draw_box((Rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 },
			    UI_ACCENT);
	ui_draw_button(r, label, primary);
}

void ui_find_draw(void)
{
	if (!ui_find_active())
		return;
	ui_scrim();
	int w = MIN(ui.width - 20, 670), h = MIN(ui.height - 24, 650),
	    x = (ui.width - w) / 2, y = (ui.height - h) / 2;
	f.modal = (Rect){ x, y, w, h };
	ui_draw_box(f.modal, PANEL);
	ui_draw_label(f.all ? _("Search all chats") : _("Search this chat"),
		      (Rect){ x + 17, y + 16, w - 106, 34 }, 21, 650, INK,
		      FALSE);
	f.close = (Rect){ x + w - 73, y + 13, 56, 32 };
	button(f.close, _("Close"), FALSE, CLOSE);
	f.entry = (Rect){ x + 16, y + 60, w - 116, 39 };
	ui_draw_edit(&f.edit, f.entry, f.focus == INPUT, _("Find messages"));
	f.search = (Rect){ x + w - 90, y + 60, 73, 39 };
	button(f.search, _("Search"), TRUE, SEARCH);
	gboolean older = f.next && *f.next;
	int footer = y + h - 18;

	if (f.all) {
		g_autofree char *note =
			f.skipped ?
				g_strdup_printf(_("Could not search %u chats."),
						f.skipped) :
				NULL;

		footer -= 24;
		ui_draw_label(
			note ? note :
			       _("Searches the latest 50 messages per chat."),
			(Rect){ x + 17, footer, w - 34, 23 }, 12, 450, MUTED,
			FALSE);
	}
	f.more = (Rect){ 0 };
	if (older) {
		f.more = (Rect){ x + 16, footer - 35, 163, 35 };
		button(f.more,
		       f.all ? _("Search more chats") : _("Search earlier"),
		       FALSE, EARLIER);
		footer -= 49;
	}
	const char *note = f.error   ? f.error :
			   f.pending ? _("Searching…") :
			   !f.done   ? "" :
			   !f.results->len ?
				     (older ? _("No matches in this batch.") :
					      _("No matching messages.")) :
				     "";
	if (*note) {
		ui_draw_label(note, (Rect){ x + 17, y + 114, w - 34, 50 }, 14,
			      450, MUTED, TRUE);
	}
	int top = y + (*note ? 160 : 111);

	f.area = (Rect){ x + 16, top, w - 32, MAX(10, footer - top) };
	prepare(f.area.w);
	for (guint i = first_visible(); i < f.results->len; i++) {
		struct search_result *r =
			&g_array_index(f.results, struct search_result, i);
		int ry = top + r->y - f.scroll;

		if (ry + r->height < top)
			continue;
		if (ry >= footer)
			break;
		if (ry >= top && ry + 22 <= footer) {
			const char *name =
				ui_string(r->item, "from",
					  json_object_get_boolean(
						  ui_field(r->item, "own")) ?
						  ui.name :
						  "LINE");
			const char *chat =
				ui_string(ui_field(r->item, "conversation"),
					  "name", _("Chat"));
			g_autofree char *context =
				f.all && g_strcmp0(chat, name) ?
					g_strdup_printf("%s · %s", chat, name) :
					NULL;

			ui_draw_label(context ? context : name,
				      (Rect){ x + 24, ry, w - 193, 24 }, 15,
				      650, INK, FALSE);
			if (r->stamp)
				ui_draw_label(r->stamp,
					      (Rect){ x + w - 158, ry + 3, 140,
						      20 },
					      12, 450, MUTED, FALSE);
		}
		ui_result_paint(r->body, x + 24, ry + 23, f.area);
		if (ry + r->height - 9 >= top && ry + r->height - 9 < footer)
			ui_draw_box((Rect){ x + 18, ry + r->height - 9, w - 36,
					    1 },
				    UI_LINE);
	}
}

static void scroll(int delta)
{
	int pos = CLAMP(f.scroll + delta, 0, MAX(0, f.pixel_height - f.area.h));

	if (pos != f.scroll) {
		f.scroll = pos;
		ui.dirty = TRUE;
	}
}

static void tab(gboolean back)
{
	enum search_control order[4] = { INPUT, SEARCH, EARLIER, CLOSE };

	int n = 4;

	if (!f.next || !*f.next) {
		order[2] = CLOSE;
		n = 3;
	}
	int i = 0;

	while (i < n && order[i] != f.focus)
		i++;
	focus(order[(i + n + (back ? -1 : 1)) % n]);
}

static void open_result(int y)
{
	for (guint i = first_visible(); i < f.results->len; i++) {
		struct search_result *r =
			&g_array_index(f.results, struct search_result, i);
		int row_y = r->y - f.scroll + f.area.y;

		if (y < row_y || row_y >= f.area.y + f.area.h)
			break;
		if (y < row_y + r->height) {
			const char *id = ui_string(
				ui_field(r->item, "conversation"), "id", "");

			for (size_t k = 0;
			     ui.chats && k < json_object_array_length(ui.chats);
			     k++) {
				Json *chat =
					json_object_array_get_idx(ui.chats, k);

				if (!strcmp(id, ui_string(chat, "id", ""))) {
					close_find();
					ui_open_conversation(chat);
					return;
				}
			}
			break;
		}
	}
}

gboolean ui_find_event(SDL_Event *e)
{
	if (!ui_find_active())
		return FALSE;
	switch (e->type) {
	case SDL_KEYDOWN: {
		SDL_Keycode code = e->key.keysym.sym;

		if (f.focus == INPUT && ui.composition && *ui.composition)
			return TRUE;
		if (code == SDLK_ESCAPE) {
			close_find();
			return TRUE;
		}
		if (code == SDLK_TAB) {
			tab(e->key.keysym.mod & KMOD_SHIFT);
			return TRUE;
		}
		if (code == SDLK_RETURN || code == SDLK_KP_ENTER ||
		    (code == SDLK_SPACE && f.focus != INPUT)) {
			if (!e->key.repeat) {
				if (f.focus == CLOSE)
					close_find();
				else
					request(f.focus == EARLIER);
			}
			return TRUE;
		}
		if (code == SDLK_PAGEUP || code == SDLK_PAGEDOWN ||
		    code == SDLK_UP || code == SDLK_DOWN) {
			scroll((code == SDLK_PAGEUP || code == SDLK_PAGEDOWN ?
					f.area.h :
					44) *
			       (code == SDLK_UP || code == SDLK_PAGEUP ? -1 :
									 1));
			return TRUE;
		}
		if (f.focus == INPUT) {
			g_autofree char *before = g_strdup(f.edit.value->str);

			ui_edit_key(&f.edit, e->key);
			edited(before);
		}
		return TRUE;
	}
	case SDL_TEXTINPUT:
		if (f.focus == INPUT) {
			g_autofree char *before = g_strdup(f.edit.value->str);

			ui_insert(&f.edit, e->text.text);
			edited(before);
		}
		return TRUE;
	case SDL_TEXTEDITING:
		if (f.focus == INPUT)
			set(&ui.composition, e->edit.text);
		return TRUE;
	case SDL_MOUSEBUTTONDOWN: {
		int x = e->button.x, y = e->button.y;

		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (hit(f.close, x, y) || !hit(f.modal, x, y))
			close_find();
		else if (hit(f.search, x, y))
			request(FALSE);
		else if (hit(f.more, x, y))
			request(TRUE);
		else if (f.all && hit(f.area, x, y))
			open_result(y);
		else if (hit(f.entry, x, y)) {
			focus(INPUT);
			f.edit.cursor = f.edit.anchor =
				ui_edit_hit(&f.edit, f.entry, x);
			f.drag = TRUE;
		}
		return TRUE;
	}
	case SDL_MOUSEMOTION:
		if (f.drag && (e->motion.state & SDL_BUTTON_LMASK)) {
			int pos = ui_edit_hit(&f.edit, f.entry, e->motion.x);

			if (pos != f.edit.cursor) {
				f.edit.cursor = pos;
				ui.dirty = TRUE;
			}
		}
		return TRUE;
	case SDL_MOUSEBUTTONUP:
		f.drag = FALSE;
		return TRUE;
	case SDL_MOUSEWHEEL:
		scroll((int)lround(-48. * ui_wheel_y(&e->wheel)));
		return TRUE;
	case SDL_DROPFILE:
		SDL_free(e->drop.file);
		return TRUE;
	default:
		return FALSE;
	}
}
