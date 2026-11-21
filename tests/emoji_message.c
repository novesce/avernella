// SPDX-License-Identifier: GPL-3.0-or-later
#include "messaging.h"
#include "sealing.h"
#include <openssl/evp.h>
#include <string.h>

#define ALICE "uaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BOB "ubbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PACK "670e0cce840a8236ddd4ee4c"
#define CAPTION "😀 (Brown) (Cony)"
static guint sent_count;
static J *message;

static Line account(char *mid, guint8 key_id)
{
	Line s = { .mid = mid, .keys = obj() };
	guint8 scalar[32];

	memset(scalar, key_id, sizeof(scalar));
	g_autofree char *encoded = g_base64_encode(scalar, sizeof(scalar));
	g_autofree char *id = g_strdup_printf("%u", key_id);
	J *own = obj();

	json_object_object_add(own, id, json_object_new_string(encoded));
	json_object_object_add(s.keys, "active", json_object_new_int(key_id));
	json_object_object_add(s.keys, "own", own);
	json_object_object_add(s.keys, "peers", obj());
	return s;
}

static void clear_account(Line *s)
{
	json_object_put(s->keys);
	g_clear_pointer(&s->history_secrets, g_array_unref);
	g_clear_pointer(&s->profiles, g_hash_table_unref);
}

static J *contact_key(const char *mid)
{
	guint8 id = !strcmp(mid, BOB) ? 20 : 10, scalar[32], pub[32];
	size_t n = sizeof(pub);

	memset(scalar, id, sizeof(scalar));
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
						   scalar, sizeof(scalar));
	g_assert_cmpint(EVP_PKEY_get_raw_public_key(k, pub, &n), ==, 1);
	EVP_PKEY_free(k);
	J *key = obj();

	json_object_object_add(key, "2", json_object_new_int(id));
	json_object_object_add(
		key, "4", json_object_new_string_len((const char *)pub, n));
	return key;
}

J *line_rpc(Line *s, const char *path, const char *method, J *args, GError **e)
{
	(void)path;
	(void)e;
	if (!strcmp(method, "negotiateE2EEPublicKey")) {
		J *out = obj();

		json_object_object_add(out, "2",
				       contact_key(strfield(args, "2:str")));
		json_object_object_add(out, "3", json_object_new_int(2));
		return out;
	}
	if (!strcmp(method, "getE2EEPublicKey"))
		return contact_key(strfield(args, "2:str"));
	if (!strcmp(method, "getContactsV3"))
		return obj();
	if (!strcmp(method, "getRecentMessagesV2")) {
		J *out = json_object_new_array();

		json_object_array_add(out, json_object_get(message));
		return out;
	}
	g_assert_cmpstr(method, ==, "sendMessage");
	J *src = field(args, "2:obj"), *meta = field(src, "18:map:str:str");

	g_assert_cmpstr(strfield(meta, "STICON_OWNERSHIP"), ==,
			"[\"" PACK "\"]");
	g_assert_null(field(meta, "REPLACE"));
	g_assert_null(field(src, "10:str"));
	g_assert_cmpstr(strfield(src, "21:str"), ==, "123");
	g_assert_cmpint(json_object_array_length(field(src, "20:list:str")), ==,
			5);
	g_clear_pointer(&message, json_object_put);
	message = obj();
	json_object_object_add(message, "1", json_object_new_string(s->mid));
	json_object_object_add(message, "2",
			       json_object_get(field(src, "2:str")));
	json_object_object_add(message, "15", json_object_new_int(0));
	json_object_object_add(message, "18", json_object_get(meta));
	json_object_object_add(message, "20",
			       json_object_get(field(src, "20:list:str")));
	json_object_object_add(message, "4", json_object_new_string("124"));
	sent_count++;
	J *receipt = obj();

	json_object_object_add(receipt, "4", json_object_new_string("124"));
	return receipt;
}

static J *spans(void)
{
	return json_tokener_parse(
		"[{\"productId\":\"" PACK
		"\",\"emojiId\":\"001\",\"start\":3,\"length\":7,"
		"\"version\":3,\"resourceType\":\"ANIMATION\"},{\"productId\":\"" PACK
		"\",\"emojiId\":\"003\","
		"\"start\":11,\"length\":6}]");
}

static J *field_at(J *array, size_t index, const char *name)
{
	return field(json_object_array_get_idx(array, index), name);
}

static void encrypted_emoji(void)
{
	Line alice = account(ALICE, 10), bob = account(BOB, 20);

	g_autoptr(GError) e = NULL;
	g_autoptr(J) items = spans();
	g_autoptr(J) ack =
		line_send_emoji(&alice, BOB, CAPTION, "123", items, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(strfield(ack, "id"), ==, "124");
	g_autoptr(J) plain = line_open_payload(&bob, message, 0, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(strfield(plain, "text"), ==, CAPTION);
	J *sticons =
		field(field(field(plain, "REPLACE"), "sticon"), "resources");
	g_assert_cmpuint(json_object_array_length(sticons), ==, 2);
	g_assert_cmpint(json_object_get_int(field_at(sticons, 0, "S")), ==, 3);
	g_assert_cmpint(json_object_get_int(field_at(sticons, 0, "E")), ==, 10);
	g_assert_cmpint(json_object_get_int(field_at(sticons, 1, "S")), ==, 11);
	g_assert_cmpint(json_object_get_int(field_at(sticons, 1, "E")), ==, 17);
	g_assert_cmpstr(
		json_object_get_string(field_at(sticons, 0, "sticonId")), ==,
		"001");
	g_assert_cmpint(json_object_get_int(field_at(sticons, 1, "version")),
			==, 1);
	g_assert_cmpstr(
		json_object_get_string(field_at(sticons, 1, "resourceType")),
		==, "STATIC");
	json_object_object_add(field(message, "18"), "REPLACE",
			       json_object_new_string("{}"));
	g_autoptr(J) rows = line_history(&bob, ALICE, &e);
	g_assert_no_error(e);
	J *row = json_object_array_get_idx(rows, 0);

	g_assert_cmpstr(strfield(row, "text"), ==, CAPTION);
	J *views = field(row, "emojis");

	g_assert_cmpuint(json_object_array_length(views), ==, 2);
	g_assert_cmpint(json_object_get_int(field_at(views, 0, "length")), ==,
			7);
	g_assert_cmpstr(
		json_object_get_string(field_at(views, 0, "resourceType")), ==,
		"ANIMATION");
	g_assert_cmpint(json_object_get_int(field_at(views, 1, "start")), ==,
			11);
	clear_account(&alice);
	clear_account(&bob);
	g_clear_pointer(&message, json_object_put);
}

static void invalid_offsets(void)
{
	Line s = account(ALICE, 10);
	guint before = sent_count;

	g_autoptr(GError) e = NULL;
	const int bad[][2] = { { 1, 1 },  { 0, 1 }, { 17, 1 }, { 3, -4 },
			       { 3, 64 }, { 9, 5 }, { 0, 0 } };
	for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(J) list = spans();
		J *first = json_object_array_get_idx(list, 0);

		json_object_object_add(first, "start",
				       json_object_new_int(bad[i][0]));
		json_object_object_add(first, "length",
				       json_object_new_int(bad[i][1]));
		g_assert_null(
			line_send_emoji(&s, BOB, CAPTION, "123", list, &e));
		g_assert_error(e, LINE_ERROR, 1);
		g_clear_error(&e);
	}
	g_assert_cmpuint(sent_count, ==, before);
	g_assert_cmpint(s.seq, ==, 0);
	clear_account(&s);
}

static void legacy_open(void)
{
	Line s = account(BOB, 20);

	g_autoptr(GError) e = NULL;
	const char *wire = "{\"sticon\":{\"resources\":[{\"productId\":\"" PACK
			   "\",\"sticonId\":\"001\","
			   "\"S\":3,\"E\":10,\"version\":3}]}}";
	message = obj();
	J *meta = obj();

	json_object_object_add(message, "1", json_object_new_string(ALICE));
	json_object_object_add(message, "2", json_object_new_string(BOB));
	json_object_object_add(message, "10", json_object_new_string(CAPTION));
	json_object_object_add(message, "18", meta);
	json_object_object_add(meta, "REPLACE", json_object_new_string(wire));
	g_autoptr(J) legacy = line_history(&s, ALICE, &e);
	g_assert_no_error(e);
	J *views = field_at(legacy, 0, "emojis");

	g_assert_cmpstr(json_object_get_string(field_at(views, 0, "emojiId")),
			==, "001");
	json_object_object_add(meta, "REPLACE", json_object_new_string("{"));
	g_autoptr(J) broken = line_history(&s, ALICE, &e);
	g_assert_no_error(e);
	g_assert_null(field_at(broken, 0, "emojis"));
	g_assert_cmpstr(json_object_get_string(field_at(broken, 0, "text")), ==,
			CAPTION);
	clear_account(&s);
	g_clear_pointer(&message, json_object_put);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/emoji/encrypted-two-accounts", encrypted_emoji);
	g_test_add_func("/emoji/invalid-utf16-offsets", invalid_offsets);
	g_test_add_func("/emoji/plaintext-history", legacy_open);
	return g_test_run();
}
