// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.h"
#include "media_crypto.h"
#include "messaging.h"
#include "obs_http.h"
#include "sealing.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void media_error(GError **error, const char *message)
{
	g_set_error_literal(error, LINE_ERROR, 1, message);
}

static const char *media_namespace(int type)
{
	switch (type) {
	case LINE_MEDIA_IMAGE:
		return "emi";
	case LINE_MEDIA_VIDEO:
		return "emv";
	case LINE_MEDIA_AUDIO:
		return "ema";
	case LINE_MEDIA_FILE:
		return "emf";
	default:
		return NULL;
	}
}

static GBytes *read_attachment(const char *path, GError **error)
{
	if (!path || !*path) {
		media_error(error, "Choose a file to send.");
		return NULL;
	}
	/* A named pipe must not block the worker before fstat can reject it. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);

	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			    "Could not open the attachment: %s.",
			    g_strerror(errno));
		return NULL;
	}
	struct stat st;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > LINE_MEDIA_LIMIT) {
		close(fd);
		media_error(error,
			    "Choose a nonempty regular file of up to 64 MiB.");
		return NULL;
	}
	gsize size = (gsize)st.st_size;

	g_autofree guint8 *buf = g_malloc(size);
	gsize used = 0;

	while (used < size) {
		ssize_t n = read(fd, buf + used, size - used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (gsize)n;
	}
	guint8 trailing;
	ssize_t extra;

	do {
		extra = read(fd, &trailing, 1);
	} while (extra < 0 && errno == EINTR);
	close(fd);
	if (used != size || extra != 0) {
		media_error(
			error,
			"The attachment changed or could not be read completely.");
		return NULL;
	}
	return g_bytes_new_take(g_steal_pointer(&buf), size);
}

static const char *image_format(GBytes *bytes)
{
	gsize size;
	const guint8 *data = g_bytes_get_data(bytes, &size);

	if (size >= 8 && !memcmp(data, "\211PNG\r\n\032\n", 8))
		return "png";
	if (size >= 3 && !memcmp(data, "\xff\xd8\xff", 3))
		return "jpg";
	if (size >= 6 &&
	    (!memcmp(data, "GIF89a", 6) || !memcmp(data, "GIF87a", 6)))
		return "gif";
	if (size >= 12 && !memcmp(data, "RIFF", 4) &&
	    !memcmp(data + 8, "WEBP", 4))
		return "webp";
	return NULL;
}

static gboolean safe_filename(const char *name)
{
	if (!name || !*name || !g_utf8_validate(name, -1, NULL) ||
	    strlen(name) > 255 || !strcmp(name, ".") || !strcmp(name, "..") ||
	    strpbrk(name, "/\\"))
		return FALSE;
	for (const char *p = name; *p; p = g_utf8_next_char(p))
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			return FALSE;
	return TRUE;
}

static J *image_info(GBytes *image, const char *format)
{
	gsize n;
	const guint8 *p = g_bytes_get_data(image, &n);
	J *info = obj();

	json_object_object_add(info, "category",
			       json_object_new_string("original"));
	json_object_object_add(info, "fileSize",
			       json_object_new_int64((gint64)n));
	json_object_object_add(info, "extension",
			       json_object_new_string(format));
	if (!strcmp(format, "gif"))
		json_object_object_add(info, "animated",
				       json_object_new_boolean(TRUE));
	if (!strcmp(format, "png") && n >= 24 && !memcmp(p + 12, "IHDR", 4)) {
		guint32 width, height;

		memcpy(&width, p + 16, 4);
		memcpy(&height, p + 20, 4);
		json_object_object_add(
			info, "width",
			json_object_new_int64(GUINT32_FROM_BE(width)));
		json_object_object_add(
			info, "height",
			json_object_new_int64(GUINT32_FROM_BE(height)));
	}
	return info;
}

static guint32 load_be32(const guint8 *p)
{
	guint32 value;

	memcpy(&value, p, sizeof(value));
	return GUINT32_FROM_BE(value);
}

static guint64 load_be64(const guint8 *p)
{
	guint64 value;

	memcpy(&value, p, sizeof(value));
	return GUINT64_FROM_BE(value);
}

static const guint8 *find_box(const guint8 *data, gsize *length,
			      const char *fourcc)
{
	gsize remaining = *length;

	for (guint i = 0; remaining >= 8 && i < 65536; i++) {
		guint64 size = load_be32(data);
		gsize header = 8;

		if (size == 1) {
			header = 16;
			if (remaining < header)
				break;
			size = load_be64(data + 8);
		} else if (!size) {
			size = remaining;
		}
		if (size < header || size > remaining)
			break;
		if (!memcmp(data + 4, fourcc, 4)) {
			*length = size - header;
			return data + header;
		}
		data += size;
		remaining -= size;
	}
	return NULL;
}

/* MP4 and QuickTime store the movie timebase in the top-level moov/mvhd. */
static guint64 video_duration(GBytes *video)
{
	gsize len;
	const guint8 *data = g_bytes_get_data(video, &len);
	const guint8 *moov = find_box(data, &len, "moov");
	const guint8 *mvhd = moov ? find_box(moov, &len, "mvhd") : NULL;
	guint64 duration;
	guint32 scale;

	if (mvhd && len >= 20 && mvhd[0] == 0) {
		scale = load_be32(mvhd + 12);
		duration = load_be32(mvhd + 16);
		if (duration == G_MAXUINT32)
			return 0;
	} else if (mvhd && len >= 32 && mvhd[0] == 1) {
		scale = load_be32(mvhd + 20);
		duration = load_be64(mvhd + 24);
		if (duration == G_MAXUINT64)
			return 0;
	} else {
		return 0;
	}
	/* Match the protocol's positive, effectively integral millisecond field. */
	const guint64 limit = (G_GUINT64_CONSTANT(1) << 53) - 1;

	if (!scale || duration / scale > limit / 1000)
		return 0;
	guint64 millis = duration / scale * 1000 +
			 (duration % scale * 1000 + scale / 2) / scale;

	return millis <= limit ? millis : 0;
}

static J *material_payload(const guint8 material[32], int type,
			   const char *basename)
{
	char *b64 = g_base64_encode(material, 32);
	J *payload = obj();

	json_object_object_add(payload, "keyMaterial",
			       json_object_new_string(b64));
	OPENSSL_cleanse(b64, strlen(b64));
	g_free(b64);
	if (type == LINE_MEDIA_FILE)
		json_object_object_add(payload, "fileName",
				       json_object_new_string(basename));
	return payload;
}

/* Consumes its input buffer. */
static J *send_attachment_bytes(Line *s, const char *chat, GBytes *bytes,
				const char *filename, int type, GError **error)
{
	g_autoptr(GBytes) original = bytes;
	const char *sid = media_namespace(type);

	if (!sid || !chat || !*chat || !strchr("uc", chat[0]) || s->seq < 0 ||
	    s->seq >= G_MAXINT32) {
		media_error(
			error,
			"Select a direct or group conversation that supports encrypted attachments.");
		return NULL;
	}
	gsize size = g_bytes_get_size(original);
	guint64 duration = type == LINE_MEDIA_VIDEO ? video_duration(original) :
						      0;
	const char *format = type == LINE_MEDIA_IMAGE ? image_format(original) :
							NULL;
	if (type == LINE_MEDIA_IMAGE && !format) {
		media_error(error, "Choose a JPEG, PNG, GIF or WebP image.");
		return NULL;
	}
	const char *basename = safe_filename(filename) ? filename :
							 "attachment";
	guint8 material[32];

	if (RAND_bytes(material, 32) != 1) {
		media_error(error, "Could not generate an attachment key.");
		return NULL;
	}
	g_autoptr(J) payload = material_payload(material, type, basename);
	/*
	 * Negotiate and seal before sending bytes to OBS. Uploads do not grant
	 * recipients access to either the key or the object until sendMessage.
	 */
	g_autoptr(J) chunks = line_seal_payload(s, chat, type, payload, error);
	if (!chunks) {
		OPENSSL_cleanse(material, 32);
		return NULL;
	}
	g_autoptr(J) info = format ? image_info(original, format) : NULL;
	g_autoptr(GBytes) hashes = NULL;
	g_autoptr(GBytes) encrypted = line_media_encrypt(
		g_steal_pointer(&original), material,
		type == LINE_MEDIA_VIDEO ? &hashes : NULL, error);
	OPENSSL_cleanse(material, 32);
	if (!encrypted)
		return NULL;
	g_autofree char *token = line_obs_token(s, error);

	if (!token)
		return NULL;
	g_autofree char *uuid = g_uuid_string_random();
	g_autofree char *request_id = g_strconcat("reqid-", uuid, NULL);
	const char *extension = format			 ? format :
				type == LINE_MEDIA_VIDEO ? "mp4" :
				type == LINE_MEDIA_AUDIO ? "m4a" :
							   "bin";
	g_autofree char *upload_name =
		g_strdup_printf("%s.%s", uuid, extension);
	g_autofree char *oid = line_obs_upload(token, sid, request_id,
					       upload_name, encrypted, error);
	if (!oid)
		return NULL;
	if (format || hashes) {
		/*
		 * Reuse the original image ciphertext as its own preview. A distinct
		 * plaintext encrypted under this same CTR stream would leak data.
		 */
		g_autofree char *side_id = g_strconcat(
			oid, format ? "__ud-preview" : "__ud-hash", NULL);
		g_autofree char *stored = line_obs_upload(
			token, sid, side_id, format ? "preview.jpg" : "hash",
			format ? encrypted : hashes, error);
		if (!stored)
			return NULL;
	}
	g_autoptr(J) meta = obj();
	g_autofree char *size_str = g_strdup_printf("%zu", size);

	json_object_object_add(meta, "SID", json_object_new_string(sid));
	json_object_object_add(meta, "OID", json_object_new_string(oid));
	json_object_object_add(meta, "FILE_SIZE",
			       json_object_new_string(size_str));
	if (duration) {
		g_autofree char *millis =
			g_strdup_printf("%" G_GUINT64_FORMAT, duration);

		json_object_object_add(meta, "DURATION",
				       json_object_new_string(millis));
	}
	json_object_object_add(meta, "e2eeVersion",
			       json_object_new_string("2"));
	if (info)
		json_object_object_add(
			meta, "MEDIA_CONTENT_INFO",
			json_object_new_string(json_object_to_json_string_ext(
				info, JSON_C_TO_STRING_PLAIN)));
	return line_send_sealed(s, chat, type, meta, chunks, NULL, error);
}

J *line_send_attachment(Line *s, const char *chat, const char *path, int type,
			GError **error)
{
	GBytes *original = read_attachment(path, error);

	if (!original)
		return NULL;
	g_autofree char *basename = g_path_get_basename(path);

	return send_attachment_bytes(s, chat, original, basename, type, error);
}

J *line_forward_attachment(Line *s, const char *chat, J *message,
			   GError **error)
{
	int type = json_object_get_int(field(message, "15"));
	g_autofree char *name = NULL;

	if (type == LINE_MEDIA_FILE) {
		name = line_attachment_filename(s, message, error);
		if (!name)
			return NULL;
	}
	GBytes *bytes = line_download_attachment(s, message, error);

	if (!bytes)
		return NULL;
	return send_attachment_bytes(s, chat, bytes, name, type, error);
}

J *line_send_image(Line *s, const char *chat, const char *path, GError **error)
{
	return line_send_attachment(s, chat, path, LINE_MEDIA_IMAGE, error);
}

J *line_send_file(Line *s, const char *chat, const char *path, GError **error)
{
	return line_send_attachment(s, chat, path, LINE_MEDIA_FILE, error);
}

static J *open_attachment(Line *s, J *message, GError **error)
{
	int type = json_object_get_int(field(message, "15"));
	const char *sid = media_namespace(type);
	J *meta = field(message, "18");

	if (!sid || g_strcmp0(strfield(meta, "SID"), sid)) {
		media_error(
			error,
			"This attachment does not use a supported Letter Sealing envelope.");
		return NULL;
	}
	return line_open_payload(s, message, type, error);
}

/*
 * A legacy talk object uses the message ID as its object ID. Select this
 * representation only from the original envelope, before any cryptography.
 */
static gboolean plain_attachment(J *message)
{
	J *v = field(message, "15"), *meta = field(message, "18");

	if (!json_object_is_type(v, json_type_int) ||
	    !media_namespace(json_object_get_int(v)))
		return FALSE;
	J *chunks = NULL;

	if (json_object_object_get_ex(message, "20", &chunks) &&
	    (!json_object_is_type(chunks, json_type_array) ||
	     json_object_array_length(chunks)))
		return FALSE;
	if (json_object_object_get_ex(message, "18", &meta)) {
		if (!json_object_is_type(meta, json_type_object))
			return FALSE;
		json_object_object_foreach(meta, name, value) {
			(void)value;
			if (!g_ascii_strncasecmp(name, "e2ee", 4) ||
			    !g_ascii_strcasecmp(name, "SID") ||
			    !g_ascii_strcasecmp(name, "OID") ||
			    !g_ascii_strcasecmp(name, "keyMaterial"))
				return FALSE;
		}
	}
	const char *id = strfield(message, "4");

	return id && *id && strlen(id) <= 31 &&
	       strlen(id) == (size_t)json_object_get_string_len(
				     field(message, "4")) &&
	       id[strspn(id, "0123456789")] == 0;
}

static GBytes *download_object(Line *s, const char *sid, const char *oid,
			       const char *id, gboolean preview, GError **error)
{
	g_autoptr(GError) failure = NULL;

	for (int i = 0; i < 2; i++) {
		g_autofree char *token = line_obs_token(s, &failure);

		if (!token)
			break;
		GBytes *bytes = preview ? line_obs_download_preview(token, sid,
								    oid, id,
								    &failure) :
					  line_obs_download(token, sid, oid, id,
							    &failure);

		if (bytes)
			return bytes;
		if (i || !g_error_matches(failure, LINE_ERROR,
					  LINE_OBS_UNAUTHORIZED))
			break;
		/* The OBS token may have been invalidated ahead of its local TTL. */
		g_clear_error(&failure);
		g_clear_pointer(&s->obs_token, g_free);
		s->obs_until = 0;
	}
	if (failure)
		g_propagate_error(error, g_steal_pointer(&failure));
	return NULL;
}

static GBytes *download_sealed_object(Line *s, J *message, gboolean preview,
				      GError **error)
{
	g_autoptr(J) payload = open_attachment(s, message, error);
	if (!payload)
		return NULL;
	const char *encoded = strfield(payload, "keyMaterial");

	if (!encoded || strlen(encoded) != 44 ||
	    json_object_get_string_len(field(payload, "keyMaterial")) != 44) {
		media_error(error,
			    "The message has an invalid attachment key.");
		return NULL;
	}
	gsize len;

	g_autofree guint8 *key = g_base64_decode(encoded, &len);
	g_autofree char *roundtrip = g_base64_encode(key, len);
	gboolean valid = len == 32 && !strcmp(encoded, roundtrip);

	OPENSSL_cleanse(roundtrip, strlen(roundtrip));
	g_autoptr(GBytes) result = NULL;
	if (!valid)
		media_error(error,
			    "The message has an invalid attachment key.");
	else {
		J *meta = field(message, "18");
		GBytes *bytes = download_object(s, strfield(meta, "SID"),
						strfield(meta, "OID"),
						strfield(message, "4"), preview,
						error);

		if (bytes)
			result = line_media_decrypt(bytes, key, error);
	}
	OPENSSL_cleanse(key, len);
	return g_steal_pointer(&result);
}

GBytes *line_download_attachment(Line *s, J *message, GError **error)
{
	if (!plain_attachment(message))
		return download_sealed_object(s, message, FALSE, error);
	const char *id = strfield(message, "4");

	g_autoptr(GBytes) bytes = download_object(s, "m", id, id, FALSE, error);

	if (bytes && (!g_bytes_get_size(bytes) ||
		      g_bytes_get_size(bytes) > LINE_MEDIA_LIMIT)) {
		media_error(error, "Unsupported attachment size.");
		return NULL;
	}
	return g_steal_pointer(&bytes);
}

GBytes *line_download_video_preview(Line *s, J *message, GError **error)
{
	if (json_object_get_int(field(message, "15")) != LINE_MEDIA_VIDEO) {
		media_error(error, "The selected message is not a video.");
		return NULL;
	}
	const char *id = strfield(message, "4");

	g_autoptr(GBytes) preview =
		plain_attachment(message) ?
			download_object(s, "m", id, id, TRUE, error) :
			download_sealed_object(s, message, TRUE, error);

	if (preview && (g_bytes_get_size(preview) > LINE_MEDIA_PREVIEW_LIMIT ||
			!image_format(preview))) {
		media_error(error,
			    "LINE has no supported thumbnail for this video.");
		return NULL;
	}
	return g_steal_pointer(&preview);
}

GBytes *line_download_image(Line *s, J *message, GError **error)
{
	if (json_object_get_int(field(message, "15")) != LINE_MEDIA_IMAGE) {
		media_error(error, "The selected message is not an image.");
		return NULL;
	}
	return line_download_attachment(s, message, error);
}

char *line_attachment_filename(Line *s, J *message, GError **error)
{
	if (plain_attachment(message)) {
		J *meta = field(message, "18");
		const char *name = strfield(meta, "FILE_NAME");

		return g_strdup(
			name &&
					strlen(name) ==
						(size_t)json_object_get_string_len(
							field(meta,
							      "FILE_NAME")) &&
					safe_filename(name) ?
				name :
				"attachment");
	}
	g_autoptr(J) payload = open_attachment(s, message, error);
	if (!payload)
		return NULL;
	const char *name = strfield(payload, "fileName");

	if (name &&
	    strlen(name) == (size_t)json_object_get_string_len(
				    field(payload, "fileName")) &&
	    safe_filename(name))
		return g_strdup(name);
	return g_strdup("attachment");
}
