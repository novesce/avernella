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
#define SQUARE "m00000000000000000000000000000001"

/* Well-framed MP4 boxes for checking the handoff, without codec decoding. */
#define MP4                                                \
	"000000146674797069736f6d0000000069736f6d"         \
	"0000002c6d6f6f76000000247472616b0000001c6d646961" \
	"0000001468646c72000000000000000076696465"         \
	"0000000c6d64617461626364"

struct expected_video {
	const char *sid, *oid, *id, *pop, *url;
	gboolean square, preview;
	GBytes *reply;
};

static GBytes *object, *sidecar, *preview_blob;
static J *sent;
static const struct expected_video *video_route;
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

GBytes *line_obs_download_original(const char *token, const char *sid,
				   const char *oid, const char *message_id,
				   GError **error)
{
	return line_obs_download(token, sid, oid, message_id, error);
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

GBytes *line_obs_download_video(const char *token, const char *mid,
				const char *sid, const char *oid,
				const char *id, const char *pop,
				const char *url, gboolean square,
				gboolean preview, GError **error)
{
	if (!video_route) {
		if (!oid && !strcmp(sid, "m"))
			oid = id;
		return preview ? line_obs_download_preview(token, sid, oid, id,
							   error) :
				 line_obs_download(token, sid, oid, id, error);
	}
	g_assert_cmpstr(token, ==, "fixture-token");
	g_assert_cmpstr(mid, ==, ME);
	g_assert_cmpstr(sid, ==, video_route->sid);
	g_assert_cmpstr(oid, ==, video_route->oid);
	g_assert_cmpstr(id, ==, video_route->id);
	g_assert_cmpstr(pop, ==, video_route->pop);
	g_assert_cmpstr(url, ==, video_route->url);
	g_assert_cmpint(square, ==, video_route->square);
	g_assert_cmpint(preview, ==, video_route->preview);
	download_count++;
	return g_bytes_ref(video_route->reply);
}

static int square_upload_count;
static gboolean square_file_upload;

char *line_obs_upload_square(const char *token, const char *chat, gint32 seq,
			     const char *name, const char *type,
			     guint64 duration_ms, GBytes *body, GError **error)
{
	(void)duration_ms;
	square_upload_count++;
	if (!strcmp(token, "stale-token")) {
		g_set_error_literal(error, LINE_ERROR, LINE_OBS_UNAUTHORIZED,
				    "LINE media service returned HTTP 401.");
		return NULL;
	}
	g_assert_cmpstr(token, ==, "fixture-token");
	g_assert_cmpstr(chat, ==, "m00000000000000000000000000000001");
	g_assert_cmpint(seq, ==, 77);
	if (square_file_upload) {
		g_assert_true(g_str_has_suffix(name, ".zip"));
		g_assert_cmpstr(type, ==, "FILE");
	} else {
		g_assert_cmpstr(name, ==, "image.png");
		g_assert_cmpstr(type, ==, "IMAGE");
	}
	g_assert_cmpuint(g_bytes_get_size(body), >, 0);
	return g_strdup("609999999999");
}

GBytes *line_obs_download_square(const char *token, const char *message_id,
				 GError **error)
{
	(void)token;
	(void)message_id;
	g_set_error_literal(error, LINE_ERROR, 1, "Unused square download");
	return NULL;
}

GBytes *line_obs_download_square_original(const char *token,
					  const char *message_id,
					  GError **error)
{
	return line_obs_download_square(token, message_id, error);
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
	g_autoptr(GBytes) opened = line_media_decrypt(g_bytes_ref(expected),
						      material, LINE_MEDIA_BODY,
						      sizeof(input), &err);
	g_assert_no_error(err);
	g_assert_cmpmem(g_bytes_get_data(opened, NULL),
			g_bytes_get_size(opened), input, sizeof(input));
	g_assert_null(line_media_decrypt(g_bytes_ref(expected), material,
					 LINE_MEDIA_VIDEO_BODY, sizeof(input),
					 &err));
	g_assert_error(err, LINE_ERROR, 1);
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
	g_autoptr(GBytes) read = line_media_decrypt(
		g_bytes_ref(body), key, LINE_MEDIA_VIDEO_BODY, length, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(read), ==, length);
	const guint8 *opened = g_bytes_get_data(read, NULL);

	for (size_t i = 0; i < length; i++)
		g_assert_cmpuint(opened[i], ==, (guint8)i);
	g_assert_null(line_media_decrypt(g_bytes_ref(body), key,
					 LINE_MEDIA_BODY, length, &error));
	g_assert_error(error, LINE_ERROR, 1);
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

	/* A valid poster whose length overlaps a video MAC input is ambiguous. */
	guint8 pixels[64] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	g_bytes_unref(preview_blob);
	preview_blob = line_media_encrypt(g_bytes_new(pixels, sizeof(pixels)),
					  key, NULL, &error);
	g_assert_no_error(error);
	g_assert_null(line_download_video_preview(s, sent, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);

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
	g_assert_cmpint(preview_count, ==, 4);
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
		J *meta = field(sent, "18");

		json_object_object_add(meta, "FILE_SIZE",
				       json_object_new_string("42"));
		g_assert_null(line_download_attachment(&s, sent, &err));
		g_assert_error(err, LINE_ERROR, 1);
		g_clear_error(&err);
		int prior = download_count;

		json_object_object_add(meta, "FILE_SIZE",
				       json_object_new_null());
		g_assert_null(line_download_attachment(&s, sent, &err));
		g_assert_error(err, LINE_ERROR, 1);
		g_assert_cmpint(download_count, ==, prior);
		g_clear_error(&err);
		json_object_object_del(meta, "FILE_SIZE");
		g_autoptr(GBytes) no_size =
			line_download_attachment(&s, sent, &err);

		g_assert_no_error(err);
		g_assert_true(g_bytes_equal(no_size, read));
	}
	if (type == LINE_MEDIA_FILE) {
		g_assert_cmpuint(line_message_file_size(field(sent, "18")), ==,
				 n);
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
	if (type == LINE_MEDIA_FILE) {
		int tokens = token_count;

		json_object_object_del(field(s.keys, "own"), "20");
		g_assert_null(line_send_attachment(&s, to, path, type, &err));
		g_assert_error(err, LINE_ERROR, LINE_LETTER_SEALING_RELINK);
		g_clear_error(&err);
		json_object_object_del(s.keys, "active");
		g_assert_null(line_send_attachment(&s, to, path, type, &err));
		g_assert_error(err, LINE_ERROR, LINE_LETTER_SEALING_RELINK);
		g_assert_cmpint(sent_count, ==, 1);
		g_assert_cmpint(upload_count, ==, 1);
		g_assert_cmpint(token_count, ==, tokens);
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

static void video_routes(void)
{
	Line s = { .mid = ME };
	struct expected_video route = { .sid = "m",
					.oid = "object_456",
					.id = "901",
					.pop = "route/fixture",
					.url = "/r/talk/m/object_456" };
	g_autoptr(J) msg = json_tokener_parse(
		"{\"4\":\"901\",\"15\":2,\"18\":{\"SID\":\"m\","
		"\"OID\":\"object_456\",\"OBS_POP\":\"route/fixture\","
		"\"DOWNLOAD_URL\":\"/r/talk/m/object_456\"}}");
	g_autoptr(GBytes) mp4 = unhex(MP4);
	g_autoptr(GError) error = NULL;

	route.reply = mp4;
	video_route = &route;
	g_autoptr(GBytes) talk = line_download_video(&s, msg, &error);

	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(talk, mp4));
	J *meta = field(msg, "18");

	route.square = TRUE;
	route.oid = NULL;
	route.url = "https://obs-sg.line-apps.com/r/g2/m/object_456";
	json_object_object_del(meta, "OID");
	json_object_object_add(msg, "2", json_object_new_string(SQUARE));
	json_object_object_add(meta, "DOWNLOAD_URL",
			       json_object_new_string(route.url));
	g_autoptr(GBytes) openchat = line_download_video(&s, msg, &error);

	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(openchat, mp4));
	g_autoptr(GBytes) jpg = unhex("ffd8ff000000000000000000");

	route.preview = TRUE;
	route.url = "https://obs-sg.line-apps.com/r/g2/m/object_456/preview";
	route.reply = jpg;
	json_object_object_add(meta, "PREVIEW_URL",
			       json_object_new_string(route.url));
	g_autoptr(GBytes) preview =
		line_download_video_preview(&s, msg, &error);

	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(preview, jpg));
	video_route = NULL;
}

static void video_validate(void)
{
	Line s = { .mid = ME };
	struct expected_video route = { .sid = "m", .id = "901" };

	g_autoptr(J) msg = json_tokener_parse("{\"4\":\"901\",\"15\":2}");
	g_autoptr(GBytes) mp4 = unhex(MP4);
	g_autoptr(GBytes) partial =
		g_bytes_new_from_bytes(mp4, 0, g_bytes_get_size(mp4) - 1);
	g_autoptr(GBytes) html = g_bytes_new_static("<html>", 6);
	g_autoptr(GBytes) audio =
		unhex("000000146674797069736f6d0000000069736f6d"
		      "0000002c6d6f6f76000000247472616b0000001c6d646961"
		      "0000001468646c720000000000000000736f756e"
		      "0000000c6d64617461626364");
	GBytes *const invalid[] = { partial, html, audio };

	video_route = &route;
	for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_autoptr(GError) error = NULL;

		route.reply = invalid[i];
		g_assert_null(line_download_video(&s, msg, &error));
		g_assert_error(error, LINE_ERROR, 1);
	}
	video_route = NULL;
}

static void sealed_video_routing(gconstpointer data)
{
	gboolean sealed_oid = data != NULL;
	Line s = session();
	struct expected_video route = {
		.sid = "emv",
		.id = "902",
		.pop = "fixture",
		.url = "https://obs-sg.line-apps.com/r/talk/emv/video-object"
	};
	static const guint8 material[32] = { 5 };

	g_autofree char *key = g_base64_encode(material, sizeof(material));

	g_autoptr(J) payload = obj();
	g_autoptr(GError) error = NULL;

	json_object_object_add(payload, "keyMaterial",
			       json_object_new_string(key));
	if (sealed_oid) {
		route.oid = "sealed-object";
		route.url = NULL;
		json_object_object_add(payload, "OID",
				       json_object_new_string(route.oid));
	}
	J *chunks =
		line_seal_payload(&s, GROUP, LINE_MEDIA_VIDEO, payload, &error);

	g_assert_no_error(error);
	g_assert_nonnull(chunks);
	g_autoptr(J) msg = obj();
	J *meta = obj();

	json_object_object_add(msg, "1", json_object_new_string(ME));
	json_object_object_add(msg, "2", json_object_new_string(GROUP));
	json_object_object_add(msg, "4", json_object_new_string(route.id));
	json_object_object_add(msg, "15",
			       json_object_new_int(LINE_MEDIA_VIDEO));
	json_object_object_add(msg, "18", meta);
	json_object_object_add(msg, "20", chunks);
	json_object_object_add(meta, "OBS_POP",
			       json_object_new_string(route.pop));
	if (route.url)
		json_object_object_add(meta, "DOWNLOAD_URL",
				       json_object_new_string(route.url));
	g_autoptr(GBytes) mp4 = unhex(MP4);
	g_autoptr(GBytes) hashes = NULL;
	g_autoptr(GBytes) encrypted =
		line_media_encrypt(g_bytes_ref(mp4), material, &hashes, &error);

	g_assert_no_error(error);
	route.reply = encrypted;
	video_route = &route;
	g_autoptr(GBytes) video = line_download_video(&s, msg, &error);

	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(video, mp4));

	/* A sealed message must authenticate a response before checking its type. */
	route.reply = mp4;
	g_assert_null(line_download_video(&s, msg, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);

	if (sealed_oid) {
		int previous = download_count;

		json_object_object_add(meta, "OID",
				       json_object_new_string("wrong"));
		g_assert_null(line_download_video(&s, msg, &error));
		g_assert_error(error, LINE_ERROR, 1);
		g_assert_cmpint(download_count, ==, previous);
		g_clear_error(&error);
	}

	/* Public route metadata may not relabel a sealed message as plaintext. */
	json_object_object_add(meta, "SID", json_object_new_string("m"));
	int previous = download_count;

	g_assert_null(line_download_video(&s, msg, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpint(download_count, ==, previous);
	video_route = NULL;
	g_clear_pointer(&s.history_secrets, g_array_unref);
	json_object_put(s.keys);
}

static void square_attachment_upload(gconstpointer is_file)
{
	Line s = { .obs_token = g_strdup("stale-token"),
		   .obs_until = G_MAXINT64 };
	g_autoptr(GError) error = NULL;
	g_autofree char *path = NULL;

	square_file_upload = is_file != NULL;
	int fd = g_file_open_tmp(square_file_upload ?
					 "avernella-sq-XXXXXX.zip" :
					 "avernella-sq-XXXXXX.png",
				 &path, &error);

	g_assert_no_error(error);
	g_assert_cmpint(fd, >=, 0);
	static const guint8 png[16] = { 0x89, 'P',  'N', 'G', '\r', '\n',
					0x1a, '\n', 0,	 0,   0,    0,
					0,    0,    0,	 0 };
	static const guint8 zip[] = { 'P', 'K', 3, 4, 0, 0, 0, 0 };
	const guint8 *bytes = square_file_upload ? zip : png;
	size_t n = square_file_upload ? sizeof(zip) : sizeof(png);

	g_assert_cmpint(write(fd, bytes, n), ==, n);
	close(fd);

	token_count = 0;
	square_upload_count = 0;
	stale_token = TRUE;
	g_autoptr(J) receipt = line_send_square_attachment(
		&s, "m00000000000000000000000000000001",
		"p00000000000000000000000000000001", path,
		square_file_upload ? LINE_MEDIA_FILE : LINE_MEDIA_IMAGE, 77,
		&error);
	unlink(path);
	g_assert_no_error(error);
	g_assert_nonnull(receipt);
	g_assert_cmpstr(strfield(receipt, "id"), ==, "609999999999");
	g_assert_cmpint(square_upload_count, ==, 2);
	g_assert_cmpint(token_count, ==, 2);
	g_free(s.obs_token);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/media/legacy-no-sealed-fallback", legacy);
	g_test_add_func("/media/video-stale-obs-token-refresh", token_refresh);
	g_test_add_func("/media/video-talk-and-square-routes", video_routes);
	g_test_add_func("/media/video-container-validation", video_validate);
	g_test_add_data_func("/media/video-sealed-url", NULL,
			     sealed_video_routing);
	g_test_add_data_func("/media/video-sealed-oid", GINT_TO_POINTER(1),
			     sealed_video_routing);
	g_test_add_data_func("/media/square-image-v1-upload", NULL,
			     square_attachment_upload);
	g_test_add_data_func("/media/square-file-v1-upload", GINT_TO_POINTER(1),
			     square_attachment_upload);
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
