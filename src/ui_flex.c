// SPDX-License-Identifier: GPL-3.0-or-later
#include "liff_url.h"
#include "ui_flex.h"
#include <math.h>
#include <string.h>

enum {
	MAX_NODES = 200,
	MAX_CHILDREN = 48,
	MAX_DEPTH = 12,
	MAX_TEXT = 4096,
	MAX_CARD_HEIGHT = 1050,
	CARD_GAP = 8
};

struct flex_item {
	Rect rect, clip;
	guint32 fill, ink, border;
	int radius, stroke, label_x, label_y;
	PangoLayout *label;
	char *url, *uri;
	gboolean cover, card, action;
};

struct ui_flex_layout {
	GArray *items;
	int width, height;
};

struct builder {
	struct ui_flex_layout *layout;
	PangoContext *context;
	guint nodes, visible;
	gboolean rtl;
};

static const char *string(Json *object, const char *key)
{
	Json *value;

	if (!json_object_is_type(object, json_type_object) ||
	    !json_object_object_get_ex(object, key, &value) ||
	    !json_object_is_type(value, json_type_string))
		return NULL;
	const char *s = json_object_get_string(value);
	size_t len = json_object_get_string_len(value);

	return len <= MAX_TEXT && strlen(s) == len &&
			       g_utf8_validate(s, len, NULL) ?
		       s :
		       NULL;
}

static Json *field(Json *object, const char *key)
{
	Json *value = NULL;

	if (json_object_is_type(object, json_type_object))
		json_object_object_get_ex(object, key, &value);
	return value;
}

static gboolean is(const char *value, const char *expected)
{
	return g_strcmp0(value, expected) == 0;
}

static gboolean truth(Json *object, const char *key)
{
	Json *value = field(object, key);

	return json_object_is_type(value, json_type_boolean) &&
	       json_object_get_boolean(value);
}

static guint32 color(const char *s, guint32 fallback)
{
	if (!s || *s != '#')
		return fallback;
	size_t n = strlen(++s);

	if ((n != 6 && n != 8) || strspn(s, "0123456789abcdefABCDEF") != n)
		return fallback;
	guint32 v = g_ascii_strtoull(s, NULL, 16);

	return n == 6 ? (v << 8) | 255 : v;
}

static int dimension(const char *s, int bound, int fallback)
{
	if (!s || !g_ascii_isdigit(*s))
		return fallback;
	char *end;
	double num = g_ascii_strtod(s, &end);

	if (!isfinite(num) || num < 0 || num > 100000)
		return fallback;
	if (is(end, "%"))
		num = num * bound / 100;
	else if (!is(end, "px"))
		return fallback;
	return MIN(bound, (int)(num + .5));
}

static int size(const char *s, int bound, int fallback, gboolean text)
{
	static const char *const names[] = { "none", "xxs", "xs", "sm",
					     "md",   "lg",  "xl", "xxl",
					     "3xl",  "4xl", "5xl" };
	static const int offsets[] = { 0, 2, 4, 8, 12, 16, 20, 24, 30, 36, 42 };
	static const int fonts[] = { 0, 10, 11, 13, 15, 17, 20, 24, 28, 32, 38 };

	for (guint i = 0; i < G_N_ELEMENTS(names); i++)
		if (is(s, names[i]))
			return MIN(bound, text ? fonts[i] : offsets[i]);
	return dimension(s, bound, fallback);
}

static int dimension_for(Json *node, const char *name, int max, int def)
{
	return dimension(string(node, name), max, def);
}

static int space(Json *node, const char *name, int def)
{
	return size(string(node, name), 72, def, FALSE);
}

static Rect intersection(Rect a, Rect b)
{
	int x = MAX(a.x, b.x), y = MAX(a.y, b.y);

	return (Rect){ x, y, MAX(0, MIN(a.x + a.w, b.x + b.w) - x),
		       MAX(0, MIN(a.y + a.h, b.y + b.h) - y) };
}

static char *https_url(const char *url)
{
	if (!url || !g_str_has_prefix(url, "https://"))
		return NULL;
	for (const unsigned char *p = (const unsigned char *)url; *p; p++)
		if (*p <= 0x20 || *p == 0x7f || *p == '\\')
			return NULL;
	g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);

	return uri && g_uri_get_host(uri) && *g_uri_get_host(uri) &&
			       !g_uri_get_userinfo(uri) ?
		       g_strdup(url) :
		       NULL;
}

/* Card actions may also open LIFF apps by line://app/ links. */
static char *link_url(const char *url)
{
	return line_liff_url(url, NULL, NULL) ? g_strdup(url) : https_url(url);
}

static char *action_uri(Json *node)
{
	Json *action = field(node, "action");

	return is(string(action, "type"), "uri") ?
		       link_url(string(action, "uri")) :
		       NULL;
}

static void item_clear(gpointer value)
{
	struct flex_item *item = value;

	g_clear_object(&item->label);
	g_free(item->url);
	g_free(item->uri);
}

static guint item_add(struct builder *b, struct flex_item item)
{
	guint index = b->layout->items->len;

	g_array_append_val(b->layout->items, item);
	return index;
}

static PangoAlignment alignment(const struct builder *b, const char *align)
{
	if (is(align, "center"))
		return PANGO_ALIGN_CENTER;
	if (is(align, "end"))
		return b->rtl ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT;
	return b->rtl ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
}

static void attribute(PangoAttrList *attrs, PangoAttribute *a, guint from,
		      guint to)
{
	a->start_index = from;
	a->end_index = to;
	pango_attr_list_insert(attrs, a);
}

static void span_style(Json *span, PangoAttrList *attrs, guint from, guint to,
		       gboolean override)
{
	const char *value = string(span, "size");

	if (override && value)
		attribute(attrs,
			  pango_attr_size_new_absolute(
				  MAX(9, size(value, 44, 15, TRUE)) *
				  PANGO_SCALE),
			  from, to);
	if (is(string(span, "weight"), "bold"))
		attribute(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD), from,
			  to);
	if (is(string(span, "style"), "italic"))
		attribute(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC), from,
			  to);
	value = string(span, "decoration");
	if (is(value, "underline"))
		attribute(attrs,
			  pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
			  from, to);
	else if (is(value, "line-through"))
		attribute(attrs, pango_attr_strikethrough_new(TRUE), from, to);
	value = string(span, "color");
	if (override && value) {
		guint32 rgba = color(value, 0x222329ff);

		attribute(attrs,
			  pango_attr_foreground_new((rgba >> 24) * 257,
						    ((rgba >> 16) & 255) * 257,
						    ((rgba >> 8) & 255) * 257),
			  from, to);
	}
}

static PangoLayout *label(struct builder *b, Json *node, const char *value,
			  int width, gboolean button)
{
	PangoLayout *layout = pango_layout_new(b->context);
	PangoFontDescription *font = pango_font_description_new();
	int px = MAX(9, size(string(node, "size"), 44, 15, TRUE));

	pango_font_description_set_family(
		font, "Inter, Noto Sans, Noto Sans Thai, sans-serif");
	pango_font_description_set_absolute_size(font, px * PANGO_SCALE);
	pango_font_description_set_weight(font, button ? PANGO_WEIGHT_SEMIBOLD :
							 PANGO_WEIGHT_NORMAL);
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	g_autoptr(PangoAttrList) attrs = pango_attr_list_new();
	g_autoptr(GString) combined = NULL;

	span_style(node, attrs, 0, G_MAXUINT, FALSE);
	if (!value && !button) {
		Json *contents = field(node, "contents");
		size_t count = json_object_is_type(contents, json_type_array) ?
				       json_object_array_length(contents) :
				       0;

		combined = g_string_new("");
		for (size_t i = 0; i < count && i < MAX_CHILDREN; i++) {
			Json *span = json_object_array_get_idx(contents, i);
			const char *text = string(span, "text");

			if (!is(string(span, "type"), "span") || !text ||
			    strlen(text) > MAX_TEXT - combined->len)
				continue;
			guint from = combined->len;

			g_string_append(combined, text);
			span_style(span, attrs, from, combined->len, TRUE);
		}
		value = combined->str;
	}
	if (!value || !*value) {
		g_object_unref(layout);
		return NULL;
	}
	pango_layout_set_attributes(layout, attrs);
	pango_layout_set_text(layout, value, -1);
	if (width > 0)
		pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_alignment(
		layout, button ? PANGO_ALIGN_CENTER :
				 alignment(b, string(node, "align")));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	Json *lines = field(node, "maxLines");
	int max_lines =
		json_object_is_type(lines, json_type_int) ?
			(int)CLAMP(json_object_get_int64(lines), 1, 20) :
			20;

	pango_layout_set_height(
		layout, button || !truth(node, "wrap") ? -1 : -max_lines);
	pango_layout_set_spacing(layout,
				 space(node, "lineSpacing", 0) * PANGO_SCALE);
	return layout;
}

static int text_width(struct builder *b, Json *node, gboolean button)
{
	const char *s = button ? string(field(node, "action"), "label") :
				 string(node, "text");
	PangoLayout *layout = label(b, node, s, 0, button);
	int width = 0;

	if (layout) {
		pango_layout_get_pixel_size(layout, &width, NULL);
		g_object_unref(layout);
	}
	return width + (button ? 16 : 0);
}

static double aspect(Json *node)
{
	const char *s = string(node, "aspectRatio");

	if (s && g_ascii_isdigit(*s)) {
		char *next;
		double w = g_ascii_strtod(s, &next);

		if (*next == ':' && g_ascii_isdigit(next[1])) {
			double h = g_ascii_strtod(next + 1, &next);

			if (!*next && isfinite(w) && isfinite(h) && w > 0 &&
			    h > 0)
				return CLAMP(w / h, .2, 5.);
		}
	}
	return 1;
}

static int image_width(Json *node, int width, gboolean icon)
{
	const char *s = string(node, "size");
	static const char *const keywords[] = { "xxs", "xs", "sm",  "md",
						"lg",  "xl", "xxl", "3xl",
						"4xl", "5xl" };

	if (icon)
		return MAX(1, size(s, width, MIN(width, 16), TRUE));
	if (is(s, "full"))
		return width;
	for (guint i = 0; i < G_N_ELEMENTS(keywords); i++)
		if (is(s, keywords[i]))
			return MAX(1, width * (20 + 8 * (int)i) / 100);
	return MAX(1, dimension(s, width, width * 44 / 100));
}

static int component(struct builder *b, Json *node, Rect slot, Rect clip,
		     guint depth, gboolean section);

static int fixed_width(struct builder *b, Json *node, int width)
{
	const char *kind = string(node, "type");

	if (is(kind, "separator"))
		return 1;
	int specified = dimension_for(node, "width", width, -1);

	if (specified >= 0)
		return specified;
	if (is(kind, "icon") || is(kind, "image"))
		return image_width(node, width, is(kind, "icon"));
	if (is(kind, "text") || is(kind, "button"))
		return MIN(width, text_width(b, node, is(kind, "button")));
	return 0;
}

static int child_flex(Json *node, gboolean baseline)
{
	const char *type = string(node, "type");

	if (is(type, "separator") || string(node, "width"))
		return 0;
	Json *flex = field(node, "flex");

	return json_object_is_type(flex, json_type_int) ?
		       (int)CLAMP(json_object_get_int64(flex), 0, 100) :
		       !(is(type, "icon") || baseline);
}

static void move_items(struct builder *b, guint first, guint end, int dy,
		       Rect clip)
{
	for (guint i = first; i < end && dy; i++) {
		struct flex_item *item =
			&g_array_index(b->layout->items, struct flex_item, i);
		item->rect.y += dy;
		item->clip.y += dy;
		item->clip = intersection(item->clip, clip);
	}
}

static int box_contents(struct builder *b, Json *node, Rect slot, Rect clip,
			guint depth)
{
	Json *contents = field(node, "contents");
	const char *flow = string(node, "layout");
	gboolean baseline = is(flow, "baseline");
	gboolean horizontal = baseline || is(flow, "horizontal");
	size_t count =
		json_object_is_type(contents, json_type_array) ?
			MIN(MAX_CHILDREN, json_object_array_length(contents)) :
			0;
	int gap = space(node, "spacing", 0), fixed = 0, total = 0;
	int basis[MAX_CHILDREN], before[MAX_CHILDREN], flex[MAX_CHILDREN];
	gboolean first = TRUE;

	for (size_t i = 0; i < count; i++) {
		Json *child = json_object_array_get_idx(contents, i);

		if (is(string(child, "position"), "absolute")) {
			before[i] = -1;
			continue;
		}
		before[i] = first ? space(child, "margin", 0) :
				    space(child, "margin", gap);
		first = FALSE;
		fixed += before[i];
		if (!horizontal)
			continue;
		flex[i] = child_flex(child, baseline);
		total += flex[i];
		basis[i] = flex[i] ? 0 : fixed_width(b, child, slot.w);
		fixed += basis[i];
	}
	int used = 0, height = 0, flexible = MAX(0, slot.w - fixed);
	guint start[MAX_CHILDREN], end[MAX_CHILDREN];
	int heights[MAX_CHILDREN];

	for (size_t i = 0; i < count; i++) {
		Json *child = json_object_array_get_idx(contents, i);

		heights[i] = 0;
		start[i] = b->layout->items->len;
		if (before[i] < 0) {
			end[i] = start[i];
			continue;
		}
		used += before[i];
		Rect r = slot;

		if (horizontal) {
			int share = total && flex[i] ?
					    flexible * flex[i] / total :

					    0;

			if (flex[i]) {
				total -= flex[i];
				flexible -= share;
			}
			r.w = MIN(MAX(0, slot.w - used), basis[i] + share);
			r.x += b->rtl ? slot.w - used - r.w : used;
			used += r.w;
		} else {
			r.y += used;
			r.h = MAX(0, slot.h - used);
		}
		heights[i] = component(b, child, r, clip, depth + 1, FALSE);
		end[i] = b->layout->items->len;
		if (horizontal)
			height = MAX(height, heights[i]);
		else
			used += heights[i];
	}

	if (horizontal) {
		const char *gravity = string(node, "alignItems");

		for (size_t i = 0; i < count; i++) {
			Json *child = json_object_array_get_idx(contents, i);
			const char *align = string(child, "gravity");

			if (!align)
				align = gravity;
			int dy = is(align, "center") ?
					 (height - heights[i]) / 2 :
				 is(align, "bottom") || is(align, "flex-end") ||
						 baseline ?
					 height - heights[i] :
					 0;

			if (start[i] < end[i] &&
			    is(string(child, "type"), "separator"))
				g_array_index(b->layout->items,
					      struct flex_item, start[i])
					.rect.h = height;
			else
				move_items(b, start[i], end[i], dy, clip);
		}
	} else {
		height = used;
	}
	for (size_t i = 0; i < count; i++) {
		if (before[i] >= 0)
			continue;
		Json *child = json_object_array_get_idx(contents, i);
		int top = dimension_for(child, "offsetTop", slot.h, 0);
		int left = dimension_for(
			child, b->rtl ? "offsetEnd" : "offsetStart", slot.w, 0);
		int right = dimension_for(
			child, b->rtl ? "offsetStart" : "offsetEnd", slot.w, 0);
		Rect r = { slot.x + left, slot.y + top, slot.w - left - right,
			   slot.h - top };
		int h = component(b, child, r, clip, depth + 1, FALSE);

		height = MAX(height, top + h);
	}
	return MIN(height, slot.h);
}

static int component(struct builder *b, Json *node, Rect slot, Rect clip,
		     guint depth, gboolean section)
{
	if (depth > MAX_DEPTH || b->nodes >= MAX_NODES || slot.w < 1 ||
	    slot.h < 1 || !json_object_is_type(node, json_type_object))
		return 0;
	b->nodes++;
	const char *kind = string(node, "type");
	gboolean box = is(kind, "box");
	gboolean button = is(kind, "button");
	gboolean icon = is(kind, "icon");
	gboolean image = icon || is(kind, "image");

	if (is(kind, "video")) {
		Json *alternate = field(node, "altContent");

		return component(b, alternate, slot, clip, depth + 1, section);
	}
	struct flex_item item = {
		.rect = slot,
		.clip = clip,
		.ink = 0x212126ff,
		.action = json_object_is_type(field(node, "action"),
					      json_type_object),
	};
	int height = 0;

	if (is(kind, "separator")) {
		item.fill = color(string(node, "color"), 0xdcdde1ff);
		item.rect.h = 1;
		item_add(b, item);
		return 1;
	}
	if (is(kind, "spacer"))
		return MIN(slot.h, space(node, "size", 8));
	if (button || is(kind, "text")) {
		Json *action = field(node, "action");
		const char *s = button ? string(action, "label") :
					 string(node, "text");
		guint32 tone = color(string(node, "color"),
				     button ? 0x287cc8ff : item.ink);

		item.uri = action_uri(node);
		item.label = label(b, node, s,
				   MAX(1, slot.w - (button ? 16 : 0)), button);
		if (!item.label) {
			g_free(item.uri);
			return 0;
		}
		gboolean filled = button &&
				  (is(string(node, "style"), "primary") ||
				   is(string(node, "style"), "secondary"));

		item.ink = filled ? 0xffffffff : tone;
		if (filled) {
			item.fill = color(string(node, "color"),
					  is(string(node, "style"), "primary") ?
						  0x28934dff :
						  0x626571ff);
			item.radius = 6;
		}
		pango_layout_get_pixel_size(item.label, NULL, &height);
		if (button) {
			int label_height = height;

			height = is(string(node, "height"), "sm") ? 36 : 44;
			item.label_x = 8;
			item.label_y = MAX(0, (height - label_height) / 2);
		}
	} else if (image) {
		item.url = https_url(string(node, "url"));
		if (!item.url)
			return 0;
		item.uri = action_uri(node);
		item.fill = color(string(node, "backgroundColor"), 0xe4e5eaff);
		item.cover = is(string(node, "aspectMode"), "cover");
		item.rect.w = image_width(node, slot.w, icon);
		const char *align = string(node, "align");

		if (is(align, "center") || (!align && !icon))
			item.rect.x += (slot.w - item.rect.w) / 2;
		else if (is(align, b->rtl ? "start" : "end"))
			item.rect.x += slot.w - item.rect.w;
		height = MAX(1, (int)(item.rect.w / aspect(node) + .5));
	} else if (box) {
		item.fill = color(string(node, "backgroundColor"), 0);
		item.border = color(string(node, "borderColor"), 0);
		item.stroke = space(node, "borderWidth", 0);
		if (is(string(node, "borderWidth"), "light"))
			item.stroke = 1;
		else if (is(string(node, "borderWidth"), "normal"))
			item.stroke = 2;
		else if (is(string(node, "borderWidth"), "medium"))
			item.stroke = 3;
		else if (is(string(node, "borderWidth"), "semi-bold"))
			item.stroke = 4;
		else if (is(string(node, "borderWidth"), "bold"))
			item.stroke = 5;
		item.radius = space(node, "cornerRadius", 0);
		item.uri = action_uri(node);
		item.rect.w =
			MIN(dimension_for(node, "width", slot.w, slot.w),
			    dimension_for(node, "maxWidth", slot.w, slot.w));
		Rect inner = item.rect;
		int padding = space(node, "paddingAll", section ? 14 : 0);
		int left = space(node, b->rtl ? "paddingEnd" : "paddingStart",
				 padding);
		int right = space(node, b->rtl ? "paddingStart" : "paddingEnd",
				  padding);
		int top = space(node, "paddingTop", padding);
		int bottom = space(node, "paddingBottom", padding);
		int max = dimension_for(node, "maxHeight", inner.h, inner.h);
		int fixed = dimension_for(node, "height", max, -1);
		int bound = fixed >= 0 ? fixed : max;

		inner.x += left;
		inner.y += top;
		inner.w = MAX(0, inner.w - left - right);
		inner.h = MAX(0, bound - top - bottom);
		guint index = item_add(b, item);
		guint old_visible = b->visible;

		height = box_contents(b, node, inner,
				      intersection(clip, item.rect), depth) +
			 top + bottom;
		height = fixed >= 0 ? fixed : CLAMP(height, 0, max);
		if (item.fill && fixed > 0 && b->visible == old_visible)
			b->visible++;
		if (b->visible == old_visible && !item.fill && !item.uri)
			height = 0;
		g_array_index(b->layout->items, struct flex_item, index).rect.h =
			height;
		return height;
	} else {
		return 0;
	}
	item.rect.h = CLAMP(height, 0, slot.h);
	item_add(b, item);
	b->visible++;
	return item.rect.h;
}

static int bubble_width(Json *bubble)
{
	const char *s = string(bubble, "size");

	if (is(s, "nano"))
		return 120;
	if (is(s, "micro"))
		return 160;
	if (is(s, "deca"))
		return 224;
	if (is(s, "hecto"))
		return 256;
	if (is(s, "kilo"))
		return 260;
	if (is(s, "giga"))
		return 340;
	return 300;
}

static int draw_bubble(struct builder *b, Json *bubble, int x, int y, int width)
{
	const char *const names[] = { "header", "hero", "body", "footer" };
	Json *styles = field(bubble, "styles");
	int height = 0;
	guint visible = b->visible;
	guint start = b->layout->items->len;
	Rect outer = { x, y, width, MAX_CARD_HEIGHT };

	b->rtl = is(string(bubble, "direction"), "rtl");
	guint card =
		item_add(b, (struct flex_item){ .rect = outer,
						.clip = outer,
						.fill = 0xffffffff,
						.radius = 12,
						.card = TRUE,
						.action = json_object_is_type(
							field(bubble, "action"),
							json_type_object),
						.uri = action_uri(bubble) });
	for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
		Json *block = field(bubble, names[i]);

		if (!block || height >= MAX_CARD_HEIGHT)
			continue;
		Json *style = field(styles, names[i]);
		Rect section = { x, y + height, width,
				 MAX_CARD_HEIGHT - height };
		guint line = truth(style, "separator") && height ? 1 : 0;
		guint bg = item_add(
			b, (struct flex_item){
				   .rect = section,
				   .clip = outer,
				   .fill = color(
					   string(style, "backgroundColor"), 0),
			   });

		if (line) {
			section.y++;
			section.h--;
		}
		int h = component(b, block, section, outer, 0, i != 1);

		if (line && h) {
			section.y--;
			item_add(b,
				 (struct flex_item){
					 .rect = { section.x, section.y, width,
						   1 },
					 .clip = outer,
					 .fill = color(string(style,
							      "separatorColor"),
						       0xdcdde1ff),
				 });
		}
		g_array_index(b->layout->items, struct flex_item, bg).rect.h =
			h ? MIN(section.h + (int)line, h + (int)line) : 0;
		height = MIN(MAX_CARD_HEIGHT, height + h + (h ? line : 0));
	}
	if (visible == b->visible || !height) {
		g_array_set_size(b->layout->items, start);
		return 0;
	}
	outer.h = height;
	g_array_index(b->layout->items, struct flex_item, card).rect.h = height;
	for (guint i = start; i < b->layout->items->len; i++) {
		struct flex_item *item =
			&g_array_index(b->layout->items, struct flex_item, i);
		item->clip = intersection(item->clip, outer);
	}
	return height;
}

struct ui_flex_layout *ui_flex_new(PangoContext *context, Json *flex,
				   int max_width)
{
	if (!context || max_width < 36)
		return NULL;
	if (is(string(flex, "type"), "flex"))
		flex = field(flex, "contents");
	gboolean carousel = is(string(flex, "type"), "carousel");
	Json *contents = field(flex, "contents");

	if (!carousel && !is(string(flex, "type"), "bubble"))
		return NULL;
	size_t cards =
		carousel ?
			(json_object_is_type(contents, json_type_array) ?
				 MIN(12, json_object_array_length(contents)) :
				 0) :
			1;
	struct ui_flex_layout *layout = g_new0(struct ui_flex_layout, 1);

	layout->items = g_array_new(FALSE, FALSE, sizeof(struct flex_item));
	g_array_set_clear_func(layout->items, item_clear);
	struct builder b = { .layout = layout, .context = context };
	int x = 0, y = 0, line = 0;

	for (size_t i = 0; i < cards && b.nodes < MAX_NODES && y < 3600; i++) {
		Json *card = carousel ? json_object_array_get_idx(contents, i) :
					flex;

		if (!is(string(card, "type"), "bubble"))
			continue;
		int width = MIN(max_width, bubble_width(card));

		if (x && x + width > max_width) {
			x = 0;
			y += line + CARD_GAP;
			line = 0;
		}
		int h = draw_bubble(&b, card, x, y, width);

		if (h) {
			line = MAX(line, h);
			layout->width = MAX(layout->width, x + width);
			layout->height = MAX(layout->height, y + h);
			x += width + CARD_GAP;
		}
	}
	if (!layout->height) {
		ui_flex_free(layout);
		return NULL;
	}
	return layout;
}

int ui_flex_width(const struct ui_flex_layout *layout)
{
	return layout ? layout->width : 0;
}

int ui_flex_height(const struct ui_flex_layout *layout)
{
	return layout ? layout->height : 0;
}

static void source(cairo_t *cr, guint32 rgba)
{
	cairo_set_source_rgba(cr, (rgba >> 24) / 255.,
			      ((rgba >> 16) & 255) / 255.,
			      ((rgba >> 8) & 255) / 255., (rgba & 255) / 255.);
}

static void shape(cairo_t *cr, Rect r, int radius)
{
	double s = MIN(radius, MIN(r.w, r.h) / 2.0);

	if (s <= 0) {
		cairo_rectangle(cr, r.x, r.y, r.w, r.h);
		return;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, r.x + r.w - s, r.y + s, s, -G_PI_2, 0);
	cairo_arc(cr, r.x + r.w - s, r.y + r.h - s, s, 0, G_PI_2);
	cairo_arc(cr, r.x + s, r.y + r.h - s, s, G_PI_2, G_PI);
	cairo_arc(cr, r.x + s, r.y + s, s, G_PI, 3 * G_PI_2);
	cairo_close_path(cr);
}

static void paint_image(cairo_t *cr, Rect r, cairo_surface_t *image,
			gboolean cover)
{
	if (!image || cairo_surface_get_type(image) != CAIRO_SURFACE_TYPE_IMAGE)
		return;
	int w = cairo_image_surface_get_width(image);
	int h = cairo_image_surface_get_height(image);

	if (w < 1 || h < 1)
		return;
	double sx = (double)r.w / w, sy = (double)r.h / h;
	double scale = cover ? MAX(sx, sy) : MIN(sx, sy);

	cairo_translate(cr, r.x + (r.w - w * scale) * .5,
			r.y + (r.h - h * scale) * .5);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, image, 0, 0);
	/* Art is kept at screen resolution; GOOD avoids shimmer when shrunk. */
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
}

void ui_flex_draw(cairo_t *cr, const struct ui_flex_layout *layout, int x,
		  int y, UiFlexImage image, gpointer data)
{
	if (!layout)
		return;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	double x1, y1, x2, y2;
	gboolean card = FALSE;

	cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
	for (guint i = 0; i < layout->items->len; i++) {
		const struct flex_item *item =
			&g_array_index(layout->items, struct flex_item, i);
		Rect r = item->rect, clip = intersection(r, item->clip);

		if (item->card) {
			if (card)
				cairo_restore(cr);
			card = TRUE;
			cairo_save(cr);
			shape(cr, r, item->radius);
			cairo_clip(cr);
		}
		if (!clip.w || !clip.h || clip.x >= x2 || clip.y >= y2 ||
		    clip.x + clip.w <= x1 || clip.y + clip.h <= y1)
			continue;
		if (!item->fill && !item->stroke && !item->label && !item->url)
			continue;
		cairo_save(cr);
		cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
		cairo_clip(cr);
		if (item->fill || item->stroke) {
			shape(cr, r, item->radius);
			source(cr, item->fill);
			if (item->stroke) {
				cairo_fill_preserve(cr);
				source(cr, item->border);
				cairo_set_line_width(cr, item->stroke);
				cairo_stroke(cr);
			} else {
				cairo_fill(cr);
			}
		}
		if (item->label) {
			source(cr, item->ink);
			cairo_move_to(cr, r.x + item->label_x,
				      r.y + item->label_y);
			pango_cairo_show_layout(cr, item->label);
		} else if (item->url && image) {
			paint_image(cr, r, image(item->url, data), item->cover);
		}
		cairo_restore(cr);
	}
	if (card)
		cairo_restore(cr);
	cairo_restore(cr);
}

const char *ui_flex_uri_at(const struct ui_flex_layout *layout, int x, int y)
{
	for (guint i = layout ? layout->items->len : 0; i > 0; i--) {
		const struct flex_item *item =
			&g_array_index(layout->items, struct flex_item, i - 1);
		Rect r = intersection(item->rect, item->clip);

		if (item->action && x >= r.x && x < r.x + r.w && y >= r.y &&
		    y < r.y + r.h)
			return item->uri;
	}
	return NULL;
}

void ui_flex_free(struct ui_flex_layout *layout)
{
	if (layout) {
		g_array_unref(layout->items);
		g_free(layout);
	}
}
