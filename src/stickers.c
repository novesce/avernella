// SPDX-License-Identifier: GPL-3.0-or-later
#include "stickers.h"
#include "obs_http.h"
#include <string.h>

#define SHOP_PAGE 100
#define MAX_PACKS 10000
#define MAX_PACK_ITEMS 1000
#define MAX_RANGES 100000
#define STICKER_ROOT "https://stickershop.line-scdn.net/stickershop/"

static const struct {
	const char *type, *option, *file;
} kinds[] = {
	[1] = { "STATIC", "", "sticker.png" },
	[2] = { "ANIMATION", "A", "sticker_animation.png" },
	[3] = { "SOUND", "S", "sticker.png" },
	[4] = { "ANIMATION_SOUND", "AS", "sticker_animation.png" },
	[5] = { "POPUP", "P", "sticker_popup.png" },
	[6] = { "POPUP_SOUND", "PS", "sticker_popup.png" },
};

struct sticker_range {
	gint64 start;
	guint count;
};

struct sticker_pack {
	char *id, *name, *folded, *hash;
	gint64 version, expires;
	guint kind;
	GArray *ranges;
};

struct line_stickers {
	char *owner;
	gint64 until;
	GPtrArray *packs;
	GHashTable *by_id;
};

static void pack_free(gpointer ptr)
{
	struct sticker_pack *p = ptr;

	g_free(p->id);
	g_free(p->name);
	g_free(p->folded);
	g_free(p->hash);
	g_array_unref(p->ranges);
	g_free(p);
}

LineStickers *line_stickers_new(void)
{
	return g_new0(LineStickers, 1);
}

static void clear_catalog(LineStickers *c)
{
	g_clear_pointer(&c->by_id, g_hash_table_unref);
	g_clear_pointer(&c->packs, g_ptr_array_unref);
	g_clear_pointer(&c->owner, g_free);
	c->until = 0;
}

void line_stickers_free(LineStickers *c)
{
	if (c) {
		clear_catalog(c);
		g_free(c);
	}
}

static gpointer fail(GError **e, const char *what)
{
	g_set_error_literal(e, LINE_ERROR, 1, what);
	return NULL;
}

static const char *checked_string(J *o, const char *key)
{
	const char *value = strfield(o, key);

	return value && strlen(value) == (size_t)json_object_get_string_len(
						 field(o, key)) ?
		       value :
		       NULL;
}

static gint64 integer(J *o, const char *key)
{
	J *v = field(o, key);

	return json_object_is_type(v, json_type_int) ?
		       json_object_get_int64(v) :
		       0;
}

static gboolean decimal_id(const char *id, gint64 *parsed)
{
	guint64 n;

	if (!id || id[0] < '1' || id[0] > '9' ||
	    !g_ascii_string_to_unsigned(id, 10, 1, G_MAXINT64, &n, NULL))
		return FALSE;
	if (parsed)
		*parsed = (gint64)n;
	return TRUE;
}

static gboolean talk_id(const char *id)
{
	if (!id || strlen(id) != 33 || !strchr("ucr", *id))
		return FALSE;
	return id[1 + strspn(id + 1, "0123456789abcdef")] == '\0';
}

static gboolean valid_hash(const char *hash)
{
	if (!hash || strlen(hash) > 128)
		return FALSE;
	for (const char *p = hash; *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
			return FALSE;
	return TRUE;
}

static gboolean current(gint64 expires)
{
	return expires == -1 || expires > g_get_real_time() / 1000;
}

static gint range_cmp(gconstpointer a, gconstpointer b)
{
	const struct sticker_range *x = a, *y = b;

	return (x->start > y->start) - (x->start < y->start);
}

static struct sticker_pack *parse_pack(J *product)
{
	J *summary = field(field(product, "93"), "1");
	J *ranges = field(summary, "1");
	const char *id = checked_string(product, "1"),
		   *name = checked_string(product, "11");
	const char *hash = checked_string(summary, "3");
	gint64 expires = integer(product, "94"),
	       version = integer(product, "21"), kind = integer(summary, "5");
	if (!decimal_id(id, NULL) || !current(expires) || version <= 0 ||
	    version > G_MAXUINT32 || kind < 1 ||
	    kind >= (gint64)G_N_ELEMENTS(kinds) ||
	    (field(summary, "3") && !valid_hash(hash)) ||
	    !json_object_is_type(ranges, json_type_array))
		return NULL;
	gsize count = json_object_array_length(ranges);

	if (!count || count > MAX_PACK_ITEMS)
		return NULL;
	g_autoptr(GArray) validated = g_array_sized_new(
		FALSE, FALSE, sizeof(struct sticker_range), count);
	guint total = 0;

	for (guint i = 0; i < count; i++) {
		J *r = json_object_array_get_idx(ranges, i);
		gint64 start = integer(r, "1"), size = integer(r, "2");

		if (start < 1 || size < 1 || size > MAX_PACK_ITEMS - total ||
		    start > G_MAXINT64 - (size - 1))
			return NULL;
		struct sticker_range range = { start, size };

		g_array_append_val(validated, range);
		total += size;
	}
	/* Pack ranges may arrive out of order. Never expand overlapping server data. */
	g_array_sort(validated, range_cmp);
	for (guint i = 1; i < count; i++) {
		struct sticker_range prev =
			g_array_index(validated, struct sticker_range, i - 1);
		if (g_array_index(validated, struct sticker_range, i).start -
			    prev.start <
		    prev.count)
			return NULL;
	}
	struct sticker_pack *p = g_new0(struct sticker_pack, 1);

	p->name = g_strdup(name && *name && strlen(name) <= 1024 &&
					   g_utf8_validate(name, -1, NULL) ?
				   name :
				   "LINE sticker");
	p->folded = g_utf8_casefold(p->name, -1);
	p->id = g_strdup(id);
	p->hash = g_strdup(hash);
	p->version = version;
	p->kind = kind;
	p->expires = expires;
	p->ranges = g_steal_pointer(&validated);
	return p;
}

static void putstr(J *o, const char *key, const char *v)
{
	json_object_object_add(o, key, json_object_new_string(v));
}

static void putnum(J *o, const char *key, gint64 v)
{
	json_object_object_add(o, key, json_object_new_int64(v));
}

static J *shop_request(void)
{
	const char *lang = g_get_language_names()[0];
	const char *country = lang ? strchr(lang, '_') : NULL;

	g_autofree char *language =
		lang && g_ascii_isalpha(lang[0]) && g_ascii_isalpha(lang[1]) &&
				(!lang[2] || strchr("_.@", lang[2])) ?
			g_ascii_strdown(lang, 2) :
			g_strdup("en");
	g_autofree char *region =
		country && g_ascii_isalpha(country[1]) &&
				g_ascii_isalpha(country[2]) &&
				(!country[3] || strchr(".@", country[3])) ?
			g_ascii_strup(country + 1, 2) :
			g_strdup("US");
	J *args = obj(), *locale = obj();

	putstr(locale, "1:str", language);
	putstr(locale, "2:str", region);
	json_object_object_add(args, "5:obj", locale);
	putstr(args, "2:str", "stickershop");
	putnum(args, "4:i32", SHOP_PAGE);
	return args;
}

static gboolean load_catalog(Line *s, LineStickers *c, GError **e)
{
	if (!c || !s || !s->access || !s->mid) {
		fail(e, "Sign in to open your LINE stickers.");
		return FALSE;
	}
	if (c->owner && strcmp(c->owner, s->mid))
		clear_catalog(c);
	if (c->packs && c->until > g_get_monotonic_time())
		return TRUE;
	g_autoptr(GPtrArray) packs = g_ptr_array_new_with_free_func(pack_free);
	g_autoptr(GHashTable) by_id = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) seen =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(J) args = shop_request();
	guint range_count = 0;

	for (guint offset = 0;;) {
		putnum(args, "3:i32", offset);
		g_autoptr(J) reply = line_rpc(
			s, "/TSHOP4", "getOwnedProductSummaries", args, e);
		if (!reply) {
			/* ShopException codes have a separate namespace from TalkException. */
			s->service_code = -1;
			return FALSE;
		}
		J *list = field(reply, "1");
		gint64 total = integer(reply, "3");

		if (!json_object_is_type(list, json_type_array) || total < 0 ||
		    total > MAX_PACKS)
			break;
		gsize n = json_object_array_length(list);

		if (n > SHOP_PAGE || offset + n > MAX_PACKS ||
		    (!n && total > offset))
			break;
		for (guint i = 0; i < n; i++) {
			J *product = json_object_array_get_idx(list, i);
			const char *id = checked_string(product, "1");

			if (!decimal_id(id, NULL))
				continue;
			if (!g_hash_table_add(seen, g_strdup(id)))
				goto malformed;
			struct sticker_pack *p = parse_pack(product);

			if (p) {
				range_count += p->ranges->len;
				g_ptr_array_add(packs, p);
				g_hash_table_insert(by_id, p->id, p);
				if (range_count > MAX_RANGES)
					goto malformed;
			}
		}
		offset += n;
		if (!n || (total ? offset >= total : n < SHOP_PAGE)) {
			clear_catalog(c);
			c->packs = g_steal_pointer(&packs);
			c->by_id = g_steal_pointer(&by_id);
			c->owner = g_strdup(s->mid);
			c->until = g_get_monotonic_time() +
				   10 * 60 * G_USEC_PER_SEC;
			return TRUE;
		}
	}
malformed:
	fail(e,
	     "LINE returned an inconsistent sticker catalog. Reopen the picker to retry.");
	return FALSE;
}

static J *sticker_row(const struct sticker_pack *p, gint64 item)
{
	char id[32];

	g_snprintf(id, sizeof(id), "%" G_GINT64_FORMAT, item);
	g_autofree char *label = g_strdup_printf("%s · %s", p->name, id);
	J *row = obj();

	putstr(row, "packageId", p->id);
	putstr(row, "stickerId", id);
	putnum(row, "version", p->version);
	putstr(row, "resourceType", kinds[p->kind].type);
	putstr(row, "pack", p->name);
	putstr(row, "label", label);
	return row;
}

J *line_sticker_catalog(Line *s, LineStickers *c, guint offset, guint limit,
			const char *query, GError **e)
{
	if (query && (strlen(query) > 512 || !g_utf8_validate(query, -1, NULL)))
		return fail(
			e, "Enter a shorter sticker pack name or item number.");
	if (!load_catalog(s, c, e))
		return NULL;
	g_autofree char *needle = g_utf8_casefold(query ? query : "", -1);

	g_strstrip(needle);
	gint64 target = 0;

	decimal_id(needle, &target);
	limit = CLAMP(limit, 1, 128);
	J *rows = json_object_new_array();

	for (guint i = 0; i < c->packs->len && limit; i++) {
		struct sticker_pack *p = c->packs->pdata[i];

		if (!current(p->expires))
			continue;
		gboolean whole_pack = !*needle || strstr(p->folded, needle) ||
				      !strcmp(p->id, needle);
		if (!whole_pack && !target)
			continue;
		for (guint j = 0; j < p->ranges->len && limit; j++) {
			struct sticker_range r = g_array_index(
				p->ranges, struct sticker_range, j);
			if (!whole_pack) {
				if (target < r.start ||
				    target - r.start >= r.count)
					continue;
				r = (struct sticker_range){ target, 1 };
			}
			if (offset >= r.count) {
				offset -= r.count;
				continue;
			}
			for (guint k = offset; k < r.count && limit;
			     k++, limit--)
				json_object_array_add(
					rows, sticker_row(p, r.start + k));
			offset = 0;
		}
	}
	return rows;
}

static struct sticker_pack *owned(LineStickers *c, const char *product,
				  const char *id, GError **e)
{
	gint64 item;

	if (c && c->by_id && c->until > g_get_monotonic_time() &&
	    decimal_id(product, NULL) && decimal_id(id, &item)) {
		struct sticker_pack *p = g_hash_table_lookup(c->by_id, product);

		if (p && current(p->expires)) {
			for (guint i = 0; i < p->ranges->len; i++) {
				struct sticker_range r = g_array_index(
					p->ranges, struct sticker_range, i);
				if (item >= r.start && item - r.start < r.count)
					return p;
			}
		}
	}
	return fail(
		e,
		"Choose an available sticker from your LINE sticker picker.");
}

static char *asset_url(const char *id, const char *hash, guint kind)
{
	const char *name = kinds[kind].file;

	if (hash && *hash)
		return g_strdup_printf(STICKER_ROOT
				       "v2/sticker/%s/%s/android/%s",
				       id, hash, name);
	return g_strdup_printf(STICKER_ROOT "v1/sticker/%s/android/%s", id,
			       name);
}

char *line_sticker_asset(LineStickers *c, const char *product, const char *id,
			 GError **e)
{
	struct sticker_pack *p = owned(c, product, id, e);

	return p ? asset_url(id, p->hash, p->kind) : NULL;
}

char *line_sticker_message_asset(J *message, GError **e)
{
	J *meta = field(message, "18");
	const char *id = checked_string(meta, "STKID"),
		   *hash = checked_string(meta, "STKHASH"),
		   *opt = checked_string(meta, "STKOPT");
	if (integer(message, "15") != 7 || !decimal_id(id, NULL) ||
	    (field(meta, "STKHASH") && !valid_hash(hash)))
		return fail(e,
			    "The LINE sticker has an invalid image reference.");
	guint kind = 1;

	for (guint i = 2; opt && i < G_N_ELEMENTS(kinds); i++)
		if (!strcmp(opt, kinds[i].option)) {
			kind = i;
			break;
		}
	return asset_url(id, hash, kind);
}

static gboolean trusted_image_url(const char *url)
{
	if (!url || !g_str_has_prefix(url, STICKER_ROOT) || strlen(url) > 320)
		return FALSE;
	g_auto(GStrv) fields = g_strsplit(url + strlen(STICKER_ROOT), "/", -1);
	guint n = g_strv_length(fields);

	if (!((n == 5 && !strcmp(fields[0], "v1")) ||
	      (n == 6 && !strcmp(fields[0], "v2") && *fields[3] &&
	       valid_hash(fields[3]))) ||
	    strcmp(fields[1], "sticker") || !decimal_id(fields[2], NULL) ||
	    strcmp(fields[n - 2], "android"))
		return FALSE;
	for (guint i = 1; i < G_N_ELEMENTS(kinds); i++)
		if (!strcmp(fields[n - 1], kinds[i].file))
			return TRUE;
	return FALSE;
}

GBytes *line_sticker_fetch(const char *url, GError **e)
{
	const gsize max = 8 * 1024 * 1024;

	if (!trusted_image_url(url))
		return fail(e, "The LINE sticker image URL is invalid.");
	g_autoptr(GError) error = NULL;
	GBytes *data = line_public_asset(url, max, &error);

	if (!data && g_error_matches(error, LINE_ERROR, LINE_ASSET_NOT_FOUND) &&
	    !g_str_has_suffix(url, "/sticker.png")) {
		g_autofree char *prefix =
			g_strndup(url, strrchr(url, '/') - url);
		g_autofree char *poster =
			g_strconcat(prefix, "/sticker.png", NULL);
		g_clear_error(&error);
		data = line_public_asset(poster, max, &error);
	}
	if (!data) {
		if (error)
			g_propagate_error(e, g_steal_pointer(&error));
		else
			fail(e, "LINE did not return a sticker image.");
		return NULL;
	}
	gsize size;
	const guint8 *bytes = g_bytes_get_data(data, &size);

	if (size < 33 || size > max ||
	    memcmp(bytes, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16)) {
		g_bytes_unref(data);
		return fail(e, "LINE returned an unreadable sticker image.");
	}
	/* The native image decoder validates the remaining PNG chunks and dimensions. */
	return data;
}

J *line_send_sticker(Line *s, LineStickers *c, const char *chat,
		     const char *product, const char *id, const char *reply_to,
		     gint32 reserved_seq, GError **e)
{
	if (!talk_id(chat) || reserved_seq <= 0 || !decimal_id(product, NULL) ||
	    !decimal_id(id, NULL) || (reply_to && !decimal_id(reply_to, NULL)))
		return fail(e, "Select a chat and a LINE sticker to send.");
	if (!load_catalog(s, c, e))
		return NULL;
	struct sticker_pack *p = owned(c, product, id, e);

	if (!p)
		return NULL;
	char version[32];

	g_snprintf(version, sizeof(version), "%" G_GINT64_FORMAT, p->version);
	g_autoptr(J) args = obj();
	J *msg = obj(), *meta = obj();

	putnum(args, "1:i32", reserved_seq);
	json_object_object_add(args, "2:obj", msg);
	putstr(msg, "2:str", chat);
	putnum(msg, "5:i64", 0);
	putnum(msg, "6:i64", 0);
	json_object_object_add(msg, "14:bool", json_object_new_boolean(FALSE));
	putnum(msg, "15:i32", 7);
	json_object_object_add(msg, "18:map:str:str", meta);
	putnum(msg, "19:byte", 0);
	putstr(meta, "STKID", id);
	putstr(meta, "STKPKGID", product);
	putstr(meta, "STKVER", version);
	putstr(meta, "STKTXT", p->name);
	if (*kinds[p->kind].option)
		putstr(meta, "STKOPT", kinds[p->kind].option);
	if (p->hash && *p->hash)
		putstr(meta, "STKHASH", p->hash);
	if (reply_to) {
		putstr(msg, "21:str", reply_to);
		putnum(msg, "22:i32", 3);
		putnum(msg, "24:i32", 1);
	}
	g_autoptr(J) reply = line_rpc(s, "/S4", "sendMessage", args, e);
	if (!reply) {
		/* USER_NOT_STICKER_OWNER: refresh entitlements on the next picker request. */
		if (s->service_code == 32)
			clear_catalog(c);
		return NULL;
	}
	const char *sent_id = checked_string(reply, "4");

	if (!decimal_id(sent_id, NULL))
		return fail(
			e,
			"LINE returned no valid message ID. Check the chat before resending.");
	J *receipt = obj();

	putstr(receipt, "id", sent_id);
	return receipt;
}
