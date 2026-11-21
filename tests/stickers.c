// SPDX-License-Identifier: GPL-3.0-or-later
#include "stickers.h"
#include "obs_http.h"
#include <string.h>

#define SELF "u00000000000000000000000000000001"
#define PEER "u00000000000000000000000000000002"
#define ROOT "https://stickershop.line-scdn.net/stickershop/"

static J *products, *last_send;
static unsigned int requests, sends, downloads, short_page, repeat_page;
static int shop_failure, send_failure, asset_failure;
static gboolean bad_receipt, bad_image;
static char *last_url;

static void reset(void)
{
	g_clear_pointer(&products, json_object_put);
	g_clear_pointer(&last_send, json_object_put);
	g_clear_pointer(&last_url, g_free);
	products = json_object_new_array();
	requests = sends = downloads = 0;
	short_page = 100;
	repeat_page = shop_failure = send_failure = asset_failure = 0;
	bad_receipt = bad_image = FALSE;
}

static J *unpack(const char *method, J *args)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) encoded = line_encode_call(method, args, &error);
	g_assert_no_error(error);
	gsize size, skip = 4 + strlen(method);
	const guint8 *bytes = g_bytes_get_data(encoded, &size);

	g_assert_cmpuint(size, >, skip);
	g_assert_cmpuint(bytes[3], ==, strlen(method));
	J *value = line_decode_struct(bytes + skip, size - skip, &error);

	g_assert_no_error(error);
	return value;
}

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	s->service_code = -1;
	g_autoptr(J) wire = unpack(method, args);
	if (!strcmp(method, "getOwnedProductSummaries")) {
		g_assert_cmpstr(path, ==, "/TSHOP4");
		g_assert_cmpstr(strfield(wire, "2"), ==, "stickershop");
		g_assert_cmpint(json_object_get_int(field(wire, "4")), ==, 100);
		g_assert_cmpuint(strlen(strfield(field(wire, "5"), "1")), ==,
				 2);
		g_assert_cmpuint(strlen(strfield(field(wire, "5"), "2")), ==,
				 2);
		requests++;
		if (shop_failure) {
			s->service_code = shop_failure;
			g_set_error_literal(
				error, LINE_ERROR, 1,
				"Shop service refused the request.");
			return NULL;
		}
		unsigned int offset =
			repeat_page ? 0 : json_object_get_int(field(wire, "3"));
		unsigned int total = json_object_array_length(products);
		J *page = obj(), *list = json_object_new_array();

		for (unsigned int i = offset;
		     i < total && i < offset + short_page; i++)
			json_object_array_add(
				list, json_object_get(json_object_array_get_idx(
					      products, i)));
		json_object_object_add(page, "1", list);
		json_object_object_add(page, "3", json_object_new_int(total));
		return page;
	}
	g_assert_cmpstr(method, ==, "sendMessage");
	g_assert_cmpstr(path, ==, "/S4");
	sends++;
	g_clear_pointer(&last_send, json_object_put);
	last_send = g_steal_pointer(&wire);
	if (send_failure) {
		s->service_code = send_failure;
		g_set_error_literal(error, LINE_ERROR, 1, "Message declined.");
		return NULL;
	}
	return json_tokener_parse(bad_receipt ?
					  "{\"4\":\"123x\"}" :
					  "{\"4\":\"9223372036854775807\"}");
}

GBytes *line_public_asset(const char *url, gsize limit, GError **error)
{
	g_assert_cmpuint(limit, <=, 16 * 1024 * 1024);
	downloads++;
	g_free(last_url);
	last_url = g_strdup(url);
	if (asset_failure) {
		g_set_error_literal(error, LINE_ERROR, asset_failure,
				    "Fixture download failed.");
		asset_failure = 0;
		return NULL;
	}
	if (bad_image)
		return g_bytes_new_static("not a png", 9);
	gsize len;
	guint8 *data = g_base64_decode(
		"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9Zl1sAAAAASUVORK5CYII=",
		&len);
	return g_bytes_new_take(data, len);
}

static J *pack(const char *id, const char *name, int kind, gint64 until,
	       const char *ranges, const char *hash)
{
	J *p = obj(), *summary = obj(), *u = obj();

	json_object_object_add(p, "1", json_object_new_string(id));
	json_object_object_add(p, "11", json_object_new_string(name));
	json_object_object_add(p, "21", json_object_new_int(7));
	json_object_object_add(p, "94", json_object_new_int64(until));
	json_object_object_add(p, "93", u);
	json_object_object_add(u, "1", summary);
	json_object_object_add(summary, "5", json_object_new_int(kind));
	json_object_object_add(summary, "1", json_tokener_parse(ranges));
	if (hash)
		json_object_object_add(summary, "3",
				       json_object_new_string(hash));
	json_object_array_add(products, p);
	return p;
}

#define RANGE "[{\"1\":1234,\"2\":3}]"

static void pages(void)
{
	reset();
	Line s = { .access = "token", .mid = SELF };

	g_autoptr(LineStickers) c = line_stickers_new();
	short_page = 1;
	pack("12", "Expired", 1, 1, RANGE, NULL);
	pack("13", "Expired default", 1, 0, RANGE, NULL);
	pack("14", "Customized", 8, -1, RANGE, NULL);
	pack("15", "Bad hash", 1, -1, RANGE, "../sprite");
	pack("16", "Overflow", 1, -1, "[{\"1\":9223372036854775807,\"2\":2}]",
	     NULL);
	pack("17", "Overlap", 1, -1,
	     "[{\"1\":1234,\"2\":3},{\"1\":1236,\"2\":1}]", NULL);
	pack("18", "Huge", 1, -1, "[{\"1\":1,\"2\":1001}]", NULL);
	pack("19", "Bär & Friends", 1, -1,
	     "[{\"1\":9223372036854775807,\"2\":1},{\"1\":9021,\"2\":3}]",
	     NULL);
	pack("20", "Beach", 2, g_get_real_time() / 1000 + 60000, RANGE,
	     "Y-29_0");
	g_autoptr(GError) e = NULL;
	g_autoptr(J) page = line_sticker_catalog(&s, c, 2, 2, " BÄR ", &e);
	g_assert_no_error(e);
	g_assert_cmpuint(requests, ==, 9);
	g_assert_cmpuint(json_object_array_length(page), ==, 2);
	J *row = json_object_array_get_idx(page, 1);

	g_assert_cmpstr(strfield(row, "stickerId"), ==, "9223372036854775807");
	g_assert_cmpstr(strfield(row, "packageId"), ==, "19");
	g_assert_cmpint(json_object_get_int(field(row, "version")), ==, 7);
	g_assert_cmpstr(strfield(row, "resourceType"), ==, "STATIC");
	g_autoptr(J) exact = line_sticker_catalog(&s, c, 0, 128, "1235", &e);
	g_assert_cmpuint(json_object_array_length(exact), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(exact, 0), "pack"),
			==, "Beach");
	g_autoptr(J) all = line_sticker_catalog(&s, c, 0, 128, NULL, &e);
	g_assert_cmpuint(json_object_array_length(all), ==, 7);
	g_autofree char *asset = line_sticker_asset(c, "20", "1234", &e);

	g_assert_cmpstr(asset, ==,
			ROOT
			"v2/sticker/1234/Y-29_0/android/sticker_animation.png");
	g_assert_cmpuint(requests, ==, 9);
	g_assert_cmpuint(sends, ==, 0);
	s.mid = PEER;
	shop_failure = 7;
	g_assert_null(line_sticker_catalog(&s, c, 0, 10, NULL, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_cmpint(s.service_code, ==, -1);
	g_clear_error(&e);
	g_assert_null(line_sticker_asset(c, "20", "1234", &e));
	g_assert_error(e, LINE_ERROR, 1);
}

static void inconsistent(void)
{
	reset();
	Line s = { .access = "token", .mid = SELF };

	g_autoptr(LineStickers) c = line_stickers_new();
	g_autoptr(GError) e = NULL;
	pack("12", "Bears", 1, -1, RANGE, NULL);
	pack("13", "Bunnies", 1, -1, RANGE, NULL);
	short_page = repeat_page = 1;
	g_assert_null(line_sticker_catalog(&s, c, 0, 10, NULL, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_cmpuint(requests, ==, 2);
	g_clear_error(&e);
	g_assert_null(line_sticker_asset(c, "12", "1235", &e));
	g_clear_error(&e);
	repeat_page = 0;
	g_autoptr(J) rows = line_sticker_catalog(&s, c, 0, 10, NULL, &e);
	g_assert_no_error(e);
	g_assert_cmpuint(json_object_array_length(rows), ==, 6);
}

static void send_options(void)
{
	reset();
	Line s = { .access = "token", .mid = SELF, .seq = 8 };

	g_autoptr(LineStickers) c = line_stickers_new();
	const char *const opts[] = { NULL, "A", "S", "AS", "P", "PS" };
	const char *const files[] = {
		"sticker.png",	     "sticker_animation.png",
		"sticker.png",	     "sticker_animation.png",
		"sticker_popup.png", "sticker_popup.png"
	};
	for (unsigned int i = 0; i < G_N_ELEMENTS(opts); i++) {
		g_autofree char *id = g_strdup_printf("%u", 10 + i);

		pack(id, "Bunnies", 1 + i, -1, RANGE, i ? "Ar0_2-Z" : NULL);
	}
	g_autoptr(GError) e = NULL;
	for (unsigned int i = 0; i < G_N_ELEMENTS(opts); i++) {
		g_autofree char *id = g_strdup_printf("%u", 10 + i);
		const char *quote = i ? NULL : "9007199254740993";

		g_autoptr(J) ack = line_send_sticker(&s, c, PEER, id, "1236",
						     quote, 91 + i, &e);
		g_assert_no_error(e);
		g_assert_cmpstr(strfield(ack, "id"), ==, "9223372036854775807");
		g_assert_cmpint(json_object_get_int(field(last_send, "1")), ==,
				91 + (int)i);
		J *msg = field(last_send, "2"), *meta = field(msg, "18");

		g_assert_cmpstr(strfield(msg, "2"), ==, PEER);
		g_assert_cmpint(json_object_get_int(field(msg, "15")), ==, 7);
		g_assert_null(field(msg, "10"));
		g_assert_null(field(msg, "20"));
		g_assert_cmpstr(strfield(meta, "STKID"), ==, "1236");
		g_assert_cmpstr(strfield(meta, "STKPKGID"), ==, id);
		g_assert_cmpstr(strfield(meta, "STKVER"), ==, "7");
		g_assert_cmpstr(strfield(meta, "STKTXT"), ==, "Bunnies");
		g_assert_cmpstr(strfield(meta, "STKOPT"), ==, opts[i]);
		g_assert_cmpstr(strfield(meta, "STKHASH"), ==,
				i ? "Ar0_2-Z" : NULL);
		g_assert_cmpstr(strfield(msg, "21"), ==, quote);
		if (quote) {
			g_assert_cmpint(json_object_get_int(field(msg, "22")),
					==, 3);
			g_assert_cmpint(json_object_get_int(field(msg, "24")),
					==, 1);
		}
		g_autofree char *url = line_sticker_message_asset(msg, &e);
		g_autofree char *expected = g_strconcat(
			i ? ROOT "v2/sticker/1236/Ar0_2-Z/android/" :
			    ROOT "v1/sticker/1236/android/",
			files[i], NULL);
		g_assert_cmpstr(url, ==, expected);
	}
	g_assert_cmpuint(sends, ==, 6);
	g_assert_cmpuint(requests, ==, 1);
	g_assert_cmpint(s.seq, ==, 8);
}

static void ownership(void)
{
	reset();
	Line s = { .access = "token", .mid = SELF };

	g_autoptr(LineStickers) c = line_stickers_new();
	g_autoptr(GError) e = NULL;
	pack("12", "Bears", 1, -1, RANGE, NULL);
	pack("13", "Expired", 1, 1, RANGE, NULL);
	const char *invalid[][2] = {
		{ "999", "1235" }, { "12", "1233" },
		{ "13", "1235" },  { "12", "01235" },
		{ "12", "1/23" },  { "12", "9223372036854775808" }
	};
	for (unsigned int i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_assert_null(line_send_sticker(&s, c, PEER, invalid[i][0],
						invalid[i][1], NULL, 1, &e));
		g_assert_error(e, LINE_ERROR, 1);
		g_clear_error(&e);
	}
	g_assert_null(line_send_sticker(&s, c, "not-a-chat", "12", "1234", NULL,
					1, &e));
	g_clear_error(&e);
	g_assert_null(
		line_send_sticker(&s, c, PEER, "12", "1234", "bad", 1, &e));
	g_clear_error(&e);
	g_assert_null(
		line_send_sticker(&s, c, PEER, "12", "1234", NULL, 0, &e));
	g_clear_error(&e);
	g_assert_cmpuint(sends, ==, 0);
	send_failure = 32;
	g_assert_null(
		line_send_sticker(&s, c, PEER, "12", "1234", NULL, 2, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_cmpint(s.service_code, ==, 32);
	g_clear_error(&e);
	json_object_array_del_idx(products, 0, 1);
	send_failure = 0;
	g_assert_null(
		line_send_sticker(&s, c, PEER, "12", "1234", NULL, 3, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_cmpuint(sends, ==, 1);
	g_assert_cmpuint(requests, ==, 2);
}

static void ambiguous_send(void)
{
	reset();
	Line s = { .access = "token", .mid = SELF };

	g_autoptr(LineStickers) c = line_stickers_new();
	g_autoptr(GError) e = NULL;
	pack("12", "Bears", 1, -1, RANGE, NULL);
	bad_receipt = TRUE;
	g_assert_null(
		line_send_sticker(&s, c, PEER, "12", "1234", NULL, 1, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_assert_cmpuint(sends, ==, 1);
}

static void receive(void)
{
	reset();
	g_autoptr(GError) e = NULL;
	g_autoptr(J) raw = json_tokener_parse(
		"{\"15\":7,\"18\":{\"STKID\":\"5\",\"STKHASH\":\"h\",\"STKOPT\":\"P\"}}");
	g_autofree char *url = line_sticker_message_asset(raw, &e);

	g_assert_no_error(e);
	asset_failure = LINE_ASSET_NOT_FOUND;
	g_autoptr(GBytes) image = line_sticker_fetch(url, &e);
	g_assert_no_error(e);
	g_assert_nonnull(image);
	g_assert_cmpuint(downloads, ==, 2);
	g_assert_cmpstr(last_url, ==,
			ROOT "v2/sticker/5/h/android/sticker.png");
	asset_failure = LINE_TIMEOUT;
	g_assert_null(line_sticker_fetch(url, &e));
	g_assert_error(e, LINE_ERROR, LINE_TIMEOUT);
	g_clear_error(&e);
	g_assert_cmpuint(downloads, ==, 3);
	bad_image = TRUE;
	g_assert_null(line_sticker_fetch(url, &e));
	g_assert_error(e, LINE_ERROR, 1);
	g_clear_error(&e);
	json_object_object_add(field(raw, "18"), "STKHASH",
			       json_object_new_string_len("x\0h", 3));
	g_assert_null(line_sticker_message_asset(raw, &e));
	g_clear_error(&e);
	const char *const invalid[] = {
		ROOT "v2/sticker/5/../android/sticker.png",
		ROOT "v1/sticker/5/android/evil.png",
		ROOT "v1/sticker/5/android/sticker.png?token=x",
		"https://example.org/sticker.png",
		ROOT "v2/sticker/5/a%2fb/android/sticker.png"
	};
	for (unsigned int i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_assert_null(line_sticker_fetch(invalid[i], &e));
		g_assert_error(e, LINE_ERROR, 1);
		g_clear_error(&e);
	}
	g_assert_cmpuint(downloads, ==, 4);
	g_assert_cmpuint(requests, ==, 0);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/stickers/catalog", pages);
	g_test_add_func("/stickers/catalog-transaction", inconsistent);
	g_test_add_func("/stickers/send-options", send_options);
	g_test_add_func("/stickers/ownership", ownership);
	g_test_add_func("/stickers/ambiguous-send", ambiguous_send);
	g_test_add_func("/stickers/received-artwork", receive);
	int result = g_test_run();

	g_clear_pointer(&products, json_object_put);
	g_clear_pointer(&last_send, json_object_put);
	g_free(last_url);
	return result;
}
