// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The Thrift readers in src/thrift.c: Compact structures, Compact replies
 * with their method name, sequence and exception results, and Binary
 * protocol replies from the authentication service. A decoded result must
 * serialize as JSON, which walks every value json-c was given.
 *
 * Input: a selector byte, then the message. Replies answer method "m",
 * the name the fixtures in tests/thrift.c use.
 */
#include "fuzz.h"
#include "line.h"
#include <string.h>

enum {
	SELECT_STRUCT,
	SELECT_REPLY,
	SELECT_AUTH,
	SELECTS
};

static void check(J *result, GError *error)
{
	if (!result) {
		g_assert_nonnull(error);
		g_assert_nonnull(error->message);
		return;
	}
	g_assert_null(error);
	fuzz_touch((const guint8 *)json_object_to_json_string(result),
		   strlen(json_object_to_json_string(result)));
	json_object_put(result);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	fuzz_quiet();
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autoptr(GError) error = NULL;
	struct fuzz_input in = { data, size };
	guint select = fuzz_byte(&in) % SELECTS;
	int code = -1;
	J *result;

	switch (select) {
	case SELECT_STRUCT:
		result = line_decode_struct(in.p, in.n, &error);
		g_assert_true(!result ||
			      json_object_is_type(result, json_type_object));
		break;
	case SELECT_REPLY:
		result = line_decode_reply("m", in.p, in.n, &code, &error);
		break;
	default:
		result = line_decode_auth("m", in.p, in.n, &code, &error);
		break;
	}
	check(result, error);
	if (line_service_exception())
		json_object_to_json_string(line_service_exception());
	return 0;
}
