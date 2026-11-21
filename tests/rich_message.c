// SPDX-License-Identifier: GPL-3.0-or-later
#include "rich_message.h"
#include <string.h>

/* A two-scene official-account rich message, as LINE delivers it. */
#define MARKUP                                                                       \
	"{\"canvas\":{\"width\":1040,\"height\":1040,\"initialScene\":\"scene1\"},"  \
	"\"images\":{\"image1\":{\"x\":0,\"y\":0,\"w\":1040,\"h\":1040},"            \
	"\"logo\":{\"x\":0,\"y\":0,\"w\":10,\"h\":10,"                               \
	"\"imageUrl\":\"https://obs.line-scdn.net/logo\"}},"                         \
	"\"actions\":{\"open\":{\"type\":\"web\",\"text\":\"Shop\","                 \
	"\"params\":{\"linkUri\":\"https://example.com/shop\"}},"                    \
	"\"say\":{\"type\":\"sendMessage\",\"params\":{\"text\":\"Coupon\"}},"       \
	"\"copy\":{\"type\":\"clipboard\",\"params\":{\"clipboardText\":\"C0DE\"}}," \
	"\"next\":{\"type\":\"transition\",\"params\":{\"nextScene\":\"scene2\"}},"  \
	"\"app\":{\"type\":\"web\",\"params\":{\"linkUri\":\"line://app/1\"}},"      \
	"\"liff\":{\"type\":\"web\",\"params\":{\"linkUri\":"                        \
	"\"line://app/1661444788-rnvmoOo8/coupon\"}},"                               \
	"\"bad\":{\"type\":\"transition\",\"params\":{\"nextScene\":\"nope\"}}},"    \
	"\"scenes\":{\"scene1\":{\"draws\":[{\"image\":\"image1\",\"x\":0,\"y\":0,"  \
	"\"w\":1040,\"h\":1040}],\"listeners\":["                                    \
	"{\"type\":\"touch\",\"params\":[0,0,520,520],\"action\":\"open\"},"         \
	"{\"type\":\"touch\",\"params\":[520,0,520,520],\"action\":\"say\"},"        \
	"{\"type\":\"touch\",\"params\":[0,520,520,520],\"action\":\"copy\"},"       \
	"{\"type\":\"touch\",\"params\":[520,520,520,520],\"action\":\"next\"},"     \
	"{\"type\":\"touch\",\"params\":[0,0,10,10],\"action\":\"app\"},"            \
	"{\"type\":\"touch\",\"params\":[0,0,20,20],\"action\":\"liff\"},"           \
	"{\"type\":\"touch\",\"params\":[0,0,10,10],\"action\":\"bad\"},"            \
	"{\"type\":\"touch\",\"params\":[1000,1000,100,100],\"action\":\"open\"}]}," \
	"\"scene2\":{\"draws\":[{\"image\":\"logo\",\"x\":10,\"y\":10,\"w\":100,"    \
	"\"h\":100}]}}}"

static J *metadata(const char *markup, const char *base)
{
	J *m = obj();

	json_object_object_add(m, "MARKUP_JSON",
			       json_object_new_string(markup));
	json_object_object_add(m, "ALT_TEXT",
			       json_object_new_string("Spring sale"));
	if (base)
		json_object_object_add(m, "DOWNLOAD_URL",
				       json_object_new_string(base));
	return m;
}

static J *area(J *rich, const char *scene, guint i)
{
	return json_object_array_get_idx(
		field(field(field(rich, "scenes"), scene), "areas"), i);
}

static void parse(void)
{
	g_autoptr(J) meta = metadata(MARKUP, "https://obs.line-scdn.net/r/1");
	g_autofree char *alt = NULL;

	g_autoptr(J) rich = line_rich_parse(meta, &alt);
	g_assert_nonnull(rich);
	g_assert_cmpstr(alt, ==, "Spring sale");
	g_assert_cmpint(json_object_get_int(field(rich, "w")), ==, 1040);
	g_assert_cmpstr(strfield(rich, "scene"), ==, "scene1");
	J *draw = json_object_array_get_idx(
		field(field(field(rich, "scenes"), "scene1"), "draws"), 0);

	g_assert_cmpstr(strfield(draw, "url"), ==,
			"https://obs.line-scdn.net/r/1/700");
	g_assert_cmpint(json_object_get_int(field(draw, "sw")), ==, 1040);
	/* Unsupported, dangling and off-canvas areas are dropped. */
	J *areas = field(field(field(rich, "scenes"), "scene1"), "areas");

	g_assert_cmpuint(json_object_array_length(areas), ==, 5);
	g_assert_cmpstr(strfield(area(rich, "scene1", 0), "kind"), ==, "link");
	g_assert_cmpstr(strfield(area(rich, "scene1", 0), "value"), ==,
			"https://example.com/shop");
	g_assert_cmpstr(strfield(area(rich, "scene1", 0), "label"), ==, "Shop");
	g_assert_cmpstr(strfield(area(rich, "scene1", 1), "kind"), ==,
			"message");
	g_assert_cmpstr(strfield(area(rich, "scene1", 1), "value"), ==,
			"Coupon");
	g_assert_cmpstr(strfield(area(rich, "scene1", 2), "kind"), ==, "copy");
	g_assert_cmpstr(strfield(area(rich, "scene1", 3), "kind"), ==, "scene");
	g_assert_cmpstr(strfield(area(rich, "scene1", 3), "value"), ==,
			"scene2");
	/* LIFF apps open from line://app/ links, other line:// links do not. */
	g_assert_cmpstr(strfield(area(rich, "scene1", 4), "kind"), ==, "link");
	g_assert_cmpstr(strfield(area(rich, "scene1", 4), "value"), ==,
			"line://app/1661444788-rnvmoOo8/coupon");
	J *logo = json_object_array_get_idx(
		field(field(field(rich, "scenes"), "scene2"), "draws"), 0);

	g_assert_cmpstr(strfield(logo, "url"), ==,
			"https://obs.line-scdn.net/logo");
	g_assert_true(json_object_get_boolean(field(logo, "whole")));
}

static void reject(void)
{
	const char *const markups[] = {
		"{}",
		"[]",
		"{\"canvas\":{\"width\":0,\"height\":10,\"initialScene\":\"a\"},"
		"\"images\":{},\"scenes\":{\"a\":{\"draws\":[]}}}",
		/* The initial scene must exist. */
		"{\"canvas\":{\"width\":10,\"height\":10,\"initialScene\":\"b\"},"
		"\"images\":{},\"scenes\":{\"a\":{\"draws\":[]}}}",
		/* A draw outside the canvas. */
		"{\"canvas\":{\"width\":10,\"height\":10,\"initialScene\":\"a\"},"
		"\"images\":{\"i\":{\"x\":0,\"y\":0,\"w\":10,\"h\":10}},"
		"\"scenes\":{\"a\":{\"draws\":[{\"image\":\"i\",\"x\":5,\"y\":0,"
		"\"w\":10,\"h\":10}]}}}",
		/* A draw of an unknown image. */
		"{\"canvas\":{\"width\":10,\"height\":10,\"initialScene\":\"a\"},"
		"\"images\":{},\"scenes\":{\"a\":{\"draws\":[{\"image\":\"i\","
		"\"x\":0,\"y\":0,\"w\":10,\"h\":10}]}}}",
		"not json",
	};
	for (guint i = 0; i < G_N_ELEMENTS(markups); i++) {
		g_autoptr(J) meta =
			metadata(markups[i], "https://obs.line-scdn.net/r/1");
		g_autofree char *alt = NULL;

		g_autoptr(J) rich = line_rich_parse(meta, &alt);
		g_assert_null(rich);
		/* The alternative text still describes the message. */
		g_assert_cmpstr(alt, ==, "Spring sale");
	}
	/* Base images need an https DOWNLOAD_URL without a query. */
	const char *const bases[] = { NULL, "http://obs.line-scdn.net/r/1",
				      "https://obs.line-scdn.net/r?1",
				      "https:///r/1" };
	for (guint i = 0; i < G_N_ELEMENTS(bases); i++) {
		g_autoptr(J) meta = metadata(MARKUP, bases[i]);
		g_autoptr(J) rich = line_rich_parse(meta, NULL);

		g_assert_null(rich);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/rich-message/scenes-and-actions", parse);
	g_test_add_func("/rich-message/rejects-malformed-markup", reject);
	return g_test_run();
}
