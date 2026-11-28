// SPDX-License-Identifier: GPL-3.0-or-later
#include "emoji.h"
#include "obs_http.h"
#include <gio/gio.h>

struct default_emoji_pack {
	const char *id, *name;
	int version, count;
#ifdef LINE_PRIVATE_EMOJI_LABELS
	gsize bytes;
	const char *compressed;
#endif
};

static const struct default_emoji_pack defaults[] = {
#ifdef LINE_PRIVATE_EMOJI_LABELS
#include "emoji_defaults.inc"
#else
#include "emoji_pack_index.inc"
#endif
};

struct emoji_pack {
	char *id, *name, *labels;
	const struct default_emoji_pack *builtin;
	int version, count;
	gboolean animated;
	gint64 retry_after;
};

static void pack_free(gpointer value)
{
	struct emoji_pack *p = value;

	g_free(p->id);
	g_free(p->name);
	g_free(p->labels);
	g_free(p);
}

static const struct default_emoji_pack *find_default(const char *id)
{
	for (guint i = 0; i < G_N_ELEMENTS(defaults); i++)
		if (!strcmp(id, defaults[i].id))
			return &defaults[i];
	return NULL;
}

static const char *catalog_string(J *item, const char *key)
{
	J *value = field(item, key);

	if (!json_object_is_type(value, json_type_string))
		return NULL;
	const char *text = json_object_get_string(value);

	if (strlen(text) != (size_t)json_object_get_string_len(value))
		return NULL;
	return text;
}

static struct emoji_pack *add_pack(GPtrArray *packs, GHashTable *seen,
				   const char *id, const char *name,
				   int version, gboolean animated)
{
	if (!line_emoji_id_valid(id, "001") || g_hash_table_contains(seen, id))
		return NULL;
	struct emoji_pack *p = g_new0(struct emoji_pack, 1);

	p->id = g_strdup(id);
	p->builtin = find_default(id);
	if (p->builtin)
		p->count = p->builtin->count;
	if (!name || !*name || strlen(name) > 255 ||
	    !g_utf8_validate(name, -1, NULL))
		name = p->builtin ? p->builtin->name : "LINE emoji";
	p->name = g_strdup(name);
	p->version = version > 0 ? version :
		     p->builtin	 ? p->builtin->version :
				   1;
	p->animated = animated;
	g_hash_table_add(seen, p->id);
	g_ptr_array_add(packs, p);
	return p;
}

static void load_owned(Line *s, GPtrArray *packs, GHashTable *seen)
{
	g_autoptr(J) args = obj();
	J *locale = obj();
	const char *const *langs = g_get_language_names();
	const char *local = langs && langs[0] ? langs[0] : NULL;

	if (!local || !strcmp(local, "C") || g_str_has_prefix(local, "C."))
		local = g_getenv("LANG");
	g_autofree char *lang = g_strdup(local && *local ? local : "en_US");
	char *country = strchr(lang, '_');

	if (country)
		*country++ = 0;
	lang[strcspn(lang, ".@")] = 0;
	if (country)
		country[strcspn(country, ".@")] = 0;
	json_object_object_add(
		locale, "1:str",
		json_object_new_string(strlen(lang) == 2 ? lang : "en"));
	/* Both fields are required. The client's default display locale is en_US. */
	json_object_object_add(
		locale, "2:str",
		json_object_new_string(
			country && strlen(country) == 2 ? country : "US"));
	json_object_object_add(args, "5:obj", locale);
	json_object_object_add(args, "2:str",
			       json_object_new_string("sticonshop"));
	json_object_object_add(args, "4:i32", json_object_new_int(100));
	/*
	 * The Android sync requests 100 summaries at a time. Stop on a repeated
	 * page as well as an empty page or total, including on older servers.
	 */
	g_autofree char *previous_first = NULL, *previous_last = NULL;

	for (int offset = 0; offset < 10000;) {
		json_object_object_add(args, "3:i32",
				       json_object_new_int(offset));
		g_autoptr(GError) error = NULL;
		g_autoptr(J) reply = line_rpc(
			s, "/TSHOP4", "getOwnedProductSummaries", args, &error);
		J *list = field(reply, "1");

		if (!json_object_is_type(list, json_type_array))
			break;
		int count = MIN(json_object_array_length(list), 100);

		if (!count)
			break;
		const char *first =
			catalog_string(json_object_array_get_idx(list, 0), "1");
		const char *last = catalog_string(
			json_object_array_get_idx(list, count - 1), "1");
		if (offset && !g_strcmp0(first, previous_first) &&
		    !g_strcmp0(last, previous_last))
			break;
		g_free(previous_first);
		g_free(previous_last);
		previous_first = g_strdup(first);
		previous_last = g_strdup(last);
		for (int i = 0; i < count; i++) {
			J *summary = json_object_array_get_idx(list, i);
			const char *id = catalog_string(summary, "1");
			J *sticon = field(field(summary, "93"), "3");
			gint64 expires_ms =
				json_object_get_int64(field(summary, "94"));
			if (expires_ms > 0 &&
			    expires_ms <= g_get_real_time() / 1000)
				continue;
			add_pack(packs, seen, id, catalog_string(summary, "11"),
				 json_object_get_int(field(summary, "21")),
				 json_object_get_int(field(sticon, "3")) == 2);
		}
		offset += count;
		int total = json_object_get_int(field(reply, "3"));

		if (count < 100 || (total > 0 && offset >= total))
			break;
	}
	/* A ShopException has a different error enum from TalkException. */
	s->service_code = -1;
}

static void load_packs(Line *s)
{
	g_clear_pointer(&s->emoji_packs, g_ptr_array_unref);
	GPtrArray *packs = g_ptr_array_new_with_free_func(pack_free);

	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	if (s->access)
		load_owned(s, packs, seen);
	for (guint i = 0; i < G_N_ELEMENTS(defaults); i++) {
		const struct default_emoji_pack *d = &defaults[i];

		add_pack(packs, seen, d->id, d->name, d->version, FALSE);
	}
	s->emoji_packs = packs;
	s->emoji_catalog_until =
		g_get_monotonic_time() + 10 * 60 * G_USEC_PER_SEC;
}

#ifdef LINE_PRIVATE_EMOJI_LABELS
/* Inflate only a pack that contributes a page or needs a label lookup. */
static gboolean unpack_labels(struct emoji_pack *p)
{
	if (p->labels)
		return TRUE;
	const struct default_emoji_pack *d = p->builtin;
	gsize n, consumed = 0, written = 0;

	g_autofree guint8 *compressed = g_base64_decode(d->compressed, &n);
	g_autofree char *labels = g_malloc(d->bytes);

	g_autoptr(GZlibDecompressor) unzip =
		g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
	GConverterResult result = g_converter_convert(
		G_CONVERTER(unzip), compressed, n, labels, d->bytes,
		G_CONVERTER_INPUT_AT_END, &consumed, &written, NULL);
	if (result == G_CONVERTER_ERROR || consumed != n ||
	    written != d->bytes || labels[written - 1])
		return FALSE;
	p->labels = g_steal_pointer(&labels);
	return TRUE;
}

#endif

static gboolean load_manifest(struct emoji_pack *p)
{
	if (p->labels)
		return TRUE;
	gint64 now = g_get_monotonic_time();

	if (now < p->retry_after)
		return FALSE;
	p->retry_after = now + G_USEC_PER_SEC * 60;
	g_autofree char *url = g_strdup_printf(
		"https://stickershop.line-scdn.net/sticonshop/v1/product/%s/android/meta.json",
		p->id);
	g_autoptr(GBytes) data = line_public_asset(url, 256 * 1024, NULL);
	if (!data)
		return FALSE;
	gsize n;
	const char *bytes = g_bytes_get_data(data, &n);
	struct json_tokener *parser = json_tokener_new();

	json_tokener_set_flags(parser, JSON_TOKENER_STRICT |
					       JSON_TOKENER_VALIDATE_UTF8);
	g_autoptr(J) meta = json_tokener_parse_ex(parser, bytes, (int)n);
	gboolean valid = json_tokener_get_error(parser) ==
				 json_tokener_success &&
			 json_tokener_get_parse_end(parser) == n;

	json_tokener_free(parser);
	J *order = field(meta, "orders");

	if (!valid || g_strcmp0(catalog_string(meta, "productId"), p->id) ||
	    !json_object_is_type(order, json_type_array) ||
	    json_object_array_length(order) > 999)
		return FALSE;
	J *alt = field(meta, "altTexts");
	gboolean seen[1000] = { 0 };
	int count = 0;
	GString *pairs = g_string_sized_new(n / 2);

	for (size_t i = 0; i < json_object_array_length(order); i++) {
		J *item = json_object_array_get_idx(order, i);

		if (!json_object_is_type(item, json_type_string) ||
		    json_object_get_string_len(item) != 3)
			continue;
		const char *id = json_object_get_string(item);

		if (!line_emoji_id_valid(p->id, id))
			continue;
		int index =
			(id[0] - '0') * 100 + (id[1] - '0') * 10 + id[2] - '0';
		if (seen[index])
			continue;
		seen[index] = TRUE;
		const char *label = catalog_string(alt, id);

		if (!label || !*label || strlen(label) > 200 ||
		    !g_utf8_validate(label, -1, NULL))
			label = p->name;
		if (p->builtin && index != count + 1) {
			g_string_free(pairs, TRUE);
			return FALSE;
		}
		if (!p->builtin)
			g_string_append_len(pairs, id, 4);
		g_string_append_len(pairs, label, strlen(label) + 1);
		count++;
	}
	if (p->builtin && count != p->builtin->count) {
		g_string_free(pairs, TRUE);
		return FALSE;
	}
	p->count = count;
	p->labels = g_string_free(pairs, FALSE);
	return TRUE;
}

static gboolean matches(const char *s, const char *query)
{
	g_autofree char *folded = g_utf8_casefold(s, -1);

	return folded && strstr(folded, query);
}

J *line_emoji_catalog_page(Line *s, guint offset, guint limit,
			   const char *query, GError **error)
{
	if (!s || (query && (strlen(query) > 512 ||
			     !g_utf8_validate(query, -1, NULL)))) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid emoji search.");
		return NULL;
	}
	if (!s->emoji_packs ||
	    (!offset && g_get_monotonic_time() >= s->emoji_catalog_until))
		load_packs(s);
	g_autofree char *q = g_utf8_casefold(query ? query : "", -1);

	g_strstrip(q);
	limit = limit ? MIN(limit, 128u) : 96;
	J *rows = json_object_new_array_ext((int)limit);
	unsigned int visible_manifest_budget = 2;
	gboolean item_query = strlen(q) <= 3 && !q[strspn(q, "0123456789")];

	for (guint i = 0; i < s->emoji_packs->len; i++) {
		struct emoji_pack *p = g_ptr_array_index(s->emoji_packs, i);

		gboolean whole = !*q || strstr(p->id, q) || matches(p->name, q);
		/*
		 * A text search uses already-viewed manifests. A pack-name search
		 * may fetch that pack as it becomes visible.
		 */
		if (!p->builtin && !p->labels && (!whole || !load_manifest(p)))
			continue;
		int count = p->count;

		if (whole && offset >= (guint)count) {
			offset -= count;
			continue;
		}
		if (p->builtin && !p->labels) {
#ifdef LINE_PRIVATE_EMOJI_LABELS
			unpack_labels(p);
#endif
			if (!p->labels && whole && s->access &&
			    visible_manifest_budget &&
			    g_get_monotonic_time() >= p->retry_after) {
				visible_manifest_budget--;
				load_manifest(p);
			}
		}
		if (!whole && !p->labels && !item_query)
			continue;
		const char *next_label = p->labels;

		for (int k = 0; k < count; k++) {
			char number[4];
			const char *id, *name;

			if (p->builtin) {
				g_snprintf(number, sizeof(number), "%03d",
					   k + 1);
				id = number;
			} else {
				id = next_label;
				next_label += 4;
			}
			name = next_label;
			if (next_label)
				next_label += strlen(next_label) + 1;
			if (!whole && !strstr(id, q) &&
			    (!name || !matches(name, q)))
				continue;
			if (offset) {
				offset--;
				continue;
			}
			g_autofree char *fallback =
				name ? NULL :
				       g_strdup_printf("%s %s", p->name, id);
			J *r = obj();

			json_object_object_add(r, "productId",
					       json_object_new_string(p->id));
			json_object_object_add(r, "emojiId",
					       json_object_new_string(id));
			json_object_object_add(
				r, "label",
				json_object_new_string(name ? name : fallback));
			json_object_object_add(r, "pack",
					       json_object_new_string(p->name));
			json_object_object_add(
				r, "resourceType",
				json_object_new_string(
					p->animated ? "ANIMATION" : "STATIC"));
			json_object_object_add(r, "version",
					       json_object_new_int(p->version));
			json_object_array_add(rows, r);
			if (json_object_array_length(rows) >= limit)
				return rows;
		}
	}
	return rows;
}

J *line_emoji_catalog(Line *s, GError **error)
{
	return line_emoji_catalog_page(s, 0, 96, NULL, error);
}

GBytes *line_emoji_image_variant(const char *product, const char *item,
				 gboolean animated, GError **error)
{
	if (!line_emoji_id_valid(product, item)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Invalid LINE emoji identifier.");
		return NULL;
	}
	g_autofree char *url = g_strdup_printf(
		"https://stickershop.line-scdn.net/sticonshop/v1/sticon/%s/android/%s%s.png",
		product, item, animated ? "_animation" : "");
	g_autoptr(GError) problem = NULL;
	g_autoptr(GBytes) image =
		line_public_asset(url, 4 * 1024 * 1024, &problem);
	if (!image) {
		if (animated &&
		    g_error_matches(problem, LINE_ERROR, LINE_ASSET_NOT_FOUND))
			return line_emoji_image_variant(product, item, FALSE,
							error);
		if (problem)
			g_propagate_error(error, g_steal_pointer(&problem));
		return NULL;
	}
	gsize n;
	const guint8 *p = g_bytes_get_data(image, &n);

	if (n < 33 || memcmp(p, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16)) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "LINE returned an invalid emoji image.");
		return NULL;
	}
	return g_steal_pointer(&image);
}

GBytes *line_emoji_image(const char *product, const char *item, GError **error)
{
	return line_emoji_image_variant(product, item, FALSE, error);
}
