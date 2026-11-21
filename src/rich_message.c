// SPDX-License-Identifier: GPL-3.0-or-later
#include "liff_url.h"
#include "rich_message.h"
#include <string.h>

enum {
	MARKUP_LIMIT = 64 * 1024,
	MARKUP_DEPTH = 16,
	MAX_SCENES = 8,
	MAX_DRAWS = 16,
	MAX_AREAS = 50,
	MAX_SIDE = 4096,
	TEXT_LIMIT = 5000,
	URL_LIMIT = 2048
};

static const char *text(J *o, const char *key, gsize limit)
{
	J *v = field(o, key);

	if (!json_object_is_type(v, json_type_string))
		return NULL;
	const char *s = json_object_get_string(v);
	gsize n = json_object_get_string_len(v);

	return n && n <= limit && strlen(s) == n &&
			       g_utf8_validate(s, n, NULL) ?
		       s :
		       NULL;
}

static gboolean number(J *v, int *out)
{
	if (!json_object_is_type(v, json_type_int) &&
	    !json_object_is_type(v, json_type_double))
		return FALSE;
	double d = json_object_get_double(v);

	if (d < 0 || d > MAX_SIDE)
		return FALSE;
	*out = (int)d;
	return TRUE;
}

/* x, y, w, h from named fields or a four-number array, inside the canvas. */
static gboolean box(J *o, int cw, int ch, int out[4])
{
	static const char *const keys[] = { "x", "y", "w", "h" };

	for (int i = 0; i < 4; i++) {
		J *v = json_object_is_type(o, json_type_array) ?
			       json_object_array_get_idx(o, i) :
			       field(o, keys[i]);
		if (!number(v, &out[i]))
			return FALSE;
	}
	return out[2] > 0 && out[3] > 0 && out[0] + out[2] <= cw &&
	       out[1] + out[3] <= ch;
}

static gboolean https(const char *url)
{
	if (!url || !g_str_has_prefix(url, "https://") ||
	    strlen(url) > URL_LIMIT)
		return FALSE;
	for (const char *p = url; *p; p++)
		if ((guchar)*p <= ' ' || *p == 0x7f)
			return FALSE;
	return url[8] && url[8] != '/';
}

static gboolean scene_name(const char *s)
{
	if (!s || !*s || strlen(s) > 64)
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && *s != '_' && *s != '-')
			return FALSE;
	return TRUE;
}

static void put_int(J *o, const char *k, int v)
{
	json_object_object_add(o, k, json_object_new_int(v));
}

static void put_str(J *o, const char *k, const char *v)
{
	json_object_object_add(o, k, json_object_new_string(v));
}

static void put_box(J *o, const int b[4])
{
	put_int(o, "x", b[0]);
	put_int(o, "y", b[1]);
	put_int(o, "w", b[2]);
	put_int(o, "h", b[3]);
}

/* Maps one action to a clickable area, or FALSE when unsupported. */
static gboolean action_area(J *action, J *scenes, J *area)
{
	const char *type = text(action, "type", 32);
	J *params = field(action, "params");
	const char *label = text(action, "text", 400);
	const char *kind = NULL, *value = NULL;

	if (!g_strcmp0(type, "web") || !g_strcmp0(type, "postback")) {
		value = text(params, "linkUri", URL_LIMIT);
		if (!https(value) && !line_liff_url(value, NULL, NULL))
			value = text(params, "httpUri", URL_LIMIT);
		kind = https(value) || line_liff_url(value, NULL, NULL) ?
			       "link" :
			       NULL;
	} else if (!g_strcmp0(type, "sendMessage")) {
		value = text(params, "text", TEXT_LIMIT);
		kind = value ? "message" : NULL;
	} else if (!g_strcmp0(type, "clipboard")) {
		value = text(params, "clipboardText", TEXT_LIMIT);
		kind = value ? "copy" : NULL;
	} else if (!g_strcmp0(type, "transition")) {
		value = text(params, "nextScene", 64);
		kind = scene_name(value) && field(scenes, value) ? "scene" :
								   NULL;
	}
	if (!kind)
		return FALSE;
	put_str(area, "kind", kind);
	put_str(area, "value", value);
	if (label)
		put_str(area, "label", label);
	return TRUE;
}

static J *scene(J *raw, J *images, J *actions, J *scenes, const char *base,
		int cw, int ch)
{
	J *draws = field(raw, "draws"), *listeners = field(raw, "listeners");

	if (!json_object_is_type(draws, json_type_array) ||
	    json_object_array_length(draws) > MAX_DRAWS ||
	    (listeners && !json_object_is_type(listeners, json_type_array)))
		return NULL;
	g_autoptr(J) out = obj();
	J *list = json_object_new_array(), *areas = json_object_new_array();

	json_object_object_add(out, "draws", list);
	json_object_object_add(out, "areas", areas);
	for (size_t i = 0; i < json_object_array_length(draws); i++) {
		J *d = json_object_array_get_idx(draws, i);
		const char *name = text(d, "image", 64);
		J *image = name ? field(images, name) : NULL;
		int at[4], src[4];

		if (!image || !box(d, cw, ch, at))
			return NULL;
		const char *own = text(image, "imageUrl", URL_LIMIT);
		J *draw = obj();

		put_box(draw, at);
		if (own && https(own)) {
			/* A separate picture fills its whole draw rectangle. */
			put_str(draw, "url", own);
			json_object_object_add(draw, "whole",
					       json_object_new_boolean(TRUE));
		} else if (base && box(image, cw, ch, src)) {
			g_autofree char *url = g_strdup_printf(
				"%s/%d", base, LINE_RICH_IMAGE_WIDTH);

			put_str(draw, "url", url);
			put_int(draw, "sx", src[0]);
			put_int(draw, "sy", src[1]);
			put_int(draw, "sw", src[2]);
			put_int(draw, "sh", src[3]);
		} else {
			json_object_put(draw);
			return NULL;
		}
		json_object_array_add(list, draw);
	}
	for (size_t i = 0;
	     listeners && i < json_object_array_length(listeners) &&
	     json_object_array_length(areas) < MAX_AREAS;
	     i++) {
		J *l = json_object_array_get_idx(listeners, i);
		const char *name = text(l, "action", 64);
		J *action = name ? field(actions, name) : NULL;
		int at[4];

		if (g_strcmp0(text(l, "type", 16), "touch") || !action ||
		    !box(field(l, "params"), cw, ch, at))
			continue;
		J *area = obj();

		put_box(area, at);
		if (action_area(action, scenes, area))
			json_object_array_add(areas, area);
		else
			json_object_put(area);
	}
	return g_steal_pointer(&out);
}

static J *markup(J *metadata)
{
	const char *encoded = text(metadata, "MARKUP_JSON", MARKUP_LIMIT);

	if (!encoded)
		encoded = text(metadata, "RICH_MARKUP_JSON", MARKUP_LIMIT);
	if (!encoded)
		return NULL;
	struct json_tokener *reader = json_tokener_new_ex(MARKUP_DEPTH);

	if (!reader)
		return NULL;
	json_tokener_set_flags(reader, JSON_TOKENER_STRICT |
					       JSON_TOKENER_VALIDATE_UTF8);
	size_t size = strlen(encoded);
	J *value = json_tokener_parse_ex(reader, encoded, (int)size);

	if (json_tokener_get_error(reader) != json_tokener_success ||
	    json_tokener_get_parse_end(reader) != size ||
	    !json_object_is_type(value, json_type_object)) {
		json_object_put(value);
		value = NULL;
	}
	json_tokener_free(reader);
	return value;
}

J *line_rich_parse(J *metadata, char **alt)
{
	const char *alt_text = text(metadata, "ALT_TEXT", 8192);

	if (!alt_text)
		alt_text = text(metadata, "RICH_ALT_TEXT", 8192);
	if (alt)
		*alt = g_strdup(alt_text);
	g_autoptr(J) doc = markup(metadata);
	J *canvas = field(doc, "canvas"), *images = field(doc, "images"),
	  *actions = field(doc, "actions"), *scenes = field(doc, "scenes");
	const char *base = text(metadata, "DOWNLOAD_URL", URL_LIMIT);
	int cw, ch;

	if (!base)
		base = text(metadata, "RICH_DOWNLOAD_URL", URL_LIMIT);
	if (base && (!https(base) || strchr(base, '?') || strchr(base, '#')))
		base = NULL;
	const char *first = text(canvas, "initialScene", 64);

	if (!number(field(canvas, "width"), &cw) ||
	    !number(field(canvas, "height"), &ch) || !cw || !ch ||
	    !json_object_is_type(images, json_type_object) ||
	    !json_object_is_type(scenes, json_type_object) ||
	    (actions && !json_object_is_type(actions, json_type_object)) ||
	    json_object_object_length(scenes) > MAX_SCENES ||
	    !scene_name(first) || !field(scenes, first))
		return NULL;
	g_autoptr(J) out = obj();
	J *parsed = obj();

	put_int(out, "w", cw);
	put_int(out, "h", ch);
	put_str(out, "scene", first);
	json_object_object_add(out, "scenes", parsed);
	json_object_object_foreach(scenes, name, raw) {
		if (!scene_name(name))
			return NULL;
		J *one = scene(raw, images, actions, scenes, base, cw, ch);

		if (!one)
			return NULL;
		json_object_object_add(parsed, name, one);
	}
	return g_steal_pointer(&out);
}
