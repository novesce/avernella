// SPDX-License-Identifier: GPL-3.0-or-later
#include "sealing.h"
#include <string.h>

#define ME "uaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PEER "ubbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define GROUP "ccccccccccccccccccccccccccccccccc"
#define PEER_KEY "vOBZv1sqt6kfPoY6zwyE0+u+BMqEkAlLBStbFa+rF0M="

/*
 * Independent fixtures from X25519, SHA-256, AES-CBC/ECB/GCM and a hand-
 * encoded Compact struct. Each private scalar consists of 32 repeated bytes.
 * QR transport uses bytes 99 and 100. Own key IDs use bytes 10 and 20,
 * and the peer uses byte 21. Salt and nonce count up from zero.
 */
static const char *metadata =
	"{\"keyId\":\"20\",\"publicKey\":\"az1Xdi+LrbaUM/nG0GD4X15ca2gDqBbRQcB19jVBrRA=\","
	"\"encryptedKeyChain\":\"k9NmCtDdrKpc6uz/Mv2NahyIfEcmaHqZz7Xl3spEcEojFd8kSV//Ls8iEA8b"
	"UAHZEOY/gH+ITQh4HFR6JjOw4KY0tnyhHd3u4W7Yde/ZUvP/VGNAsEfFZ0D+vkVyajTgLKHWcaT13Kxgah/"
	"iC8ld0FyFJjg6qv5Zxlu1bXn0asXIJcf2NwqSa+2nTVerPDsPTi4ZgDtC+w/+UHxEsWfRtQ==\","
	"\"hashKeyChain\":\"Yvt8q/eYpK/Fa6MUfUPavg==\"}";

static J *raw64(const char *encoded)
{
	gsize size;

	g_autofree guint8 *bytes = g_base64_decode(encoded, &size);

	return json_object_new_string_len((const char *)bytes, (int)size);
}

static gboolean bad_peer, no_key_id, unavailable_exact;
static unsigned int lookups, group_lookups;

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)s;
	g_assert_cmpstr(path, ==, "/S4");
	if (!strcmp(method, "getE2EEGroupSharedKey") ||
	    !strcmp(method, "getLastE2EEGroupSharedKey")) {
		g_assert_cmpstr(strfield(args, "3:str"), ==, GROUP);
		g_assert_cmpint(json_object_get_int(field(args, "2:i32")), ==,
				1);
		if (!strcmp(method, "getE2EEGroupSharedKey"))
			g_assert_cmpint(
				json_object_get_int(field(args, "4:i32")), ==,
				40);
		group_lookups++;
		J *wrap = obj();

		json_object_object_add(wrap, "2", json_object_new_int(40));
		json_object_object_add(wrap, "3", json_object_new_string(PEER));
		json_object_object_add(wrap, "4", json_object_new_int(21));
		json_object_object_add(wrap, "5", json_object_new_string(ME));
		json_object_object_add(wrap, "6", json_object_new_int(10));
		json_object_object_add(
			wrap, "7",
			raw64("4Py/gU05RqUzN5arb6iW5JZB9dfMAj6kfyNcaVjwxuysXY6AhwyjR63/SRejKjbI"));
		json_object_object_add(wrap, "9", json_object_new_int(2));
		return wrap;
	}
	g_assert_cmpstr(strfield(args, "2:str"), ==, PEER);
	lookups++;
	if (unavailable_exact && !strcmp(method, "getE2EEPublicKey")) {
		g_set_error_literal(error, LINE_ERROR, LINE_METHOD_UNAVAILABLE,
				    "Unavailable fixture route.");
		return NULL;
	}
	J *key = obj();

	if (!no_key_id)
		json_object_object_add(key, "2",
				       json_object_new_int(bad_peer ? 22 : 21));
	json_object_object_add(key, "4", raw64(PEER_KEY));
	if (!strcmp(method, "getE2EEPublicKey")) {
		g_assert_cmpint(json_object_get_int(field(args, "3:i32")), ==,
				1);
		g_assert_cmpint(json_object_get_int(field(args, "4:i32")), ==,
				21);
		return key;
	}
	g_assert_cmpstr(method, ==, "negotiateE2EEPublicKey");
	J *reply = obj();

	json_object_object_add(reply, "2", key);
	json_object_object_add(reply, "3", json_object_new_int(2));
	return reply;
}

static Line linked(void)
{
	guint8 qr_secret[32];

	memset(qr_secret, 99, sizeof(qr_secret));
	Line session = { .mid = ME };

	g_autoptr(J) meta = json_object_new_string(metadata);
	g_autoptr(GError) error = NULL;
	g_assert_true(line_import_keys(&session, meta, qr_secret, &error));
	g_assert_no_error(error);
	return session;
}

static J *message(J *chunks, gboolean own)
{
	J *m = obj(), *meta = obj();

	json_object_object_add(m, "1", json_object_new_string(own ? ME : PEER));
	json_object_object_add(m, "2", json_object_new_string(own ? PEER : ME));
	json_object_object_add(m, "20", chunks);
	json_object_object_add(meta, "e2eeVersion",
			       json_object_new_string("2"));
	json_object_object_add(m, "18", meta);
	return m;
}

static J *fixture(gboolean historical)
{
	J *arr = json_object_new_array();

	json_object_array_add(arr, raw64("AAECAwQFBgcICQoLDA0ODw=="));
	json_object_array_add(
		arr,
		raw64(historical ?
			      "0BWZemHhUrVMelHEJXlEuaS2vkvMjMDyDRfdoCHQWD4uZZQYBS/nuwqjLoASrvn1Iu5oAA==" :
			      "QkdHUBAc7cC+9OLrhefu8xNKii7TCqD35Ug5NRZ5Mo1HsihmMFFgveE7J1RIqxab0ZWbdQ=="));
	json_object_array_add(arr, raw64("AAECAwQFBgcICQoL"));
	json_object_array_add(arr, raw64("AAAAFQ=="));
	json_object_array_add(arr, raw64(historical ? "AAAACg==" : "AAAAFA=="));
	return message(arr, FALSE);
}

static void test_keychain(void)
{
	Line s = linked();

	g_autoptr(J) bad = json_tokener_parse(metadata);
	J *before = s.keys;
	guint8 qr_secret[32];

	memset(qr_secret, 99, sizeof(qr_secret));
	g_autoptr(GError) error = NULL;
	json_object_object_add(bad, "keyId", json_object_new_int(27));
	g_assert_false(line_import_keys(&s, bad, qr_secret, &error));
	g_assert_true(s.keys == before);
	g_clear_error(&error);
	json_object_object_add(bad, "keyId", json_object_new_int(20));
	json_object_object_add(
		bad, "hashKeyChain",
		json_object_new_string("AAAAAAAAAAAAAAAAAAAAAA=="));
	g_assert_false(line_import_keys(&s, bad, qr_secret, &error));
	g_assert_true(s.keys == before);
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

static void test_login_proof(void)
{
	g_autoptr(J) meta = json_tokener_parse(metadata);
	g_autoptr(GError) error = NULL;
	guint8 secret[32], proof[16];
	Line s = { 0 };

	memset(secret, 99, sizeof(secret));
	json_object_object_del(meta, "hashKeyChain");
	g_assert_true(line_import_login_keys(&s, meta, secret, proof, &error));
	g_assert_no_error(error);
	g_autofree char *encoded = g_base64_encode(proof, sizeof(proof));

	g_assert_cmpstr(encoded, ==, "Yvt8q/eYpK/Fa6MUfUPavg==");
	json_object_put(s.keys);
}

static void test_open(void)
{
	Line s = linked();

	g_autoptr(GError) error = NULL;
	lookups = 0;
	for (int i = 0; i <= 1; i++) {
		g_autoptr(J) m = fixture(i);
		g_autofree char *text = line_open_text(&s, m, &error);

		g_assert_no_error(error);
		g_assert_cmpstr(text, ==, "Hello, สวัสดี");
	}
	g_assert_cmpuint(lookups, ==, 1);
	g_autoptr(J) m = fixture(FALSE);
	json_object_array_put_idx(field(m, "20"), 2, raw64("AQECAwQFBgcICQoL"));
	g_assert_null(line_open_text(&s, m, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

static void test_send(void)
{
	Line s = linked();

	g_autoptr(GError) error = NULL;
	J *chunks = line_seal_text(&s, PEER, "First line.\nSecond 🌙", &error);

	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(chunks), ==, 5);
	g_autoptr(J) m = message(chunks, TRUE);
	g_autofree char *text = line_open_text(&s, m, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(text, ==, "First line.\nSecond 🌙");
	g_assert_null(line_seal_text(&s, "invalid-id", "invalid", &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

static void test_incoming_compatibility(void)
{
	Line s = linked();

	g_autoptr(GError) error = NULL;
	J *chunks = json_object_new_array();

	json_object_array_add(chunks, raw64("AAECAwQFBgc="));
	json_object_array_add(
		chunks,
		raw64("Xv3VXP7nR8ta3JmUEabtoH4rL1mXU8cQ+NUKaTZrdTqz7qf+W8gk/fh5EsQSbIlf"));
	json_object_array_add(chunks, raw64("caDTRG9u6Sz0+pMjJgROGg=="));
	json_object_array_add(chunks, raw64("FQ=="));
	json_object_array_add(chunks, raw64("FA=="));
	g_autoptr(J) legacy = message(chunks, FALSE);

	json_object_object_add(field(legacy, "18"), "e2eeVersion",
			       json_object_new_string("1"));
	no_key_id = TRUE;
	g_autofree char *plain = line_open_text(&s, legacy, &error);

	no_key_id = FALSE;
	g_assert_no_error(error);
	g_assert_cmpstr(plain, ==, "Legacy message สวัสดี");

	/* Media decoders require a content type authenticated by v2. */
	json_object_object_add(legacy, "15", json_object_new_int(2));
	g_assert_null(line_open_payload(&s, legacy, 2, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	json_object_object_add(legacy, "15", json_object_new_int(0));

	json_object_array_put_idx(chunks, 2, raw64("AAAAAAAAAAAAAAAAAAAAAA=="));
	g_assert_null(line_open_text(&s, legacy, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);

	s = linked();
	g_autoptr(J) modern = fixture(FALSE);

	json_object_object_add(field(modern, "18"), "e2eeVersion",
			       json_object_new_string(""));
	json_object_array_put_idx(field(modern, "20"), 3, raw64("FQ=="));
	json_object_array_put_idx(field(modern, "20"), 4, raw64("FA=="));
	unavailable_exact = TRUE;
	bad_peer = TRUE;
	g_assert_null(line_open_text(&s, modern, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	bad_peer = FALSE;
	g_autofree char *opened = line_open_text(&s, modern, &error);

	unavailable_exact = FALSE;

	g_assert_no_error(error);
	g_assert_cmpstr(opened, ==, "Hello, สวัสดี");
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_object_del(field(s.keys, "own"), "20");
	g_assert_null(line_open_text(&s, modern, &error));
	g_assert_error(error, LINE_ERROR, LINE_OWN_KEY_MISSING);
	json_object_put(s.keys);
}

static void test_group(void)
{
	Line s = linked();

	g_autoptr(GError) error = NULL;
	group_lookups = 0;
	for (int mine = 0; mine < 2; mine++) {
		g_autoptr(J) m = fixture(FALSE);
		J *parts = field(m, "20");

		json_object_array_put_idx(
			parts, 1,
			raw64(mine ? "vw9hL51TOYkXOZApnGvqNvNVKdiD89QbL0Vgx8atZzd7ftYomZHXLhxXyoegUcd4Zh8+Sw==" :
				     "ubpU03+ye3Tf+PTRFClD+HQ4zDZw8i0FRaQfW6nbSawHbUgw7oi7Z76dI8UjX3Q5oEddIQ=="));
		json_object_array_put_idx(
			parts, 3, raw64(mine ? "AAAAFA==" : "AAAAFQ=="));
		json_object_array_put_idx(parts, 4, raw64("AAAAKA=="));
		json_object_object_add(
			m, "1", json_object_new_string(mine ? ME : PEER));
		json_object_object_add(m, "2", json_object_new_string(GROUP));
		g_autofree char *text = line_open_text(&s, m, &error);

		g_assert_no_error(error);
		g_assert_cmpstr(text, ==, "Hello, สวัสดี");
	}
	g_assert_cmpuint(group_lookups, ==, 1);
	J *chunks = line_seal_text(&s, GROUP, "Group text", &error);

	g_assert_no_error(error);
	g_assert_nonnull(chunks);
	g_assert_cmpuint(group_lookups, ==, 2);
	g_autoptr(J) own = message(chunks, TRUE);
	json_object_object_add(own, "2", json_object_new_string(GROUP));
	g_autofree char *round_trip = line_open_text(&s, own, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(round_trip, ==, "Group text");
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

static void test_rotation(void)
{
	Line s = linked();

	g_autoptr(J) m = fixture(FALSE);
	g_autoptr(GError) error = NULL;
	bad_peer = TRUE;
	g_assert_null(line_open_text(&s, m, &error));
	g_assert_error(error, LINE_ERROR, 1);
	bad_peer = FALSE;
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

/*
 * Run with -m perf -p /sealing/authenticated-history-cache. Both passes use
 * the same immutable RPC fixtures and retain the ordinary key-generation cache.
 */
static void benchmark_history(Line *s, J *current, J *old)
{
	const guint pages = 100, width = 50;

	for (guint warm = 0; warm <= 1; warm++) {
		const gint64 start = g_get_monotonic_time();

		for (guint page = 0; page < pages; page++)
			for (guint i = 0; i < width; i++) {
				if (!warm && s->history_secrets) {
					g_array_set_size(s->history_secrets, 0);
					s->next_secret = 0;
				}
				g_autofree char *text = line_open_text(
					s, i % 2 ? old : current, NULL);
				g_assert_nonnull(text);
			}
		g_test_message(
			"50-message page, %s shared secrets: %.3f ms/page (%u pages)",
			warm ? "reused" : "recomputed",
			(g_get_monotonic_time() - start) / (1000.0 * pages),
			pages);
	}
}

static void test_history_secret_cache(void)
{
	Line s = linked();

	g_autoptr(GError) error = NULL;
	g_autoptr(J) damaged = fixture(FALSE);
	json_object_array_put_idx(field(damaged, "20"), 2,
				  raw64("AQECAwQFBgcICQoL"));
	g_assert_null(line_open_text(&s, damaged, &error));
	g_clear_error(&error);
	g_assert_null(s.history_secrets);

	g_autoptr(J) current = fixture(FALSE), old = fixture(TRUE);
	for (guint i = 0; i < 50; i++) {
		g_autofree char *opened =
			line_open_text(&s, i % 2 ? old : current, &error);
		g_assert_cmpstr(opened, ==, "Hello, สวัสดี");
	}
	g_assert_cmpuint(s.history_secrets->len, ==, 2);
	g_assert_null(line_open_text(&s, damaged, &error));
	g_clear_error(&error);
	g_assert_cmpuint(s.history_secrets->len, ==, 2);

	if (g_test_perf())
		benchmark_history(&s, current, old);
	guint8 qr_secret[32];

	memset(qr_secret, 99, sizeof(qr_secret));
	g_autoptr(J) meta = json_object_new_string(metadata);
	g_assert_true(line_import_keys(&s, meta, qr_secret, &error));
	g_assert_null(s.history_secrets);
	g_assert_cmpuint(s.next_secret, ==, 0);
	json_object_put(s.keys);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sealing/keychain", test_keychain);
	g_test_add_func("/sealing/login-proof", test_login_proof);
	g_test_add_func("/sealing/authenticated-history-cache",
			test_history_secret_cache);
	g_test_add_func("/sealing/open", test_open);
	g_test_add_func("/sealing/incoming-compatibility",
			test_incoming_compatibility);
	g_test_add_func("/sealing/send", test_send);
	g_test_add_func("/sealing/existing-group", test_group);
	g_test_add_func("/sealing/mismatched-key-generation", test_rotation);
	return g_test_run();
}
