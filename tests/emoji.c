// SPDX-License-Identifier: GPL-3.0-or-later
#include "emoji.h"
#include "obs_http.h"

#define BASIC "670e0cce840a8236ddd4ee4c"
#define OWN "123456789abcdef012345678"

static int rpc_count, manifest_count, builtin_fetches, downloads, missing;
static gboolean shop_unavailable;
static char *last_url;
static const char *manifest;

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)s;
	rpc_count++;
	g_assert_cmpstr(path, ==, "/TSHOP4");
	g_assert_cmpstr(method, ==, "getOwnedProductSummaries");
	g_assert_cmpstr(strfield(args, "2:str"), ==, "sticonshop");
	g_assert_cmpint(json_object_get_int(field(args, "4:i32")), ==, 100);
	g_assert_nonnull(strfield(field(args, "5:obj"), "2:str"));
	if (shop_unavailable) {
		g_set_error_literal(error, LINE_ERROR, LINE_METHOD_UNAVAILABLE,
				    "Shop unavailable");
		return NULL;
	}
	return json_tokener_parse(
		"{\"1\":[{\"1\":\"" OWN
		"\",\"11\":\"My Pack\",\"21\":4,\"93\":{\"3\":{\"3\":2}}},{\"1\":\"" BASIC
		"\",\"11\":\"LINE Basic Emoji\"},{\"1\":\"../bogus\"},{\"1\":\"ab3456789abcdef012345678\",\"94\":1}],\"3\":4}");
}

GBytes *line_public_asset(const char *url, gsize limit, GError **error)
{
	static const char meta[] =
		"{\"productId\":\"" OWN
		"\",\"orders\":[\"009\",\"001\",\"009\",\"001/illegal\"],"
		"\"altTexts\":{\"009\":\"Crème, CAT!\",\"001\":\"Moon\"}}\n";
	static const guint8 png[33] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR";

	g_free(last_url);
	last_url = g_strdup(url);
	if (g_str_has_suffix(url, "/meta.json")) {
		if (strstr(url, "/" BASIC "/")) {
			builtin_fetches++;
			GString *json = g_string_new("{\"productId\":\"" BASIC
						     "\",\"orders\":[");
			for (int i = 1; i <= 211; i++)
				g_string_append_printf(json, "%s\"%03d\"",
						       i > 1 ? "," : "", i);
			g_string_append(
				json,
				"],\"altTexts\":{\"001\":\"Fixture sunrise\"}}");
			return g_string_free_to_bytes(json);
		}
		manifest_count++;
		g_assert_cmpstr(
			url, ==,
			"https://stickershop.line-scdn.net/sticonshop/v1/product/" OWN
			"/android/meta.json");
		g_assert_cmpuint(limit, <=, 256 * 1024);
		return manifest ?
			       g_bytes_new_static(manifest, strlen(manifest)) :
			       g_bytes_new_static(meta, sizeof(meta) - 1);
	}
	downloads++;
	if (missing && g_str_has_suffix(url, "_animation.png")) {
		g_set_error_literal(error, LINE_ERROR,
				    missing == 1 ? LINE_ASSET_NOT_FOUND : 1,
				    "HTTP error");
		return NULL;
	}
	return g_bytes_new_static(png, sizeof(png));
}

static void defaults_only(void)
{
	rpc_count = builtin_fetches = manifest_count = 0;
	Line s = { 0 };

	g_autoptr(J) first = line_emoji_catalog(&s, NULL);
	g_assert_cmpuint(json_object_array_length(first), ==, 96);
	J *row = json_object_array_get_idx(first, 0);

	g_assert_cmpstr(strfield(row, "productId"), ==, BASIC);
#ifdef LINE_PRIVATE_EMOJI_LABELS
	g_assert_nonnull(strfield(row, "label"));
#else
	g_assert_cmpstr(strfield(row, "label"), ==, "LINE Basic Emoji 001");
#endif
	g_assert_cmpstr(strfield(row, "emojiId"), ==, "001");
	for (int offset = 0; offset < 9175; offset += 128) {
		g_autoptr(J) page =
			line_emoji_catalog_page(&s, offset, 999, NULL, NULL);
		g_assert_cmpuint(json_object_array_length(page), ==,
				 MIN(128, 9175 - offset));
		for (size_t j = 0; j < json_object_array_length(page); j++) {
			J *entry = json_object_array_get_idx(page, j);

			g_assert_true(line_emoji_id_valid(
				strfield(entry, "productId"),
				strfield(entry, "emojiId")));
			g_assert_true(g_utf8_validate(strfield(entry, "label"),
						      -1, NULL));
		}
	}
	g_autoptr(J) empty = line_emoji_catalog_page(&s, 9175, 1, NULL, NULL);
	g_assert_cmpuint(json_object_array_length(empty), ==, 0);
	g_autoptr(J) search = line_emoji_catalog_page(&s, 0, 128, "001", NULL);
	g_assert_cmpstr(strfield(json_object_array_get_idx(search, 0),
				 "emojiId"),
			==, "001");
	g_autoptr(J) last =
		line_emoji_catalog_page(&s, 210, 2, "LINE BASIC EMOJI", NULL);
	g_assert_cmpuint(json_object_array_length(last), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(last, 0), "emojiId"),
			==, "211");
	g_assert_cmpint(rpc_count, ==, 0);
	g_assert_cmpint(manifest_count, ==, 0);
	g_assert_cmpint(builtin_fetches, ==, 0);
	g_ptr_array_unref(s.emoji_packs);
}

static void ownership(void)
{
	rpc_count = builtin_fetches = manifest_count = 0;
	Line s = { .access = "fixture" };

	g_autoptr(J) first = line_emoji_catalog_page(&s, 0, 3, NULL, NULL);
	J *row = json_object_array_get_idx(first, 0);

	g_assert_cmpstr(strfield(row, "productId"), ==, OWN);
	g_assert_cmpstr(strfield(row, "emojiId"), ==, "009");
	g_assert_cmpstr(strfield(row, "resourceType"), ==, "ANIMATION");
	g_assert_cmpint(json_object_get_int(field(row, "version")), ==, 4);
	g_assert_cmpstr(strfield(json_object_array_get_idx(first, 2),
				 "productId"),
			==, BASIC);
	g_autoptr(J) search = line_emoji_catalog_page(&s, 0, 1, "CRÈME", NULL);
	g_assert_cmpstr(strfield(json_object_array_get_idx(search, 0),
				 "productId"),
			==, OWN);
	g_assert_cmpint(rpc_count, ==, 1);
	g_assert_cmpint(manifest_count, ==, 1);
#ifndef LINE_PRIVATE_EMOJI_LABELS
	g_assert_cmpint(builtin_fetches, ==, 1);
	g_autoptr(J) online =
		line_emoji_catalog_page(&s, 0, 8, "FixtuRE sun", NULL);
	g_assert_cmpuint(json_object_array_length(online), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(online, 0), "label"),
			==, "Fixture sunrise");
	g_assert_cmpint(builtin_fetches, ==, 1);
#endif
	g_ptr_array_unref(s.emoji_packs);
}

static void search_starts_offline(void)
{
	Line s = { .access = "fixture" };

	rpc_count = builtin_fetches = manifest_count = 0;
	g_autoptr(J) first =
		line_emoji_catalog_page(&s, 0, 10, "zz-unlisted-keyword", NULL);
	g_autoptr(J) second = line_emoji_catalog_page(
		&s, 0, 10, "zz-unlisted-keyword2", NULL);
	g_assert_cmpuint(json_object_array_length(first), ==, 0);
	g_assert_cmpuint(json_object_array_length(second), ==, 0);
	g_assert_cmpint(rpc_count, ==, 1);
	g_assert_cmpint(builtin_fetches + manifest_count, ==, 0);
	g_ptr_array_unref(s.emoji_packs);
}

static void catalog_manifest(void)
{
	static const char *const bad[] = {
		"{\"productId\":\"" OWN "\",\"orders\":[\"001\"]}{}",
		"{\"productId\":\"" OWN "\\u0000suffix\",\"orders\":[\"001\"]}",
		"{\"productId\":\"" OWN
		"\",\"orders\":[\"001\"],\"altTexts\":{\"001\":\"\xff\"}}",
	};

	manifest_count = 0;
	for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
		Line s = { .access = "fixture" };

		manifest = bad[i];
		g_autoptr(J) page =
			line_emoji_catalog_page(&s, 0, 5, "My Pack", NULL);

		g_assert_cmpuint(json_object_array_length(page), ==, 0);
		g_ptr_array_unref(s.emoji_packs);
	}
	g_assert_cmpuint(manifest_count, ==, G_N_ELEMENTS(bad));
	manifest = NULL;
}

static void shop_fallback(void)
{
	Line s = { .access = "fixture" };

	rpc_count = 0;
	shop_unavailable = TRUE;
	g_autoptr(GError) error = NULL;
	g_autoptr(J) page = line_emoji_catalog_page(&s, 0, 2, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(page), ==, 2);
	g_assert_cmpstr(strfield(json_object_array_get_idx(page, 0),
				 "productId"),
			==, BASIC);
	g_autoptr(J) next = line_emoji_catalog_page(&s, 2, 2, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(json_object_array_get_idx(next, 0), "emojiId"),
			==, "003");
	g_assert_cmpint(rpc_count, ==, 1);
	shop_unavailable = FALSE;
	g_ptr_array_unref(s.emoji_packs);
}

static void images(void)
{
	g_assert_false(line_emoji_id_valid(BASIC, "000"));
	g_assert_false(line_emoji_id_valid(BASIC, "../"));
	g_assert_false(line_emoji_id_valid("670e0cce840a8236ddd4ee4/", "001"));
	g_autoptr(GError) error = NULL;
	g_assert_null(line_emoji_image(BASIC, "001?a", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(downloads, ==, 0);
	g_autoptr(GBytes) anim =
		line_emoji_image_variant(BASIC, "001", TRUE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(anim);
	g_assert_cmpstr(
		last_url, ==,
		"https://stickershop.line-scdn.net/sticonshop/v1/sticon/" BASIC
		"/android/001_animation.png");
	missing = 1;
	g_autoptr(GBytes) fallback =
		line_emoji_image_variant(BASIC, "001", TRUE, &error);
	g_assert_nonnull(fallback);
	g_assert_cmpstr(
		last_url, ==,
		"https://stickershop.line-scdn.net/sticonshop/v1/sticon/" BASIC
		"/android/001.png");
	missing = 2;
	g_assert_null(line_emoji_image_variant(BASIC, "001", TRUE, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpint(downloads, ==, 4);
	g_clear_pointer(&last_url, g_free);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/emoji/catalog/default-and-pages", defaults_only);
	g_test_add_func("/emoji/catalog/owned", ownership);
	g_test_add_func("/emoji/catalog/cold-search", search_starts_offline);
	g_test_add_func("/emoji/catalog/manifest", catalog_manifest);
	g_test_add_func("/emoji/catalog/shop-fallback", shop_fallback);
	g_test_add_func("/emoji/cdn/images", images);
	return g_test_run();
}
