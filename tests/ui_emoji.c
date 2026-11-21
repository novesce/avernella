// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_emoji.h"
#include <string.h>

/* Only the picker, drafts and Pango layout run here. No worker, device or display. */
Interface ui;
static gboolean failed;
static guint fetches, catalog_tag, send_tag, sends;
static const char *catalog_method;
static Json *sent;
static cairo_surface_t *sticker_fixture;
static Rect send_box;

Json *ui_field(Json *obj, const char *key)
{
	Json *value = NULL;

	json_object_object_get_ex(obj, key, &value);
	return value;
}

const char *ui_string(Json *obj, const char *key, const char *fallback)
{
	Json *v = ui_field(obj, key);

	return json_object_is_type(v, json_type_string) ?
		       json_object_get_string(v) :
		       fallback;
}

Json *ui_selected_chat(void)
{
	return ui.opened;
}

void ui_apng_replay(cairo_surface_t *poster)
{
	(void)poster;
}

void ui_rpc(const char *name, Json *p, guint tag)
{
	if (!strcmp(name, "sendSticker")) {
		sends++;
		send_tag = tag;
		g_clear_pointer(&sent, json_object_put);
		sent = json_object_get(p);
	} else {
		g_assert_true(!strcmp(name, "emojiCatalog") ||
			      !strcmp(name, "stickerCatalog"));
		catalog_method = !strcmp(name, "stickerCatalog") ? "stickers" :
								   "emojis";
		catalog_tag = tag;
		g_assert_cmpint(json_object_get_int(ui_field(p, "limit")), <=,
				29);
	}
	json_object_put(p);
}

void ui_insert(Editor *e, const char *text)
{
	int l = MIN(e->cursor, e->anchor), r = MAX(e->cursor, e->anchor),
	    n = strlen(text);
	ui_emoji_edit(l, r, n);
	g_string_erase(e->value, l, r - l);
	g_string_insert(e->value, l, text);
	e->cursor = e->anchor = l + n;
}

void ui_status(const char *s)
{
	(void)s;
}

void ui_emoji_pick_close(void)
{
	ui_palette_closed();
}

cairo_surface_t *ui_emoji_image(Json *m, gboolean load, gboolean *missing)
{
	(void)m;
	if (load)
		fetches++;
	if (missing)
		*missing = failed;
	return ui_field(m, "stickerId") ? sticker_fixture : NULL;
}

void ui_scrim(void)
{
}

void ui_draw_box(Rect r, guint rgb)
{
	(void)r;
	(void)rgb;
}

void ui_draw_label(const char *s, Rect r, int sz, int w, guint rgb,
		   gboolean wrap)
{
	(void)s;
	(void)r;
	(void)sz;
	(void)w;
	(void)rgb;
	(void)wrap;
}

void ui_draw_button(Rect r, const char *s, gboolean p)
{
	if (!strcmp(s, "Send sticker"))
		send_box = r;
	(void)p;
}

void ui_draw_image(cairo_surface_t *img, Rect r)
{
	(void)img;
	(void)r;
}

static void picker_edit(void)
{
	ui.width = 920;
	ui.height = 630;
	ui.selected = (char *)"a";
	ui.message.value = g_string_new("");
	ui_insert(&ui.message, "🚲 ไปกัน ");
	Json *page = json_tokener_parse(
		"[{\"productId\":\"670e0cce840a8236ddd4ee4c\",\"emojiId\":\"002\","
		"\"label\":\"(smile)\",\"version\":3}]");
	ui_emoji_open();
	ui_emoji_catalog(catalog_tag, page, NULL);
	json_object_put(page);
	SDL_Event enter = { .key = { .type = SDL_KEYDOWN,
				     .repeat = 0,
				     .keysym = { .sym = SDLK_RETURN } } };
	g_assert_true(ui_emoji_input(&enter));
	ui_insert(&ui.message, " Friday?");
	Json *spans = NULL;

	g_autofree char *out = ui_emoji_export(ui.selected,
					       ui.message.value->str, 0,
					       ui.message.value->len, &spans);
	g_assert_cmpstr(out, ==, "🚲 ไปกัน (smile) Friday?");
	Json *r = json_object_array_get_idx(spans, 0);

	g_assert_cmpint(json_object_get_int(ui_field(r, "start")), ==, 9);
	g_assert_cmpint(json_object_get_int(ui_field(r, "length")), ==, 7);
	Json *params = json_object_new_object();

	json_object_object_add(params, "text", json_object_new_string(out));
	json_object_object_add(params, "emojis", spans);
	g_assert_true(
		ui_emoji_same(ui.selected, ui.message.value->str, params));
	g_assert_false(ui_emoji_same("b", ui.message.value->str, params));
	ui.message.cursor = ui.message.anchor = 0;
	ui_insert(&ui.message, "!");
	g_assert_false(
		ui_emoji_same(ui.selected, ui.message.value->str, params));
	int at = strstr(ui.message.value->str, "\xef\xbf\xbc") -
		 ui.message.value->str;
	g_autofree char *copy = ui_emoji_export(
		ui.selected, ui.message.value->str, at, at + 3, NULL);
	g_assert_cmpstr(copy, ==, "(smile)");
	ui.message.cursor = at;
	ui.message.anchor = at + 3;
	ui_insert(&ui.message, "");
	g_assert_false(ui_emoji_has_tokens());
	json_object_put(params);
	ui_emoji_reset();
	g_string_free(ui.message.value, TRUE);
}

static void shaped_history(void)
{
	const char *text = "😀 สวัสดี (happy) ดีใจ";
	const char *prefix = "↪ Reply\n";
	const char *joined = "↪ Reply\n😀 สวัสดี (happy) ดีใจ";
	Json *spans = json_tokener_parse(
		"[{\"productId\":\"670e0cce840a8236ddd4ee4c\",\"emojiId\":\"001\",\"start\":10,\"length\":7}]");
	cairo_surface_t *s =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 260, 120);
	cairo_t *cr = cairo_create(s);
	PangoLayout *layout = pango_cairo_create_layout(cr);

	ui.emojis = TRUE;
	failed = FALSE;
	pango_layout_set_text(layout, joined, -1);
	pango_layout_set_width(layout, 65 * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	ui_emoji_shape_history(layout, text, prefix, spans);
	g_assert_cmpstr(pango_layout_get_text(layout), ==,
			"↪ Reply\n😀 สวัสดี \xef\xbf\xbc ดีใจ");
	g_assert_cmpint(pango_layout_get_line_count(layout), >, 1);
	g_assert_cmpuint(fetches, ==, 0);
	pango_cairo_show_layout(cr, layout);
	g_assert_cmpuint(fetches, ==, 1);
	failed = TRUE;
	ui_emoji_shape_history(layout, text, prefix, spans);
	g_assert_cmpstr(pango_layout_get_text(layout), ==, joined);
	failed = FALSE;
	json_object_object_add(json_object_array_get_idx(spans, 0), "start",
			       json_object_new_int(1));
	ui_emoji_shape_history(layout, text, prefix, spans);
	g_assert_cmpstr(pango_layout_get_text(layout), ==, joined);
	json_object_put(spans);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

static void key(SDL_Keycode k, gboolean repeat)
{
	SDL_Event e = { .key = { .type = SDL_KEYDOWN,
				 .repeat = repeat,
				 .keysym = { .sym = k } } };

	g_assert_true(ui_emoji_input(&e));
}

static void click(Rect r)
{
	SDL_Event e = { .button = { .type = SDL_MOUSEBUTTONDOWN,
				    .button = SDL_BUTTON_LEFT,
				    .x = r.x + r.w / 2,
				    .y = r.y + r.h / 2 } };

	g_assert_true(ui_emoji_input(&e));
}

static void sticker_preview(void)
{
	ui.width = 360;
	ui.height = 480;
	ui.emojis = TRUE;
	ui.stickers = TRUE;
	ui.selected = "group";
	ui.message.value = g_string_new("Keep this draft");
	ui.reply_id = "42";
	ui.reply_chat = "group";
	Json *stickers = json_tokener_parse(
		"[{\"packageId\":\"2\",\"stickerId\":\"27\",\"version\":3,"
		"\"resourceType\":\"ANIMATION\",\"pack\":\"Fixture pack\",\"label\":\"Wave\"}]");

	ui_emoji_open();
	g_assert_cmpstr(catalog_method, ==, "emojis");
	guint stale = catalog_tag;

	key(SDLK_TAB, FALSE);
	g_assert_cmpstr(catalog_method, ==, "stickers");
	ui_emoji_catalog(stale, NULL, "Late response from emoji tab");
	ui_emoji_catalog(catalog_tag, stickers, NULL);
	ui_emoji_draw();
	key(SDLK_RETURN, FALSE);
	ui_emoji_draw();
	g_assert_cmpint(send_box.w, >, 0);
	click(send_box);
	g_assert_cmpuint(sends, ==, 0);
	key(SDLK_ESCAPE, FALSE);
	key(SDLK_RETURN, FALSE);
	sticker_fixture =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
	ui_emoji_draw();
	key(SDLK_RETURN, TRUE);
	g_assert_cmpuint(sends, ==, 0);
	ui.selected = "different chat";
	click(send_box);
	g_assert_cmpuint(sends, ==, 0);
	ui.selected = "group";
	click(send_box);
	click(send_box);
	g_assert_cmpuint(sends, ==, 1);
	g_assert_cmpstr(ui_string(sent, "chat", ""), ==, "group");
	g_assert_cmpstr(ui_string(sent, "stickerId", ""), ==, "27");
	g_assert_cmpstr(ui_string(sent, "packageId", ""), ==, "2");
	g_assert_cmpstr(ui_string(sent, "replyTo", ""), ==, "42");
	g_assert_cmpint(json_object_get_int(ui_field(sent, "version")), ==, 3);
	g_assert_cmpstr(ui_string(sent, "resourceType", ""), ==, "ANIMATION");
	ui_sticker_sent(send_tag, "Fixture refusal");
	g_assert_cmpuint(sends, ==, 1);
	key(SDLK_RETURN, FALSE);
	g_assert_cmpuint(sends, ==, 2);
	ui_sticker_sent(send_tag, NULL);
	g_assert_cmpstr(ui.message.value->str, ==, "Keep this draft");
	ui.emojis = FALSE;
	ui_emoji_open();
	g_assert_cmpstr(catalog_method, ==, "stickers");
	ui_palette_closed();
	ui_emoji_catalog(catalog_tag, stickers, NULL);
	ui_emoji_reset();
	g_clear_pointer(&sent, json_object_put);
	g_clear_pointer(&sticker_fixture, cairo_surface_destroy);
	json_object_put(stickers);
	g_string_free(ui.message.value, TRUE);
	ui.reply_id = ui.reply_chat = NULL;
	ui.stickers = FALSE;
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ui-emoji/picker-draft-and-send", picker_edit);
	g_test_add_func("/ui-emoji/shape-wrap-and-fallback", shaped_history);
	g_test_add_func("/ui-stickers/preview-confirmation-and-stale-reply",
			sticker_preview);
	return g_test_run();
}
