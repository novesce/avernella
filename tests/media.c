// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.h"
#include "media_crypto.h"
#include "obs_http.h"
#include "sealing.h"
#include "messaging.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define ME "uaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OTHER "ubbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define GROUP "ccccccccccccccccccccccccccccccccc"

static GBytes *object, *sidecar, *preview_blob;
static J *sent;
static int sent_count, upload_count, download_count, preview_count, token_count;
static gboolean reject_sidecar, tampered, stale_token;
static char *namespace;

static GBytes *unhex(const char *s)
{
	gsize n = strlen(s) / 2;
	guint8 *bytes = g_malloc(n);

	for (gsize i = 0; i < n; i++)
		bytes[i] = (g_ascii_xdigit_value(s[2 * i]) << 4) |
			   g_ascii_xdigit_value(s[2 * i + 1]);
	return g_bytes_new_take(bytes, n);
}

static J *raw64(const char *s)
{
	gsize n;

	g_autofree guint8 *b = g_base64_decode(s, &n);

	return json_object_new_string_len((const char *)b, (int)n);
}

static Line session(void)
{
	Line s = { .mid = ME, .keys = obj(), .seq = 40 };
	J *own = obj();

	json_object_object_add(s.keys, "active", json_object_new_int(20));
	json_object_object_add(s.keys, "peers", obj());
	json_object_object_add(s.keys, "own", own);
	for (guint8 id = 10; id <= 20; id += 10) {
		guint8 priv[32];

		memset(priv, id, sizeof(priv));
		g_autofree char *key = g_base64_encode(priv, sizeof(priv));

		json_object_object_add(own, id == 10 ? "10" : "20",
				       json_object_new_string(key));
	}
	return s;
}

char *line_obs_token(Line *s, GError **error)
{
	(void)error;
	token_count++;
	if (s->obs_token)
		return g_strdup(s->obs_token);
	return g_strdup("fixture-token");
}

char *line_obs_upload(const char *token, const char *sid, const char *oid,
		      const char *name, GBytes *encrypted, GError **error)
{
	g_assert_cmpstr(token, ==, "fixture-token");
	upload_count++;
	if (g_str_has_prefix(oid, "reqid-")) {
		g_assert_true(g_uuid_string_is_valid(oid + 6));
		g_free(namespace);
		namespace = g_strdup(sid);
		g_assert_false(g_str_has_prefix(name, "private-local-file"));
		g_clear_pointer(&object, g_bytes_unref);
		object = g_bytes_ref(encrypted);
		return g_strdup("fixture-object");
	}
	g_assert_cmpstr(sid, ==, namespace);
	if (reject_sidecar) {
		g_set_error_literal(error, LINE_ERROR, 1,
				    "Fixture rejected sidecar");
		return NULL;
	}
	if (!strcmp(sid, "emi")) {
		g_assert_cmpstr(oid, ==, "fixture-object__ud-preview");
		g_assert_cmpstr(name, ==, "preview.jpg");
	} else {
		g_assert_cmpstr(oid, ==, "fixture-object__ud-hash");
		g_assert_cmpstr(name, ==, "hash");
	}
	g_clear_pointer(&sidecar, g_bytes_unref);
	sidecar = g_bytes_ref(encrypted);
	return g_strdup(oid);
}

GBytes *line_obs_download(const char *token, const char *sid, const char *oid,
			  const char *message_id, GError **error)
{
	if (stale_token && !strcmp(token, "stale-token")) {
		stale_token = FALSE;
		g_set_error_literal(error, LINE_ERROR, LINE_OBS_UNAUTHORIZED,
				    "Fixture OBS token expired.");
		return NULL;
	}
	g_assert_cmpstr(token, ==, "fixture-token");
	if (!strcmp(sid, "m")) {
		g_assert_cmpstr(oid, ==, "901");
		g_assert_cmpstr(message_id, ==, "901");
		download_count++;
		return g_bytes_new_static("legacy", 6);
	}
	g_assert_cmpstr(sid, ==, namespace);
	g_assert_cmpstr(oid, ==, "fixture-object");
	g_assert_cmpstr(message_id, ==, "900");
	download_count++;
	if (!tampered)
		return g_bytes_ref(object);
	gsize size;
	const guint8 *src = g_bytes_get_data(object, &size);
	guint8 *wrong = g_memdup2(src, size);

	wrong[0] ^= 1;
	return g_bytes_new_take(wrong, size);
}

GBytes *line_obs_download_preview(const char *token, const char *sid,
				  const char *oid, const char *id,
				  GError **error)
{
	(void)error;
	g_assert_cmpstr(token, ==, "fixture-token");
	preview_count++;
	if (!strcmp(sid, "emv")) {
		g_assert_cmpstr(oid, ==, "fixture-object");
		g_assert_cmpstr(id, ==, "900");
	} else {
		g_assert_cmpstr(sid, ==, "m");
		g_assert_cmpstr(oid, ==, "901");
		g_assert_cmpstr(id, ==, "901");
	}
	return g_bytes_ref(preview_blob);
}

char *line_obs_upload_square(const char *token, const char *message_id,
			     const char *name, const char *type, GBytes *body,
			     GError **error)
{
	(void)token;
	(void)message_id;
	(void)name;
	(void)type;
	(void)body;
	g_set_error_literal(error, LINE_ERROR, 1, "Unused square upload");
	return NULL;
}

GBytes *line_obs_download_square(const char *token, const char *message_id,
				 GError **error)
{
	(void)token;
	(void)message_id;
	g_set_error_literal(error, LINE_ERROR, 1, "Unused square download");
	return NULL;
}

GBytes *line_obs_download_square_preview(const char *token,
					 const char *message_id, GError **error)
{
	(void)token;
	(void)message_id;
	g_set_error_literal(error, LINE_ERROR, 1, "Unused square preview");
	return NULL;
}

gboolean line_is_square_chat(const char *id)
{
	return id && id[0] == 'm' && strlen(id) == 33;
}

J *line_square_send_media_stub(Line *s, const char *chat,
			       const char *my_member_id, int content_type,
			       J *metadata, gint32 seq, GError **error)
{
	(void)s;
	(void)chat;
	(void)my_member_id;
	(void)content_type;
	(void)metadata;
	(void)seq;
	g_set_error_literal(error, LINE_ERROR, 1, "Unused square send");
	return NULL;
}

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)error;
	g_assert_cmpstr(path, ==, "/S4");
	if (!strcmp(method, "getLastE2EEGroupSharedKey")) {
		g_assert_cmpstr(strfield(args, "3:str"), ==, GROUP);
		J *r = obj();

		json_object_object_add(r, "2", json_object_new_int(40));
		json_object_object_add(r, "3", json_object_new_string(OTHER));
		json_object_object_add(r, "4", json_object_new_int(21));
		json_object_object_add(r, "5", json_object_new_string(ME));
		json_object_object_add(r, "6", json_object_new_int(10));
		json_object_object_add(
			r, "7",
			raw64("4Py/gU05RqUzN5arb6iW5JZB9dfMAj6kfyNcaVjwxuysXY6AhwyjR63/SRejKjbI"));
		return r;
	}
	if (!strcmp(method, "getE2EEPublicKey") ||
	    !strcmp(method, "negotiateE2EEPublicKey")) {
		J *key = obj();

		g_assert_cmpstr(strfield(args, "2:str"), ==, OTHER);
		json_object_object_add(key, "2", json_object_new_int(21));
		json_object_object_add(
			key, "4",
			raw64("vOBZv1sqt6kfPoY6zwyE0+u+BMqEkAlLBStbFa+rF0M="));
		if (!strcmp(method, "getE2EEPublicKey"))
			return key;
		J *r = obj();

		json_object_object_add(r, "2", key);
		json_object_object_add(r, "3", json_object_new_int(2));
		return r;
	}
	g_assert_cmpstr(method, ==, "sendMessage");
	sent_count++;
	g_assert_cmpint(json_object_get_int64(field(args, "1:i32")), ==,
			s->seq);
	J *wire = field(args, "2:obj");

	g_assert_null(field(wire, "10:str"));
	if (sent)
		json_object_put(sent);
	sent = obj();
	json_object_object_add(sent, "1", json_object_new_string(ME));
	json_object_object_add(sent, "4", json_object_new_string("900"));
	json_object_object_foreach(wire, typed, value) {
		g_autofree char *key = g_strndup(typed, strcspn(typed, ":"));

		json_object_object_add(sent, key, json_object_get(value));
	}
	J *receipt = obj();

	json_object_object_add(receipt, "4", json_object_new_string("900"));
	return receipt;
}

static void crypto_vector(void)
{
	/* Node HKDF/AES-CTR/HMAC cross-implementation vector, 40 bytes of 0x07. */
	guint8 material[32], input[40];

	memset(material, 1, sizeof(material));
	memset(input, 7, sizeof(input));
	g_autoptr(GError) err = NULL;
	g_autoptr(GBytes) expected = unhex(
		"b2fbb80fb2b115e55fd1c44075de7d169f8ffb93980194527a41e4e99d7b8a41f80f4ed43145559f"
		"3ec92c9db2365f64e1809b75cc0f99301c2500e88969330e10a105b59518d428");
	g_autoptr(GBytes) encrypted = line_media_encrypt(
		g_bytes_new(input, sizeof(input)), material, NULL, &err);
	g_assert_no_error(err);
	g_assert_true(g_bytes_equal(encrypted, expected));
	g_autoptr(GBytes) opened =
		line_media_decrypt(g_bytes_ref(expected), material, &err);
	g_assert_no_error(err);
	g_assert_cmpmem(g_bytes_get_data(opened, NULL),
			g_bytes_get_size(opened), input, sizeof(input));
}

static void chunk_vector(void)
{
	guint8 key[32];

	memset(key, 1, sizeof(key));
	size_t length = 128 * 1024 + 7;
	guint8 *p = g_malloc(length);

	for (size_t i = 0; i < length; i++)
		p[i] = (guint8)i;
	g_autoptr(GBytes) want = unhex(
		"9fd08ffae11a9d29e65986f8db364ecadd302025f685241d55c1f85fe63da6a3"
		"0d36172132d7abeb51ae94888c3b4ee2e0b74c34901d1bb2108f440c80ab0908");
	g_autoptr(GBytes) tag = unhex(
		"a20d90c4e3e420ce8e29c79d4f12901a1e3acf29f53d25644bd6cf3925467fc2");
	g_autoptr(GBytes) hashes = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = line_media_encrypt(g_bytes_new_take(p, length),
						    key, &hashes, &error);
	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(hashes, want));
	const guint8 *raw = g_bytes_get_data(body, NULL);

	g_assert_cmpmem(raw + length, 32, g_bytes_get_data(tag, NULL), 32);
	g_autoptr(GBytes) read =
		line_media_decrypt(g_bytes_ref(body), key, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(read), ==, length);
	const guint8 *opened = g_bytes_get_data(read, NULL);

	for (size_t i = 0; i < length; i++)
		g_assert_cmpuint(opened[i], ==, (guint8)i);
}

static void video_preview(Line *s)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(J) payload =
		line_open_payload(s, sent, LINE_MEDIA_VIDEO, &error);
	g_assert_no_error(error);
	gsize key_size;
	g_autofree guint8 *key =
		g_base64_decode(strfield(payload, "keyMaterial"), &key_size);
	g_assert_cmpuint(key_size, ==, 32);
	g_autoptr(GBytes) png = unhex("89504e470d0a1a0a0000000049454e44");

	preview_blob = line_media_encrypt(g_bytes_ref(png), key, NULL, &error);
	g_assert_no_error(error);
	preview_count = 0;
	int full_downloads = download_count;

	g_autoptr(GBytes) image = line_download_video_preview(s, sent, &error);
	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(image, png));

	g_bytes_unref(preview_blob);
	g_autoptr(GBytes) not_an_image = g_bytes_new_static("movie data", 10);
	preview_blob = line_media_encrypt(g_bytes_ref(not_an_image), key, NULL,
					  &error);
	g_assert_no_error(error);
	g_assert_null(line_download_video_preview(s, sent, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);

	g_bytes_unref(preview_blob);
	preview_blob = g_bytes_ref(png);
	g_assert_null(line_download_video_preview(s, sent, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpint(preview_count, ==, 3);
	g_assert_cmpint(download_count, ==, full_downloads);
	g_clear_pointer(&preview_blob, g_bytes_unref);
}

static void attachment_case(gconstpointer value)
{
	int type = GPOINTER_TO_INT(value);
	Line s = session();

	sent_count = upload_count = download_count = 0;
	g_clear_pointer(&sidecar, g_bytes_unref);
	g_autoptr(GError) err = NULL;
	g_autofree char *dir = g_dir_make_tmp("line-media-XXXXXX", &err);

	g_assert_no_error(err);
	g_autofree char *path =
		g_build_filename(dir, "private-local-file-🌙.data", NULL);
	gsize n = 0;
	g_autofree guint8 *data = NULL;

	if (type == LINE_MEDIA_IMAGE) {
		data = g_base64_decode(
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1V8AAAAASUVORK5CYII=",
			&n);
	} else {
		n = 128 * 1024 + 7;
		data = g_malloc(n);
		for (gsize i = 0; i < n; i++)
			data[i] = (guint8)i;
		if (type == LINE_MEDIA_VIDEO) {
			/* Extended moov, v1 mvhd, 3000 ticks and 2000 ticks/second. */
			g_autoptr(GBytes) mp4 =
				unhex("0000000866747970"
				      "000000016d6f6f760000000000000038"
				      "000000286d76686401000000"
				      "00000000000000000000000000000000"
				      "000007d00000000000000bb8");
			gsize head;
			const guint8 *prefix = g_bytes_get_data(mp4, &head);

			memcpy(data, prefix, head);
		}
	}
	g_assert_true(g_file_set_contents(path, (char *)data, n, &err));
	const char *to = type == LINE_MEDIA_IMAGE || type == LINE_MEDIA_VIDEO ?
				 GROUP :
				 OTHER;
	g_autoptr(J) ack = line_send_attachment(&s, to, path, type, &err);
	g_assert_no_error(err);
	g_assert_cmpstr(strfield(ack, "id"), ==, "900");
	g_assert_cmpint(s.seq, ==, 41);
	g_assert_cmpint(sent_count, ==, 1);
	g_assert_cmpint(
		upload_count, ==,
		type == LINE_MEDIA_IMAGE || type == LINE_MEDIA_VIDEO ? 2 : 1);
	g_assert_cmpuint(g_bytes_get_size(object), ==, n + 32);
	g_assert_cmpint(memcmp(g_bytes_get_data(object, NULL), data, n), !=, 0);
	if (type == LINE_MEDIA_IMAGE)
		g_assert_true(g_bytes_equal(sidecar, object));
	if (type == LINE_MEDIA_VIDEO) {
		g_assert_cmpuint(g_bytes_get_size(sidecar), ==, 64);
		g_assert_cmpstr(strfield(field(sent, "18"), "DURATION"), ==,
				"1500");
	} else {
		g_assert_null(field(field(sent, "18"), "DURATION"));
	}
	g_autoptr(GBytes) read = line_download_attachment(&s, sent, &err);
	g_assert_no_error(err);
	g_assert_cmpmem(g_bytes_get_data(read, NULL), g_bytes_get_size(read),
			data, n);
	g_assert_cmpint(download_count, ==, 1);
	if (type == LINE_MEDIA_VIDEO) {
		video_preview(&s);
		/* The verified envelope also identifies attachments without this marker. */
		json_object_object_del(field(sent, "18"), "e2eeVersion");
		g_autoptr(GBytes) legacy_envelope =
			line_download_attachment(&s, sent, &err);

		g_assert_no_error(err);
		g_assert_true(g_bytes_equal(legacy_envelope, read));
	}
	if (type == LINE_MEDIA_FILE) {
		g_autofree char *name =
			line_attachment_filename(&s, sent, &err);
		g_assert_no_error(err);
		g_assert_cmpstr(name, ==, "private-local-file-🌙.data");
	}
	tampered = TRUE;
	g_assert_null(line_download_attachment(&s, sent, &err));
	g_assert_error(err, LINE_ERROR, 1);
	g_clear_error(&err);
	tampered = FALSE;

	/*
	 * Changing the public type while retaining a sealed payload must fail
	 * before any request for pixels or bytes.
	 */
	int prior = download_count;
	int substitute = type == LINE_MEDIA_AUDIO ? LINE_MEDIA_FILE :
						    LINE_MEDIA_AUDIO;
	json_object_object_add(sent, "15", json_object_new_int(substitute));
	json_object_object_add(
		field(sent, "18"), "SID",
		json_object_new_string(substitute == 14 ? "emf" : "ema"));
	g_assert_null(line_download_attachment(&s, sent, &err));
	g_assert_error(err, LINE_ERROR, 1);
	g_assert_cmpint(download_count, ==, prior);
	g_clear_error(&err);
	if (type == LINE_MEDIA_IMAGE) {
		reject_sidecar = TRUE;
		g_assert_null(line_send_image(&s, to, path, &err));
		g_assert_error(err, LINE_ERROR, 1);
		g_assert_cmpint(sent_count, ==, 1);
		reject_sidecar = FALSE;
	}
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

static void legacy(void)
{
	Line session = { 0 };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) msg = obj();
	J *meta = obj();

	json_object_object_add(msg, "18", meta);
	json_object_object_add(msg, "4", json_object_new_string("901"));
	json_object_object_add(meta, "FILE_NAME",
			       json_object_new_string("file.txt"));
	json_object_object_add(msg, "15", json_object_new_int(LINE_MEDIA_FILE));
	g_autofree char *name = line_attachment_filename(&session, msg, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(name, ==, "file.txt");
	/* A sender's hidden or disguised name is never offered for saving. */
	const char *const unsafe[] = { ".bash_profile", "photo\u202egpj.sh",
				       "a\nb" };

	for (gsize i = 0; i < G_N_ELEMENTS(unsafe); i++) {
		json_object_object_add(meta, "FILE_NAME",
				       json_object_new_string(unsafe[i]));
		g_autofree char *named =
			line_attachment_filename(&session, msg, &error);

		g_assert_no_error(error);
		g_assert_cmpstr(named, ==, "attachment");
	}
	json_object_object_add(meta, "FILE_NAME",
			       json_object_new_string("file.txt"));
	download_count = 0;
	g_autoptr(GBytes) file =
		line_download_attachment(&session, msg, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(file, NULL), g_bytes_get_size(file),
			"legacy", 6);
	json_object_object_add(msg, "15",
			       json_object_new_int(LINE_MEDIA_IMAGE));
	g_autoptr(GBytes) image = line_download_image(&session, msg, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(image), ==, 6);
	json_object_object_add(msg, "15",
			       json_object_new_int(LINE_MEDIA_VIDEO));
	preview_count = 0;
	preview_blob = unhex("ffd8ff000000000000000000");
	g_autoptr(GBytes) video_thumb =
		line_download_video_preview(&session, msg, &error);

	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(video_thumb, preview_blob));
	g_assert_cmpint(preview_count, ==, 1);
	g_clear_pointer(&preview_blob, g_bytes_unref);
	const char *const indicators[] = { "OID", "SID", "e2eeVersion",
					   "e2eeMark", "keyMaterial" };
	for (size_t i = 0; i < G_N_ELEMENTS(indicators); i++) {
		json_object_object_add(meta, indicators[i],
				       json_object_new_string(""));
		g_assert_null(line_download_attachment(&session, msg, &error));
		g_assert_error(error, LINE_ERROR, 1);
		g_clear_error(&error);
		json_object_object_del(meta, indicators[i]);
	}
	J *chunks = json_object_new_array();

	json_object_array_add(chunks, json_object_new_string("bad"));
	json_object_object_add(msg, "20", chunks);
	g_assert_null(line_download_attachment(&session, msg, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	json_object_object_del(msg, "20");
	json_object_object_add(msg, "4", json_object_new_string("../some-id"));
	g_assert_null(line_download_attachment(&session, msg, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpint(download_count, ==, 2);
}

static void token_refresh(void)
{
	Line s = { .obs_token = g_strdup("stale-token"),
		   .obs_until = G_MAXINT64 };
	g_autoptr(J) msg = json_tokener_parse("{\"4\":\"901\",\"15\":2}");
	g_autoptr(GError) error = NULL;

	token_count = 0;
	stale_token = TRUE;
	g_autoptr(GBytes) video = line_download_attachment(&s, msg, &error);

	g_assert_no_error(error);
	g_assert_nonnull(video);
	g_assert_cmpint(token_count, ==, 2);
	g_assert_null(s.obs_token);
	g_assert_cmpint(s.obs_until, ==, 0);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/media/legacy-no-sealed-fallback", legacy);
	g_test_add_func("/media/video-stale-obs-token-refresh", token_refresh);
	g_test_add_func("/media/independent-crypto-vector", crypto_vector);
	g_test_add_func("/media/independent-video-chunks", chunk_vector);
	g_test_add_data_func("/media/group-image",
			     GINT_TO_POINTER(LINE_MEDIA_IMAGE),
			     attachment_case);
	g_test_add_data_func("/media/group-video",
			     GINT_TO_POINTER(LINE_MEDIA_VIDEO),
			     attachment_case);
	g_test_add_data_func("/media/direct-audio",
			     GINT_TO_POINTER(LINE_MEDIA_AUDIO),
			     attachment_case);
	g_test_add_data_func("/media/direct-file",
			     GINT_TO_POINTER(LINE_MEDIA_FILE), attachment_case);
	int status = g_test_run();

	g_clear_pointer(&object, g_bytes_unref);
	g_clear_pointer(&sidecar, g_bytes_unref);
	g_clear_pointer(&sent, json_object_put);
	g_free(namespace);
	return status;
}
