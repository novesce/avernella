// SPDX-License-Identifier: GPL-3.0-or-later
#include "line.h"
#include <string.h>

static void compact(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(J) args = obj();
	J *r = obj();

	json_object_object_add(args, "1:obj", r);
	json_object_object_add(r, "1:str", json_object_new_string("ab"));
	json_object_object_add(r, "3:i32", json_object_new_int(-7));
	json_object_object_add(r, "4:bool", json_object_new_boolean(FALSE));
	g_autoptr(GBytes) packet = line_encode_call("test", args, &error);
	g_assert_no_error(error);
	const guint8 expected[] = { 0x82, 0x21, 0,    4,    't', 'e',
				    's',  't',	0x1c, 0x18, 2,	 'a',
				    'b',  0x25, 13,   0x12, 0,	 0 };
	g_assert_cmpmem(g_bytes_get_data(packet, NULL),
			g_bytes_get_size(packet), expected, sizeof(expected));
	g_autoptr(J) decoded =
		line_decode_struct(expected + 8, sizeof(expected) - 8, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int(field(field(decoded, "1"), "3")),
			==, -7);
}

static void replies(void)
{
	const guint8 reply[] = { 0x82, 0x41, 0,	  1, 'm', 0x0c, 0,
				 0x18, 3,    'A', 0, 'B', 0,	0 };
	int code = -1;

	g_autoptr(GError) error = NULL;
	g_autoptr(J) ok =
		line_decode_reply("m", reply, sizeof(reply), &code, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_string_len(field(ok, "1")), ==, 3);
	{
		guint8 sequenced[G_N_ELEMENTS(reply)];

		memcpy(sequenced, reply, sizeof(reply));
		sequenced[2] = 7;
		g_autoptr(J) accepted = line_decode_reply(
			"m", sequenced, sizeof(sequenced), &code, &error);
		g_assert_no_error(error);
		g_assert_cmpint(
			json_object_get_string_len(field(accepted, "1")), ==,
			3);
	}
	for (size_t n = 0; n < sizeof(reply); n++) {
		g_assert_null(line_decode_reply("m", reply, n, &code, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	const guint8 denied[] = { 0x82, 0x41, 0, 1, 'm', 0x1c, 0x15, 16, 0, 0 };

	g_assert_null(
		line_decode_reply("m", denied, sizeof(denied), &code, &error));
	g_assert_cmpint(code, ==, 8);
}

static void auth(void)
{
	const guint8 request[] = { 0x80, 1, 0, 1, 0, 0, 0, 1, 'm', 0, 0, 0, 0,
				   12,	 0, 1, 8, 0, 2, 0, 0, 0,   0, 0, 0 };
	const guint8 response[] = { 0x80, 1, 0, 2,   0, 0,   0,	 1, 'm', 0,
				    0,	  0, 0, 12,  0, 0,   11, 0, 1,	 0,
				    0,	  0, 3, 'A', 0, 'B', 10, 0, 3,	 0,
				    0,	  0, 0, 0,   0, 0,   60, 0, 0 };
	g_autoptr(J) args = obj(), result = NULL;
	g_autoptr(GError) error = NULL;
	J *payload = obj();
	int code = -1;

	json_object_object_add(args, "1:obj", payload);
	json_object_object_add(payload, "2:i32", json_object_new_int(0));
	g_autoptr(GBytes) packet = line_encode_auth("m", args, &error);

	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(packet, NULL),
			g_bytes_get_size(packet), request, sizeof(request));
	result = line_decode_auth("m", response, sizeof(response), &code,
				  &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_string_len(field(result, "1")), ==, 3);
	g_assert_cmpint(json_object_get_int64(field(result, "3")), ==, 60);
	for (size_t n = 0; n < sizeof(response); n++) {
		g_assert_null(
			line_decode_auth("m", response, n, &code, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
}

static void invalid_fields(void)
{
	static const guint8 invalid[][7] = {
		/* A duplicate field 1 and an i16 that decodes to 32768. */
		{ 0x15, 0x02, 0x05, 0x02, 0x04, 0 },
		{ 0x14, 0x80, 0x80, 0x04, 0 },
		/* An i32 that decodes to 2^31. */
		{ 0x15, 0x80, 0x80, 0x80, 0x80, 0x10, 0 },
	};
	static const size_t sizes[] = { 6, 5, 7 };

	for (size_t i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_autoptr(GError) error = NULL;

		g_assert_null(line_decode_struct(invalid[i], sizes[i], &error));
		g_assert_error(error, LINE_ERROR, 1);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/thrift/compact", compact);
	g_test_add_func("/thrift/reply", replies);
	g_test_add_func("/thrift/auth", auth);
	g_test_add_func("/thrift/invalid-fields", invalid_fields);
	return g_test_run();
}
