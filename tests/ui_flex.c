// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_flex.h"
#include <string.h>

static const char coupon[] =
	"{\"type\":\"flex\",\"altText\":\"Coupon\",\"contents\":{"
	"\"type\":\"bubble\",\"size\":\"kilo\","
	"\"hero\":{\"type\":\"image\",\"url\":\"https://static.line-scdn.net/test.png\","
	"\"size\":\"full\",\"aspectRatio\":\"2:1\",\"aspectMode\":\"cover\"},"
	"\"body\":{\"type\":\"box\",\"layout\":\"vertical\",\"paddingAll\":\"12px\","
	"\"contents\":[{\"type\":\"text\",\"wrap\":true,\"size\":\"lg\",\"weight\":\"bold\","
	"\"contents\":[{\"type\":\"span\",\"text\":\"คูปอง\",\"color\":\"#333333\"},"
	"{\"type\":\"span\",\"text\":\" たのしい\",\"size\":\"sm\"}]},"
	"{\"type\":\"separator\",\"margin\":\"md\"},"
	"{\"type\":\"box\",\"layout\":\"horizontal\",\"spacing\":\"sm\",\"margin\":\"sm\","
	"\"contents\":[{\"type\":\"text\",\"text\":\"Expiry\",\"flex\":0},"
	"{\"type\":\"text\",\"text\":\"16 November\",\"align\":\"end\"}]}]},"
	"\"footer\":{\"type\":\"box\",\"layout\":\"vertical\",\"contents\":["
	"{\"type\":\"button\",\"style\":\"primary\",\"color\":\"#843acc\","
	"\"action\":{\"type\":\"uri\",\"label\":\"Coupon details\",\"uri\":\"https://example.org/coupon\"}},"
	"{\"type\":\"button\",\"action\":{\"type\":\"message\",\"label\":\"Send message\","
	"\"text\":\"example\"}}]}}}";

static const char simple[] =
	"{\"type\":\"bubble\",\"body\":{"
	"\"type\":\"box\",\"layout\":\"vertical\","
	"\"contents\":[{\"type\":\"text\",\"text\":\"One\"}]}}";

static gboolean has_link(const struct ui_flex_layout *layout, const char *uri)
{
	for (int y = 0; y < ui_flex_height(layout); y += 2)
		for (int x = 0; x < ui_flex_width(layout); x += 2) {
			const char *hit = ui_flex_uri_at(layout, x, y);

			if (hit && (!uri || !strcmp(uri, hit)))
				return TRUE;
		}
	return FALSE;
}

struct asset {
	cairo_surface_t *surface;
	int requests;
};

static cairo_surface_t *image(const char *url, gpointer data)
{
	struct asset *source = data;

	g_assert_cmpstr(url, ==, "https://static.line-scdn.net/test.png");
	source->requests++;
	return source->surface;
}

static guint32 pixel(cairo_surface_t *surface, int x, int y)
{
	cairo_surface_flush(surface);
	const unsigned char *p = cairo_image_surface_get_data(surface);
	guint32 value;

	memcpy(&value,
	       p + cairo_image_surface_get_stride(surface) * y +
		       (int)sizeof(value) * x,
	       sizeof(value));
	return value;
}

static void render(void)
{
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 400);
	cairo_surface_t *asset =
		cairo_image_surface_create(CAIRO_FORMAT_RGB24, 8, 4);
	cairo_t *ac = cairo_create(asset);

	cairo_set_source_rgb(ac, .2, .6, 1);
	cairo_paint(ac);
	cairo_destroy(ac);

	cairo_t *cr = cairo_create(surface);

	PangoContext *context = pango_cairo_create_context(cr);
	Json *json = json_tokener_parse(coupon);
	struct ui_flex_layout *layout = ui_flex_new(context, json, 240);
	struct asset source = { .surface = asset };

	json_object_put(json);
	g_assert_nonnull(layout);
	g_assert_cmpint(ui_flex_width(layout), ==, 240);
	g_assert_cmpint(ui_flex_height(layout), >, 260);
	g_assert_null(ui_flex_uri_at(layout, 15, 15));
	g_assert_true(has_link(layout, "https://example.org/coupon"));
	ui_flex_draw(cr, layout, 5, 8, image, &source);
	g_assert_cmpint(source.requests, ==, 1);
	g_assert_cmphex(pixel(surface, 100, 40), ==, 0xff3399ff);
	g_assert_cmphex(pixel(surface, 20, 130), ==, 0xffffffff);
	g_assert_cmphex(pixel(surface, 5, 8), ==, 0);
	for (int y = 0; y < ui_flex_height(layout); y++) {
		const char *hit = ui_flex_uri_at(layout, 20, y);

		if (hit) {
			g_assert_cmpstr(hit, ==, "https://example.org/coupon");
			g_assert_cmphex(pixel(surface, 25, 8 + y + 8), ==,
					0xff843acc);
			gboolean white_label = FALSE;

			for (int row = y + 10; row < y + 32; row++)
				for (int col = 58; col < 182; col++)
					white_label |= pixel(surface, 5 + col,
							     8 + row) ==
						       0xffffffff;
			g_assert_true(white_label);
			break;
		}
	}
	ui_flex_free(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(asset);
	cairo_surface_destroy(surface);
	g_object_unref(context);
}

static void carousel(void)
{
	PangoContext *context = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	g_autofree char *text = g_strdup_printf(
		"{\"type\":\"carousel\",\"contents\":[%s,%s]}", simple, simple);
	Json *json = json_tokener_parse(text);
	struct ui_flex_layout *wide = ui_flex_new(context, json, 700);
	struct ui_flex_layout *narrow = ui_flex_new(context, json, 290);

	json_object_put(json);
	g_assert_nonnull(wide);
	g_assert_nonnull(narrow);
	g_assert_cmpint(ui_flex_width(wide), >, 600);
	g_assert_cmpint(ui_flex_width(narrow), ==, 290);
	g_assert_cmpint(ui_flex_height(narrow), >, 2 * ui_flex_height(wide));
	ui_flex_free(narrow);
	ui_flex_free(wide);
	g_object_unref(context);
}

static void fallback(void)
{
	PangoContext *context = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	const char *const inputs[] = {
		"{\"type\":\"bubble\",\"body\":{\"type\":\"box\",\"contents\":[{}]}}",
		"{\"type\":\"carousel\",\"contents\":[3,-1]}",
		"{\"type\":\"flex\",\"contents\":\"bubble\"}",
		"{\"type\":\"bubble\",\"hero\":{\"type\":\"image\","
		"\"url\":\"file:///tmp/a.png\"}}",
		"false",
	};

	for (guint i = 0; i < G_N_ELEMENTS(inputs); i++) {
		Json *json = json_tokener_parse(inputs[i]);

		g_assert_null(ui_flex_new(context, json, 280));
		json_object_put(json);
	}
	g_object_unref(context);
}

static void actions(void)
{
	PangoContext *context = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	static const char *const uris[] = { "http://example.org/",
					    "https://name@example.org/",
					    "https://", "line://profile",
					    "javascript:example()" };

	for (guint i = 0; i < G_N_ELEMENTS(uris); i++) {
		g_autofree char *text = g_strdup_printf(
			"{\"type\":\"bubble\",\"body\":{\"type\":\"box\",\"contents\":["
			"{\"type\":\"button\",\"action\":{\"type\":\"uri\","
			"\"label\":\"Action\",\"uri\":\"%s\"}}]}}",
			uris[i]);
		Json *json = json_tokener_parse(text);
		struct ui_flex_layout *layout = ui_flex_new(context, json, 240);

		json_object_put(json);
		g_assert_nonnull(layout);
		g_assert_false(has_link(layout, NULL));
		ui_flex_free(layout);
	}
	g_object_unref(context);
}

/* LIFF apps open from line://app/ buttons; other line:// links do not. */
static void liff_links(void)
{
	static const char card[] =
		"{\"type\":\"bubble\",\"body\":{\"type\":\"box\","
		"\"layout\":\"vertical\",\"contents\":["
		"{\"type\":\"button\",\"action\":{\"type\":\"uri\","
		"\"label\":\"Coupon\",\"uri\":\"line://app/1661444788-rnvmoOo8\"}},"
		"{\"type\":\"button\",\"action\":{\"type\":\"uri\","
		"\"label\":\"Add\",\"uri\":\"line://ti/p/@ramen\"}}]}}";
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 10, 10);
	cairo_t *cr = cairo_create(surface);

	PangoContext *context = pango_cairo_create_context(cr);
	Json *json = json_tokener_parse(card);
	struct ui_flex_layout *layout = ui_flex_new(context, json, 240);

	json_object_put(json);
	g_assert_nonnull(layout);
	g_assert_true(has_link(layout, "line://app/1661444788-rnvmoOo8"));
	g_assert_false(has_link(layout, "line://ti/p/@ramen"));
	ui_flex_free(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	g_object_unref(context);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ui-flex/bubble", render);
	g_test_add_func("/ui-flex/carousel", carousel);
	g_test_add_func("/ui-flex/fallback", fallback);
	g_test_add_func("/ui-flex/actions", actions);
	g_test_add_func("/ui-flex/liff-links", liff_links);
	return g_test_run();
}
