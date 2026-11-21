// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_emoji.h"
#include "ui_apng.h"
#include <math.h>
#include <string.h>

#define OBJECT "\xef\xbf\xbc"

enum {
	INK = 0xefeff1,
	MUTED = 0xabacb2,
	MAX_SPANS = 1024
};

struct draft_emoji {
	int pos;
	Json *meta;
};

static GHashTable *drafts;
static guint next_request;

static struct {
	Json *catalog, *preview;
	char *chat, *send_error;
	guint catalog_tag, send_tag;
	char *error, *query, *preedit, *requested_query;
	gboolean loading, stickers;
	int selected, cols, per_page, offset, requested_offset, requested_page;
	Rect panel, grid, prev, next, search, close, emoji_tab, sticker_tab,
		send, back;
} palette;

static void drop_token(gpointer el)
{
	json_object_put(((struct draft_emoji *)el)->meta);
}

static GArray *tokens(const char *chat)
{
	return drafts && chat ? g_hash_table_lookup(drafts, chat) : NULL;
}

void ui_emoji_forget(const char *chat)
{
	if (drafts && chat)
		g_hash_table_remove(drafts, chat);
}

void ui_palette_closed(void)
{
	json_object_put(palette.preview);
	g_free(palette.chat);
	g_free(palette.send_error);
	if (palette.catalog)
		json_object_put(palette.catalog);
	g_free(palette.requested_query);
	g_free(palette.error);
	g_free(palette.query);
	g_free(palette.preedit);
	memset(&palette, 0, sizeof(palette));
}

void ui_emoji_reset(void)
{
	g_clear_pointer(&drafts, g_hash_table_unref);
	ui_palette_closed();
}

gboolean ui_emoji_has_tokens(void)
{
	GArray *a = tokens(ui.selected);

	return a && a->len;
}

void ui_emoji_edit(int left, int right, int inserted)
{
	GArray *a = tokens(ui.selected);

	for (guint i = 0; a && i < a->len;) {
		struct draft_emoji *t =
			&g_array_index(a, struct draft_emoji, i);
		if (t->pos >= left && t->pos < right)
			g_array_remove_index(a, i);
		else {
			if (t->pos >= right)
				t->pos += inserted - (right - left);
			i++;
		}
	}
}

static int units(const char *a, const char *b)
{
	int n = 0;

	for (; a < b; a = g_utf8_next_char(a))
		n += g_utf8_get_char(a) > 0xffff ? 2 : 1;
	return n;
}

static gboolean advance(const char **p, int *pos, int to)
{
	if (to < *pos)
		return FALSE;
	while (**p && *pos < to) {
		*pos += g_utf8_get_char(*p) > 0xffff ? 2 : 1;
		*p = g_utf8_next_char(*p);
	}
	return *pos == to;
}

static gboolean valid_meta(Json *m)
{
	const char *p = ui_string(m, "productId", ""),
		   *e = ui_string(m, "emojiId", "");
	if (strlen(p) != 24 || strlen(e) != 3)
		return FALSE;
	for (int i = 0; i < 24; i++)
		if (!g_ascii_isxdigit(p[i]))
			return FALSE;
	return g_ascii_isdigit(e[0]) && g_ascii_isdigit(e[1]) &&
	       g_ascii_isdigit(e[2]);
}

static const char *fallback(Json *m)
{
	return ui_string(m, "label", "(emoji)");
}

static Json *wire_span(Json *meta, int start, int length)
{
	Json *r = json_object_new_object();
	const char *const keys[] = { "productId", "emojiId", "version",
				     "resourceType" };
	for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
		if (ui_field(meta, keys[i]))
			json_object_object_add(
				r, keys[i],
				json_object_get(ui_field(meta, keys[i])));
	json_object_object_add(r, "start", json_object_new_int(start));
	json_object_object_add(r, "length", json_object_new_int(length));
	return r;
}

char *ui_emoji_export(const char *chat, const char *str, int left, int right,
		      Json **spans)
{
	GArray *a = tokens(chat);

	if (!a || !a->len)
		return g_strndup(str + left, right - left);
	GString *s = g_string_sized_new(right - left + 8);
	int pos = left, utf = 0;

	for (guint i = 0; i < a->len; i++) {
		struct draft_emoji *t =
			&g_array_index(a, struct draft_emoji, i);
		if (t->pos < pos || t->pos + 3 > right ||
		    strncmp(str + t->pos, OBJECT, 3))
			continue;
		const char *label = fallback(t->meta);

		g_string_append_len(s, str + pos, t->pos - pos);
		utf += units(str + pos, str + t->pos);
		int n = units(label, label + strlen(label));

		if (spans) {
			if (!*spans)
				*spans = json_object_new_array();
			json_object_array_add(*spans,
					      wire_span(t->meta, utf, n));
		}
		g_string_append(s, label);
		utf += n;
		pos = t->pos + 3;
	}
	g_string_append_len(s, str + pos, right - pos);
	return g_string_free(s, FALSE);
}

gboolean ui_emoji_same(const char *chat, const char *str, Json *params)
{
	if (!str)
		return FALSE;
	Json *spans = NULL;

	g_autofree char *out =
		ui_emoji_export(chat, str, 0, strlen(str), &spans);
	Json *sent = ui_field(params, "emojis");
	gboolean same = !strcmp(out, ui_string(params, "text", "")) &&
			((!spans && !sent) ||
			 (spans && sent && json_object_equal(spans, sent)));
	json_object_put(spans);
	return same;
}

static void insert_pick(Json *meta)
{
	if (!ui.selected || !valid_meta(meta))
		return;
	GArray *a = tokens(ui.selected);
	int pos = MIN(ui.message.cursor, ui.message.anchor);
	int right = MAX(ui.message.cursor, ui.message.anchor);

	if (a && a->len >= MAX_SPANS) {
		gboolean replaces = FALSE;

		for (guint i = 0; i < a->len; i++) {
			int at = g_array_index(a, struct draft_emoji, i).pos;

			if (at >= pos && at < right) {
				replaces = TRUE;
				break;
			}
		}
		if (!replaces) {
			ui_status("This message has reached the emoji limit.");
			return;
		}
	}
	ui_insert(&ui.message, OBJECT);
	if (!drafts)
		drafts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					       (GDestroyNotify)g_array_unref);
	if (!a) {
		a = g_array_new(FALSE, FALSE, sizeof(struct draft_emoji));
		g_array_set_clear_func(a, drop_token);
		g_hash_table_insert(drafts, g_strdup(ui.selected), a);
	}
	guint i = 0;

	while (i < a->len && g_array_index(a, struct draft_emoji, i).pos < pos)
		i++;
	struct draft_emoji t = { pos, json_object_get(meta) };

	g_array_insert_val(a, i, t);
	ui_emoji_pick_close();
}

/*
 * The shape renderer runs only for a visible layout; measuring offscreen
 * history never fetches assets.
 */
static gpointer copy_meta(gconstpointer value)
{
	return json_object_get((Json *)value);
}

static void free_meta(gpointer value)
{
	json_object_put(value);
}

static void render_shape(cairo_t *cr, PangoAttrShape *shape, gboolean path,
			 gpointer data)
{
	(void)data;
	double x, y;

	cairo_get_current_point(cr, &x, &y);
	double size = shape->ink_rect.width / (double)PANGO_SCALE;

	y += shape->ink_rect.y / (double)PANGO_SCALE;
	if (path) {
		cairo_rectangle(cr, x, y, size, size);
		return;
	}
	cairo_save(cr);
	cairo_surface_t *img = ui_emoji_image(shape->data, TRUE, NULL);

	if (img) {
		double w = cairo_image_surface_get_width(img),
		       h = cairo_image_surface_get_height(img),
		       z = MIN(size / w, size / h);
		cairo_translate(cr, x + (size - w * z) / 2,
				y + (size - h * z) / 2);
		cairo_scale(cr, z, z);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint(cr);
	} else {
		cairo_new_path(cr);
		cairo_set_source_rgb(cr, .64, .65, .69);
		cairo_set_line_width(cr, 1.2);
		cairo_arc(cr, x + size / 2, y + size / 2, size * .36, 0,
			  2 * G_PI);
		cairo_stroke(cr);
		cairo_arc(cr, x + size / 2, y + size * .50, size * .18, .22,
			  2.92);
		cairo_stroke(cr);
		cairo_arc(cr, x + size * .40, y + size * .43, 1, 0, 2 * G_PI);
		cairo_fill(cr);
		cairo_arc(cr, x + size * .60, y + size * .43, 1, 0, 2 * G_PI);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

static void shape(PangoAttrList *attrs, Json *meta, int at, int size)
{
	PangoRectangle ink = { 0, -(size - 4) * PANGO_SCALE, size * PANGO_SCALE,
			       size * PANGO_SCALE };
	PangoRectangle logical = ink;

	logical.width += 2 * PANGO_SCALE;
	PangoAttribute *attr = pango_attr_shape_new_with_data(
		&ink, &logical, json_object_get(meta), copy_meta, free_meta);
	attr->start_index = at;
	attr->end_index = at + 3;
	pango_attr_list_insert(attrs, attr);
}

static void apply(PangoLayout *layout, PangoAttrList *attrs)
{
	PangoContext *ctx = pango_layout_get_context(layout);

	if (pango_cairo_context_get_shape_renderer(ctx, NULL) != render_shape)
		pango_cairo_context_set_shape_renderer(ctx, render_shape, NULL,
						       NULL);
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);
}

void ui_emoji_shape_history(PangoLayout *layout, const char *body,
			    const char *prefix, Json *spans)
{
	if (!ui.emojis || !json_object_is_type(spans, json_type_array) ||
	    !json_object_array_length(spans))
		return;
	const char *last = body, *scan = body;
	int unit = 0;
	GString *s = g_string_new(prefix);
	PangoAttrList *attrs = pango_attr_list_new();

	for (size_t i = 0;
	     i < MIN(json_object_array_length(spans), (size_t)MAX_SPANS); i++) {
		Json *r = json_object_array_get_idx(spans, i);
		int64_t from = json_object_get_int64(ui_field(r, "start")),
			n = json_object_get_int64(ui_field(r, "length"));
		if (!valid_meta(r) || from < unit || n <= 0 ||
		    from > G_MAXINT - n || n > G_MAXINT)
			continue;
		const char *p = scan;
		int u = unit;

		if (!advance(&p, &u, from))
			continue;
		const char *end = p;

		if (!advance(&end, &u, from + n))
			continue;
		scan = end;
		unit = u;
		gboolean failed = FALSE;

		ui_emoji_image(r, FALSE, &failed);
		if (failed)
			continue;
		g_string_append_len(s, last, p - last);
		shape(attrs, r, s->len, 24);
		g_string_append(s, OBJECT);
		last = end;
	}
	g_string_append(s, last);
	pango_layout_set_text(layout, s->str, s->len);
	g_string_free(s, TRUE);
	apply(layout, attrs);
}

void ui_emoji_shape_editor(PangoLayout *layout, int left, int right,
			   int inserted)
{
	GArray *a = tokens(ui.selected);

	if (!a || !a->len)
		return;
	PangoAttrList *attrs = pango_attr_list_new();

	for (guint i = 0; i < a->len; i++) {
		struct draft_emoji *t =
			&g_array_index(a, struct draft_emoji, i);
		int p = t->pos;

		if (p >= left && p < right)
			continue;
		if (p >= right)
			p += inserted - (right - left);
		shape(attrs, t->meta, p, 22);
	}
	apply(layout, attrs);
}

static int tile_height(void)
{
	return palette.stickers ? 76 : 54;
}

static int page_size(void)
{
	int w = MIN(450, ui.width - 24), h = MIN(406, ui.height - 24);

	palette.cols = MAX(3, (w - 32) / (palette.stickers ? 76 : 56));
	return MAX(1, (h - 160) / tile_height()) * palette.cols;
}

static void request_page(void)
{
	if (palette.loading)
		return;
	palette.loading = TRUE;
	palette.requested_offset = palette.offset;
	palette.requested_page = palette.per_page;
	g_free(palette.requested_query);
	palette.requested_query = g_strdup(palette.query);
	Json *p = json_object_new_object();

	json_object_object_add(p, "offset",
			       json_object_new_int(palette.offset));
	json_object_object_add(p, "limit",
			       json_object_new_int(palette.per_page + 1));
	if (palette.query && *palette.query)
		json_object_object_add(p, "query",
				       json_object_new_string(palette.query));
	palette.catalog_tag = ++next_request;
	ui_rpc(palette.stickers ? "stickerCatalog" : "emojiCatalog", p,
	       palette.catalog_tag);
}

static void change_page(int offset, int select)
{
	palette.offset = offset;
	palette.selected = select;
	if (palette.catalog)
		json_object_put(palette.catalog);
	palette.catalog = NULL;
	g_clear_pointer(&palette.error, g_free);
	request_page();
	ui.dirty = TRUE;
}

void ui_emoji_catalog(guint tag, Json *result, const char *error)
{
	if (!palette.loading || tag != palette.catalog_tag)
		return;
	palette.loading = FALSE;
	if (palette.requested_offset != palette.offset ||
	    palette.requested_page != palette.per_page ||
	    g_strcmp0(palette.requested_query, palette.query)) {
		request_page();
		return;
	}
	g_free(palette.error);
	palette.error = g_strdup(error);
	if (palette.catalog)
		json_object_put(palette.catalog);
	palette.catalog = json_object_is_type(result, json_type_array) ?
				  json_object_get(result) :
				  NULL;
	if (!palette.catalog && !palette.error)
		palette.error = g_strdup(
			palette.stickers ? "Could not load your stickers." :
					   "Could not load your LINE emoji.");
	int n = palette.catalog ?
			(int)json_object_array_length(palette.catalog) :
			0;
	palette.selected = CLAMP(palette.selected, 0,
				 MAX(0, MIN(n, palette.per_page) - 1));
	ui.dirty = TRUE;
}

static void open_tab(gboolean stickers)
{
	palette.stickers = stickers;
	palette.loading = FALSE;
	g_clear_pointer(&palette.query, g_free);
	g_clear_pointer(&palette.preedit, g_free);
	palette.per_page = page_size();
	change_page(0, 0);
	SDL_StartTextInput();
}

void ui_emoji_open(void)
{
	ui_palette_closed();
	palette.chat = g_strdup(ui.selected);
	open_tab(!ui.emojis && ui.stickers);
}

static gboolean inside(Rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static int count_items(void)
{
	return palette.catalog ?
		       (int)json_object_array_length(palette.catalog) :
		       0;
}

static Json *item(guint i)
{
	return i < (guint)count_items() ?
		       json_object_array_get_idx(palette.catalog, i) :
		       NULL;
}

static void move(int delta)
{
	int count = count_items(), pos = palette.selected + delta;

	if (pos < 0 && palette.offset > 0) {
		change_page(MAX(0, palette.offset - palette.per_page),
			    delta <= -palette.per_page ? 0 :
							 palette.per_page - 1);
		return;
	}
	if (pos >= palette.per_page && count > palette.per_page) {
		change_page(palette.offset + palette.per_page, 0);
		return;
	}
	pos = CLAMP(pos, 0, MAX(0, MIN(count, palette.per_page) - 1));
	if (pos != palette.selected) {
		palette.selected = pos;
		ui.dirty = TRUE;
	}
}

static void choose_item(Json *row)
{
	if (!palette.stickers) {
		insert_pick(row);
		return;
	}
	if (!row || !*ui_string(row, "stickerId", ""))
		return;
	palette.preview = json_object_get(row);
	ui_apng_replay(ui_emoji_image(row, FALSE, NULL));
	SDL_StopTextInput();
	ui.dirty = TRUE;
}

static void leave_preview(void)
{
	g_clear_pointer(&palette.preview, json_object_put);
	g_clear_pointer(&palette.send_error, g_free);
	SDL_StartTextInput();
	ui.dirty = TRUE;
}

static void send_sticker(void)
{
	Json *row = palette.preview;
	const char *const keys[] = { "packageId", "stickerId", "version",
				     "resourceType" };

	if (palette.send_tag || !row || !palette.chat ||
	    g_strcmp0(ui.selected, palette.chat) ||
	    !ui_emoji_image(row, FALSE, NULL))
		return;
	Json *p = json_object_new_object();

	json_object_object_add(p, "chat", json_object_new_string(palette.chat));
	for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
		Json *v = ui_field(row, keys[i]);

		if (v)
			json_object_object_add(p, keys[i], json_object_get(v));
	}
	if (ui.reply_id && !g_strcmp0(ui.reply_chat, palette.chat))
		json_object_object_add(p, "replyTo",
				       json_object_new_string(ui.reply_id));
	g_clear_pointer(&palette.send_error, g_free);
	palette.send_tag = ++next_request;
	ui_rpc("sendSticker", p, palette.send_tag);
	ui.dirty = TRUE;
}

void ui_sticker_sent(guint tag, const char *error)
{
	if (!tag || tag != palette.send_tag) {
		if (error)
			ui_status(error);
		return;
	}
	palette.send_tag = 0;
	if (!error) {
		ui_emoji_pick_close();
		return;
	}
	g_free(palette.send_error);
	palette.send_error = g_strdup(error);
	ui.dirty = TRUE;
}

static void draw_preview(Rect r)
{
	gboolean failed = FALSE;
	Json *row = palette.preview;
	cairo_surface_t *image = ui_emoji_image(row, TRUE, &failed);
	g_autofree char *recipient = g_strdup_printf(
		"Send to %s",
		ui_string(ui_selected_chat(), "name", "this conversation"));
	int side = MIN(196, r.h - 190);
	Rect image_area = { r.x + (r.w - side) / 2, r.y + 66, side, side };
	const char *label = ui_string(row, "label", "Sticker");

	ui_draw_label(recipient, (Rect){ r.x + 18, r.y + 15, r.w - 36, 36 }, 21,
		      600, INK, FALSE);
	if (image)
		ui_draw_image(image, image_area);
	else
		ui_draw_label(failed ? "Sticker preview unavailable." :
				       "Loading sticker…",
			      (Rect){ r.x + 24, image_area.y + side / 2,
				      r.w - 48, 35 },
			      15, 450, MUTED, FALSE);
	ui_draw_label(label,
		      (Rect){ r.x + 18, image_area.y + side + 10, r.w - 36,
			      27 },
		      16, 600, INK, FALSE);
	if (palette.send_error)
		ui_draw_label(palette.send_error,
			      (Rect){ r.x + 18, image_area.y + side + 40,
				      r.w - 36, r.h - side - 160 },
			      14, 400, MUTED, TRUE);
	palette.back = (Rect){ r.x + 16, r.y + r.h - 49, 66, 34 };
	palette.send = (Rect){ r.x + r.w - 122, r.y + r.h - 49, 106, 34 };
	if (!palette.send_tag)
		ui_draw_button(palette.back, "Back", FALSE);
	ui_draw_button(palette.send,
		       palette.send_tag ? "Sending…" : "Send sticker",
		       image && !palette.send_tag);
}

void ui_emoji_draw(void)
{
	ui_scrim();
	int w = MIN(450, ui.width - 24), h = MIN(406, ui.height - 24),
	    x = (ui.width - w) / 2, y = (ui.height - h) / 2;
	palette.panel = (Rect){ x, y, w, h };
	ui_draw_box(palette.panel, 0x242429);
	if (palette.preview) {
		draw_preview(palette.panel);
		return;
	}
	if (ui.stickers && ui.emojis) {
		palette.emoji_tab = (Rect){ x + 16, y + 12, 77, 31 };
		palette.sticker_tab = (Rect){ x + 98, y + 12, 82, 31 };
		ui_draw_button(palette.emoji_tab, "Emoji", !palette.stickers);
		ui_draw_button(palette.sticker_tab, "Stickers",
			       palette.stickers);
	} else
		ui_draw_label(palette.stickers ? "Stickers" : "LINE emoji",
			      (Rect){ x + 18, y + 16, w - 87, 32 }, 22, 650,
			      INK, FALSE);
	palette.close = (Rect){ x + w - 69, y + 12, 54, 31 };
	ui_draw_button(palette.close, "Close", FALSE);
	palette.search = (Rect){ x + 16, y + 55, w - 32, 35 };
	ui_draw_box(palette.search, 0x383a43);
	g_autofree char *query =
		g_strconcat(palette.query ? palette.query : "",
			    palette.preedit ? palette.preedit : "", NULL);
	ui_draw_label(*query	       ? query :
		      palette.stickers ? "Search stickers or packs…" :
					 "Search emoji or packs…",
		      (Rect){ x + 26, y + 62, w - 53, 26 }, 14, 450,
		      *query ? INK : MUTED, FALSE);
	int n = page_size(), dy = tile_height(), cell = (w - 32) / palette.cols;

	palette.grid = (Rect){ x + 16, y + 101, cell * palette.cols,
			       (n / palette.cols) * dy };
	if (palette.per_page != n) {
		palette.per_page = n;
		change_page(0, 0);
	}
	int start = 0, count = count_items();

	for (int i = 0; i < palette.per_page && start + i < count; i++) {
		Json *r = item(start + i);
		Rect tile = { palette.grid.x + (i % palette.cols) * cell,
			      palette.grid.y + (i / palette.cols) * dy,
			      cell - 3, dy - 3 };
		if (start + i == palette.selected)
			ui_draw_box(tile, 0x414450);
		cairo_surface_t *image = ui_emoji_image(r, TRUE, NULL);

		int edge = palette.stickers ? MIN(64, tile.w - 5) : 36;

		if (image)
			ui_draw_image(image,
				      (Rect){ tile.x + (tile.w - edge) / 2,
					      tile.y + (tile.h - edge) / 2,
					      edge, edge });
		else
			ui_draw_label(ui_string(r,
						palette.stickers ? "stickerId" :
								   "emojiId",
						""),
				      (Rect){ tile.x + 7, tile.y + 16,
					      tile.w - 9, 19 },
				      13, 550, MUTED, FALSE);
	}
	if (!count)
		ui_draw_label(
			palette.loading ?
				(palette.stickers ? "Loading stickers…" :
						    "Loading your emoji…") :
			palette.error ?
				palette.error :
			palette.query && *palette.query ?
				(palette.stickers ? "No matching stickers." :
						    "No matching emoji.") :
				(palette.stickers ? "No stickers available." :
						    "No emoji available."),
			(Rect){ x + 20, y + 119, w - 40, 80 }, 14, 450, MUTED,
			TRUE);
	palette.prev = (Rect){ x + w - 91, y + h - 46, 32, 30 };
	palette.next = (Rect){ x + w - 53, y + h - 46, 32, 30 };
	if (palette.offset > 0)
		ui_draw_button(palette.prev, "‹", FALSE);
	if (count > palette.per_page)
		ui_draw_button(palette.next, "›", FALSE);
	Json *cur = item(palette.selected);

	if (cur) {
		const char *pack = ui_string(cur, "pack", "");
		g_autofree char *name =
			palette.stickers ?
				g_strdup(ui_string(cur, "label", "Sticker")) :
				g_strdup_printf("%s%s%s", pack,
						*pack ? " · " : "",
						fallback(cur));
		ui_draw_label(name,
			      (Rect){ x + 17, y + h - 41,
				      w - (count > palette.per_page ||
							   palette.offset ?
						   122 :
						   35),
				      30 },
			      13, 550, INK, FALSE);
	}
}

static gboolean preview_input(SDL_Event *e)
{
	if (palette.send_tag)
		return TRUE;
	if (e->type == SDL_MOUSEBUTTONDOWN &&
	    e->button.button == SDL_BUTTON_LEFT) {
		int x = e->button.x, y = e->button.y;

		if (inside(palette.back, x, y))
			leave_preview();
		else if (inside(palette.send, x, y))
			send_sticker();
	} else if (e->type == SDL_KEYDOWN && !e->key.repeat) {
		if (e->key.keysym.sym == SDLK_ESCAPE)
			leave_preview();
		if (e->key.keysym.sym == SDLK_RETURN ||
		    e->key.keysym.sym == SDLK_KP_ENTER)
			send_sticker();
	}
	return TRUE;
}

gboolean ui_emoji_input(SDL_Event *e)
{
	int count = count_items();

	if (palette.preview &&
	    (e->type == SDL_KEYDOWN || e->type == SDL_MOUSEBUTTONDOWN ||
	     e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEWHEEL ||
	     e->type == SDL_TEXTINPUT || e->type == SDL_TEXTEDITING))
		return preview_input(e);
	switch (e->type) {
	case SDL_MOUSEBUTTONDOWN: {
		int x = e->button.x, y = e->button.y;

		if (e->button.button != SDL_BUTTON_LEFT)
			return TRUE;
		if (!inside(palette.panel, x, y) || inside(palette.close, x, y))
			ui_emoji_pick_close();
		else if (ui.emojis && ui.stickers &&
			 inside(palette.emoji_tab, x, y) && palette.stickers)
			open_tab(FALSE);
		else if (ui.emojis && ui.stickers &&
			 inside(palette.sticker_tab, x, y) && !palette.stickers)
			open_tab(TRUE);
		else if (palette.offset && inside(palette.prev, x, y))
			move(-palette.per_page);
		else if (count > palette.per_page && inside(palette.next, x, y))
			move(palette.per_page);
		else if (inside(palette.grid, x, y)) {
			int i = (y - palette.grid.y) / tile_height() *
					palette.cols +
				(x - palette.grid.x) /
					(palette.grid.w / palette.cols);
			if (i < count)
				choose_item(item(i));
		}
		return TRUE;
	}
	case SDL_MOUSEMOTION:
		if (inside(palette.grid, e->motion.x, e->motion.y)) {
			int i = (e->motion.y - palette.grid.y) / tile_height() *
					palette.cols +
				(e->motion.x - palette.grid.x) /
					(palette.grid.w / palette.cols);
			if (i < count && i != palette.selected) {
				palette.selected = i;
				ui.dirty = TRUE;
			}
		}
		return TRUE;
	case SDL_MOUSEWHEEL:
		if (e->wheel.y)
			move(e->wheel.y > 0 ? -palette.per_page :
					      palette.per_page);
		return TRUE;
	case SDL_TEXTINPUT:
		if (g_utf8_validate(e->text.text, -1, NULL) &&
		    (!palette.query || strlen(palette.query) < 192)) {
			char *q =
				g_strconcat(palette.query ? palette.query : "",
					    e->text.text, NULL);
			g_free(palette.query);
			palette.query = q;
			g_clear_pointer(&palette.preedit, g_free);
			change_page(0, 0);
		}
		return TRUE;
	case SDL_TEXTEDITING:
		g_free(palette.preedit);
		palette.preedit = g_strdup(e->edit.text);
		ui.dirty = TRUE;
		return TRUE;
	case SDL_KEYDOWN:
		switch (e->key.keysym.sym) {
		case SDLK_TAB:
			if (ui.emojis && ui.stickers && !e->key.repeat)
				open_tab(!palette.stickers);
			break;
		case SDLK_ESCAPE:
			ui_emoji_pick_close();
			break;
		case SDLK_LEFT:
			move(-1);
			break;
		case SDLK_RIGHT:
			move(1);
			break;
		case SDLK_UP:
			move(-palette.cols);
			break;
		case SDLK_DOWN:
			move(palette.cols);
			break;
		case SDLK_PAGEUP:
			move(-palette.per_page);
			break;
		case SDLK_PAGEDOWN:
			move(palette.per_page);
			break;
		case SDLK_HOME:
			move(-count);
			break;
		case SDLK_END:
			move(count);
			break;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			if (count && !e->key.repeat &&
			    !(palette.preedit && *palette.preedit))
				choose_item(item(palette.selected));
			break;
		case SDLK_BACKSPACE:
			if (palette.query && *palette.query) {
				*g_utf8_prev_char(palette.query +
						  strlen(palette.query)) = 0;
				change_page(0, 0);
			}
			break;
		default:
			break;
		}
		return TRUE;
	default:
		return FALSE;
	}
}
