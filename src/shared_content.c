// SPDX-License-Identifier: GPL-3.0-or-later
#include "shared_content.h"
#include <curl/curl.h>
#include <string.h>

enum channel {
	NOTE,
	ALBUM
};

struct line_shared {
	char *tokens[2], *chat, *album;
	GHashTable *albums, *photos;
};

struct response {
	GByteArray *data;
	gsize limit;
};

static const char origin[] = LINE_RPC_ORIGIN;

static gpointer fail(GError **error, const char *reason)
{
	g_set_error_literal(error, LINE_ERROR, 1, reason);
	return NULL;
}

static gboolean plain(const char *s, size_t max)
{
	if (!s || !*s || strlen(s) > max)
		return FALSE;
	for (; *s; s++)
		if ((guint8)*s < 33 || (guint8)*s > 126)
			return FALSE;
	return TRUE;
}

static const char *value(J *o, const char *name)
{
	J *v = field(o, name);

	if (json_object_is_type(v, json_type_int))
		return json_object_get_string(v);
	const char *s = strfield(o, name);

	return s && strlen(s) == (size_t)json_object_get_string_len(v) &&
			       g_utf8_validate(s, -1, NULL) ?
		       s :
		       NULL;
}

static gboolean chat_id(const char *id)
{
	if (!id || !strchr("ucr", *id) || strlen(id) != 33)
		return FALSE;
	for (id++; *id; id++)
		if (!g_ascii_isxdigit(*id))
			return FALSE;
	return TRUE;
}

static gboolean segment(const char *s)
{
	if (!plain(s, 256) || !strcmp(s, ".") || !strcmp(s, ".."))
		return FALSE;
	for (; *s; s++)
		if (!g_ascii_isalnum(*s) && !strchr("-_.", *s))
			return FALSE;
	return TRUE;
}

void line_shared_clear(Line *s)
{
	struct line_shared *c = g_steal_pointer(&s->shared);

	if (!c)
		return;
	line_secret_free(c->tokens[NOTE]);
	line_secret_free(c->tokens[ALBUM]);
	g_free(c->chat);
	g_free(c->album);
	g_hash_table_unref(c->albums);
	g_hash_table_unref(c->photos);
	g_free(c);
}

static struct line_shared *scope(Line *s, const char *chat)
{
	if (!s->shared) {
		s->shared = g_new0(struct line_shared, 1);
		s->shared->albums = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
		s->shared->photos = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
	}
	struct line_shared *c = s->shared;

	if (g_strcmp0(chat, c->chat)) {
		g_free(c->chat);
		c->chat = g_strdup(chat);
		g_clear_pointer(&c->album, g_free);
		g_hash_table_remove_all(c->albums);
		g_hash_table_remove_all(c->photos);
	}
	return c;
}

static const char *channel_token(Line *s, enum channel index, GError **e)
{
	char **cached = &s->shared->tokens[index];

	if (*cached)
		return *cached;
	g_autoptr(J) args = obj();
	const char *id = index == NOTE ? "1341209850" : "1375220249";

	json_object_object_add(args, "1:str", json_object_new_string(id));
	g_autoptr(J) r = line_rpc(
		s, "/CH4", "approveChannelAndIssueChannelToken", args, e);
	if (!r)
		return NULL;
	const char *token = value(r, "5");

	if (!plain(token, 16384))
		return fail(
			e,
			"LINE did not return a shared-content channel token.");
	*cached = g_strdup(token);
	return *cached;
}

static size_t receive(char *data, size_t size, size_t count, void *user)
{
	struct response *r = user;

	if (size && count > (r->limit - r->data->len) / size)
		return 0;
	g_byte_array_append(r->data, (guint8 *)data, size * count);
	return size * count;
}

static GBytes *transfer(Line *s, enum channel channel, const char *path,
			const char *logical_method, J *body,
			const char *chat_header, const char *album, GError **e)
{
	const char *token = channel_token(s, channel, e);

	if (!token)
		return NULL;
	if (!plain(s->access, 16384) || !chat_id(s->mid))
		return fail(e, "The LINE session is unavailable.");
	g_autofree char *url = g_strconcat(origin, path, NULL);

	/* Albums and notes share legy's cooldown with every RPC. */
	if (line_cooldown_check(url, e))
		return NULL;
	g_autoptr(LineSecret) auth =
		g_strconcat("x-line-access: ", s->access, NULL);
	g_autoptr(LineSecret) ch =
		g_strconcat("x-line-channeltoken: ", token, NULL);
	g_autofree char *mid =
		g_strconcat("x-line-mid: ", album ? chat_header : s->mid, NULL);
	g_autofree char *room =
		chat_header && !album ?
			g_strconcat("x-line-chat-id: ", chat_header, NULL) :
			NULL;
	g_autofree char *album_header =
		album ? g_strconcat("x-line-album: ", album, NULL) : NULL;
	g_autofree char *method_header =
		g_strconcat("x-lhm: ", logical_method, NULL);
	char tag[17];

	line_language_tag(tag);
	g_autofree char *language = g_strconcat("x-lal: ", tag, NULL);
	const char *const headers[] = {
		auth,
		ch,
		mid,
		method_header,
		LINE_APP_HEADER,
		"accept: application/json",
		"content-type: application/json; charset=UTF-8",
		"x-lpv: 1",
		language,
		room,
		album_header
	};
	struct curl_slist *list = NULL;
	CURL *curl = curl_easy_init();
	struct response r = { .data = g_byte_array_new(),
			      .limit = album ? 32u << 20 : 4u << 20 };
	CURLcode rc = CURLE_FAILED_INIT;
	long status = 0;

	if (!curl)
		goto out;
	for (size_t i = 0; i < G_N_ELEMENTS(headers); i++)
		if (headers[i]) {
			struct curl_slist *next =
				curl_slist_append(list, headers[i]);
			if (!next) {
				rc = CURLE_OUT_OF_MEMORY;
				goto out;
			}
			list = next;
		}
#define SET(opt, v)                                  \
	do {                                         \
		rc = curl_easy_setopt(curl, opt, v); \
		if (rc)                              \
			goto out;                    \
	} while (0)
	if (!line_curl_tune(curl, TRUE))
		goto out;
	SET(CURLOPT_URL, url);
	SET(CURLOPT_HTTPHEADER, list);
	SET(CURLOPT_FOLLOWLOCATION, 0L);
	SET(CURLOPT_PROTOCOLS_STR, "https");
	SET(CURLOPT_CONNECTTIMEOUT, 8L);
	SET(CURLOPT_TIMEOUT, 30L);
	SET(CURLOPT_NOSIGNAL, 1L);
	SET(CURLOPT_ACCEPT_ENCODING, "");
	SET(CURLOPT_USERAGENT, LINE_USER_AGENT);
	SET(CURLOPT_POSTFIELDS, body ? json_object_to_json_string_ext(
					       body, JSON_C_TO_STRING_PLAIN) :
				       "");
	SET(CURLOPT_WRITEFUNCTION, receive);
	SET(CURLOPT_WRITEDATA, &r);
#undef SET
	rc = curl_easy_perform(curl);
	if (!rc)
		rc = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (!rc)
		line_cooldown_note(url, curl, status);
out:
	curl_easy_cleanup(curl);
	line_curl_headers_free(list);
	if (!rc && status >= 200 && status < 300)
		return g_byte_array_free_to_bytes(r.data);
	g_byte_array_unref(r.data);
	if (status == 401 || status == 403)
		line_secret_set(&s->shared->tokens[channel], NULL);
	if (rc)
		g_set_error(e, LINE_ERROR, 1,
			    "Shared-content transfer failed: %s.",
			    curl_easy_strerror(rc));
	else
		g_set_error(e, LINE_ERROR,
			    status == 429 || status == 503 ? LINE_RATE_LIMITED :
							     1,
			    "LINE shared content returned HTTP %ld.", status);
	return NULL;
}

static J *request(Line *s, enum channel channel, const char *path,
		  const char *logical_method, J *body, const char *chat,
		  GError **e)
{
	g_autoptr(GBytes) bytes =
		transfer(s, channel, path, logical_method, body, chat, NULL, e);
	if (!bytes)
		return NULL;
	gsize n;
	const char *p = g_bytes_get_data(bytes, &n);
	struct json_tokener *parser = json_tokener_new();

	json_tokener_set_flags(parser, JSON_TOKENER_STRICT |
					       JSON_TOKENER_VALIDATE_UTF8);
	g_autoptr(J) raw = json_tokener_parse_ex(parser, p, (int)n);
	gboolean ok = json_tokener_get_error(parser) == json_tokener_success;

	json_tokener_free(parser);
	if (!ok || !json_object_is_type(raw, json_type_object) ||
	    !json_object_is_type(field(raw, "code"), json_type_int))
		return fail(
			e,
			"LINE returned an unreadable shared-content response.");
	if (json_object_get_int64(field(raw, "code")) != 0) {
		const char *msg = value(raw, "message");

		g_set_error(e, LINE_ERROR, 1, "LINE shared content: %.250s",
			    msg ? msg : "Request declined.");
		return NULL;
	}
	J *result = field(raw, "result");

	if (!json_object_is_type(result, json_type_object))
		return fail(e,
			    "LINE did not confirm the shared-content request.");
	return json_object_get(result);
}

static J *item(J *items, const char *id, const char *title, const char *text)
{
	J *o = obj();

	json_object_object_add(o, "id", json_object_new_string(id));
	json_object_object_add(o, "title",
			       json_object_new_string(title ? title : ""));
	json_object_object_add(o, "text",
			       json_object_new_string(text ? text : ""));
	json_object_array_add(items, o);
	return o;
}

static J *page(J *items, const char *next)
{
	J *out = obj();

	json_object_object_add(out, "items", items);
	if (plain(next, 2048))
		json_object_object_add(out, "next",
				       json_object_new_string(next));
	return out;
}

static J *albums(Line *s, const char *chat, const char *cursor, GError **e)
{
	g_autofree char *esc =
		g_uri_escape_string(cursor ? cursor : "", NULL, FALSE);
	g_autofree char *path = g_strdup_printf(
		"/ext/album/moa/v2/albums?cursor=%s&orderBy=createTimeDesc&include=",
		esc);
	g_autoptr(J) reply = request(s, ALBUM, path, "GET", NULL, NULL, e);
	if (!reply)
		return NULL;
	J *rows = field(reply, "albums");

	if (!json_object_is_type(rows, json_type_array))
		return fail(e, "LINE did not return an album list.");
	struct line_shared *c = s->shared;

	if (!cursor)
		g_hash_table_remove_all(c->albums);
	J *items = json_object_new_array();

	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		const char *id = value(r, "albumId"),
			   *count = value(r, "photoCount");
		if (!segment(id) || g_strcmp0(chat, value(r, "chatId")) ||
		    g_hash_table_size(c->albums) >= 4096)
			continue;
		g_hash_table_add(c->albums, g_strdup(id));
		g_autofree char *text =
			count ? g_strdup_printf("%s photos", count) : NULL;
		item(items, id, value(r, "title"), text);
	}
	const char *next = value(reply, "nextCursor");

	if (!next && json_object_get_boolean(field(reply, "hasMore")))
		next = value(reply, "cursor");
	return page(items, g_strcmp0(next, cursor) ? next : NULL);
}

static J *photos(Line *s, const char *chat, const char *album,
		 const char *cursor, GError **e)
{
	struct line_shared *c = s->shared;

	if (!segment(album) || !g_hash_table_contains(c->albums, album))
		return fail(e, "Choose an album from this chat.");
	g_autofree char *esc =
		g_uri_escape_string(cursor ? cursor : "", NULL, FALSE);
	g_autofree char *path = g_strdup_printf(
		"/ext/album/api/v6/albums/%s/photos?cursor=%s&pageSize=50&orderBy=createTimeDesc&include=all&filterType=",
		album, esc);
	g_autoptr(J) reply = request(s, ALBUM, path, "GET", NULL, chat, e);
	if (!reply)
		return NULL;
	J *rows = field(reply, "photos");

	if (!json_object_is_type(rows, json_type_array))
		return fail(e, "LINE did not return photos for this album.");
	if (!cursor || g_strcmp0(c->album, album)) {
		g_hash_table_remove_all(c->photos);
		g_free(c->album);
		c->album = g_strdup(album);
	}
	J *items = json_object_new_array();

	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i),
		  *resource = field(r, "obsResourceId");
		const char *id = value(resource, "oid");

		if (!id)
			id = value(r, "oid");
		if (!segment(id) || g_hash_table_size(c->photos) >= 8192)
			continue;
		gboolean video =
			!g_strcmp0(value(resource, "sid"), "v") ||
			!g_ascii_strcasecmp(value(r, "resourceType") ?
						    value(r, "resourceType") :
						    "",
					    "video");
		if (!video)
			g_hash_table_add(c->photos, g_strdup(id));
		const char *number = value(r, "photoId");

		if (!number)
			number = value(r, "id");
		g_autofree char *name = g_strdup_printf(
			"%s%s%s", video ? "Video" : "Photo", number ? " " : "",
			number ? number : "");
		int width = json_object_get_int(field(r, "width")),
		    height = json_object_get_int(field(r, "height"));
		g_autofree char *desc =
			width > 0 && height > 0 ?
				g_strdup_printf("%d × %d", width, height) :
				NULL;
		J *it = item(items, id, name, desc);

		if (number && segment(number))
			json_object_object_add(it, "photoId",
					       json_object_new_string(number));
		json_object_object_add(it, "viewable",
				       json_object_new_boolean(!video));
	}
	const char *next = value(reply, "nextCursor");

	return page(items, g_strcmp0(next, cursor) ? next : NULL);
}

GBytes *line_shared_photo(Line *s, J *params, GError **e)
{
	const char *chat = value(params, "chat"),
		   *album = value(params, "album"), *id = value(params, "id");
	struct line_shared *c = s->shared;

	if (!c || !chat_id(chat) || g_strcmp0(chat, c->chat) ||
	    !segment(album) || g_strcmp0(c->album, album) || !segment(id) ||
	    !g_hash_table_contains(c->photos, id))
		return fail(e, "Choose a displayed photo from this chat.");
	g_autofree char *path = g_strdup_printf("/oa/r/album/a/%s", id);

	return transfer(s, ALBUM, path, "GET", NULL, chat, album, e);
}

static J *create_note(Line *s, const char *chat, J *p, GError **e)
{
	const char *text = value(p, "text");

	if (!strfield(p, "text") || !text || !*text || strlen(text) > 10000)
		return fail(e, "Enter a note of up to 10,000 UTF-8 bytes.");
	g_autoptr(J) body = json_tokener_parse(
		"{\"postInfo\":{\"readPermission\":{\"type\":\"GROUP\",\"gids\":[]}},\"contents\":{\"contentsStyle\":{\"textStyle\":{\"textSizeMode\":\"NORMAL\",\"backgroundColor\":\"#FFFFFF\",\"textAnimation\":\"NONE\"},\"mediaStyle\":{\"displayType\":\"GRID_1_A\"}},\"media\":[],\"stickers\":[],\"locations\":[]}}");
	json_object_array_add(
		field(field(field(body, "postInfo"), "readPermission"), "gids"),
		json_object_new_string(chat));
	json_object_object_add(field(body, "contents"), "text",
			       json_object_new_string(text));
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/post/create.json?homeId=%s&sourceType=TALKROOM",
		chat);
	g_autoptr(J) reply = request(s, NOTE, path, "POST", body, NULL, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "saved", json_object_new_boolean(TRUE));
	return ack;
}

static J *update_note(Line *s, const char *chat, J *p, GError **e)
{
	const char *id = value(p, "id"), *text = value(p, "text");

	if (!segment(id) || !strfield(p, "text") || !text || !*text ||
	    strlen(text) > 10000)
		return fail(e, "Enter a note of up to 10,000 UTF-8 bytes.");
	g_autoptr(J) body = json_tokener_parse(
		"{\"postInfo\":{\"readPermission\":{\"type\":\"GROUP\",\"gids\":[]}},\"contents\":{\"contentsStyle\":{\"textStyle\":{\"textSizeMode\":\"NORMAL\",\"backgroundColor\":\"#FFFFFF\",\"textAnimation\":\"NONE\"},\"mediaStyle\":{\"displayType\":\"GRID_1_A\"}},\"media\":[],\"stickers\":[],\"locations\":[]}}");
	json_object_object_add(field(body, "postInfo"), "postId",
			       json_object_new_string(id));
	json_object_array_add(
		field(field(field(body, "postInfo"), "readPermission"), "gids"),
		json_object_new_string(chat));
	json_object_object_add(field(body, "contents"), "text",
			       json_object_new_string(text));
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/post/update.json?homeId=%s&sourceType=TALKROOM",
		chat);
	g_autoptr(J) reply = request(s, NOTE, path, "POST", body, NULL, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "saved", json_object_new_boolean(TRUE));
	return ack;
}

static J *delete_note(Line *s, const char *chat, J *p, GError **e)
{
	const char *id = value(p, "id");

	if (!segment(id))
		return fail(e, "Choose a group note to delete.");
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/post/delete.json?homeId=%s&postId=%s",
		chat, id);
	g_autoptr(J) reply = request(s, NOTE, path, "GET", NULL, NULL, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "deleted", json_object_new_boolean(TRUE));
	return ack;
}

static J *toggle_note_like(Line *s, const char *chat, J *p, GError **e)
{
	const char *id = value(p, "id"), *actor = value(p, "actorId");
	gboolean like = json_object_get_boolean(field(p, "like"));

	if (!segment(id) || !chat_id(actor))
		return fail(e, "Choose a group note to react to.");
	g_autofree char *path = NULL;

	g_autoptr(J) body = NULL;

	if (like) {
		path = g_strdup_printf(
			"/ext/note/nt/api/v57/like/create.json?homeId=%s",
			chat);
		body = obj();
		json_object_object_add(body, "contentId",
				       json_object_new_string(id));
		json_object_object_add(body, "actorId",
				       json_object_new_string(actor));
		json_object_object_add(body, "likeType",
				       json_object_new_string("1001"));
	} else {
		path = g_strdup_printf(
			"/ext/note/nt/api/v57/like/cancel.json?homeId=%s&contentId=%s&actorId=%s",
			chat, id, actor);
	}
	g_autoptr(J) reply =
		request(s, NOTE, path, like ? "POST" : "GET", body, NULL, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "liked", json_object_new_boolean(like));
	return ack;
}

static J *note_comments(Line *s, const char *chat, J *p, GError **e)
{
	const char *id = value(p, "id"), *actor = value(p, "actorId");

	if (!segment(id) || !chat_id(actor))
		return fail(e, "Choose a group note.");
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/comment/getList.json?homeId=%s&contentId=%s&actorId=%s&limit=50",
		chat, id, actor);
	g_autoptr(J) reply = request(s, NOTE, path, "GET", NULL, NULL, e);
	if (!reply)
		return NULL;
	J *list = field(reply, "commentList");

	if (!list)
		list = field(reply, "comments");
	J *items = json_object_new_array();

	for (size_t i = 0; json_object_is_type(list, json_type_array) &&
			   i < json_object_array_length(list);
	     i++) {
		J *c = json_object_array_get_idx(list, i);
		J *user = field(c, "userInfo");
		const char *cid = value(c, "commentNo");

		if (!cid)
			cid = value(c, "commentId");
		const char *who = value(user, "displayName");
		const char *mid = value(user, "mid");
		const char *text = value(c, "commentText");

		if (!cid || !segment(cid))
			continue;
		J *it = item(items, cid, who ? who : "Member",
			     text ? text : "");

		json_object_object_add(it, "own",
				       json_object_new_boolean(
					       mid && !g_strcmp0(mid, s->mid)));
	}
	return page(items, NULL);
}

static J *post_comment(Line *s, const char *chat, J *p, GError **e)
{
	const char *id = value(p, "id"), *actor = value(p, "actorId"),
		   *text = value(p, "text");

	if (!segment(id) || !chat_id(actor) || !strfield(p, "text") || !text ||
	    !*text || strlen(text) > 2000)
		return fail(e, "Enter a comment of up to 2,000 UTF-8 bytes.");
	g_autoptr(J) body = obj();
	json_object_object_add(body, "contentId", json_object_new_string(id));
	json_object_object_add(body, "actorId", json_object_new_string(actor));
	json_object_object_add(body, "commentText",
			       json_object_new_string(text));
	json_object_object_add(body, "contentsList", json_object_new_array());
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/comment/create.json?homeId=%s", chat);
	g_autoptr(J) reply = request(s, NOTE, path, "POST", body, NULL, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "saved", json_object_new_boolean(TRUE));
	return ack;
}

static J *delete_comment(Line *s, const char *chat, J *p, GError **e)
{
	const char *cid = value(p, "commentId"), *actor = value(p, "actorId");

	if (!segment(cid) || !chat_id(actor))
		return fail(e, "Choose a comment to delete.");
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/comment/delete.json?homeId=%s&commentId=%s&actorId=%s",
		chat, cid, actor);
	g_autoptr(J) reply = request(s, NOTE, path, "GET", NULL, NULL, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "deleted", json_object_new_boolean(TRUE));
	return ack;
}

static J *create_album(Line *s, const char *chat, J *p, GError **e)
{
	const char *title = value(p, "title");

	if (!strfield(p, "title") || !title || !*title || strlen(title) > 100)
		return fail(e, "Enter an album title of up to 100 bytes.");
	g_autoptr(J) body = obj();
	json_object_object_add(body, "title", json_object_new_string(title));
	g_autoptr(J) reply = request(
		s, ALBUM,
		"/ext/album/api/v6/albums/create?modifyDuplicateTitle=true",
		"POST", body, chat, e);
	if (!reply)
		return NULL;
	const char *id = value(reply, "id");

	if (!id)
		id = value(reply, "albumId");
	if (id && segment(id))
		g_hash_table_add(s->shared->albums, g_strdup(id));
	J *ack = obj();

	json_object_object_add(ack, "saved", json_object_new_boolean(TRUE));
	return ack;
}

static J *rename_album(Line *s, const char *chat, J *p, GError **e)
{
	const char *album = value(p, "album"), *title = value(p, "title");

	if (!segment(album) ||
	    !g_hash_table_contains(s->shared->albums, album) ||
	    !strfield(p, "title") || !title || !*title || strlen(title) > 100)
		return fail(e, "Choose an album and enter a new title.");
	g_autoptr(J) body = obj();
	json_object_object_add(body, "title", json_object_new_string(title));
	g_autofree char *path =
		g_strdup_printf("/ext/album/api/v6/albums/%s/update", album);
	g_autoptr(J) reply = request(s, ALBUM, path, "POST", body, chat, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "saved", json_object_new_boolean(TRUE));
	return ack;
}

static J *delete_album(Line *s, const char *chat, J *p, GError **e)
{
	const char *album = value(p, "album");

	if (!segment(album) || !g_hash_table_contains(s->shared->albums, album))
		return fail(e, "Choose an album from this chat.");
	g_autoptr(J) body = obj();
	g_autofree char *path =
		g_strdup_printf("/ext/album/api/v6/albums/%s/delete", album);
	g_autoptr(J) reply = request(s, ALBUM, path, "POST", body, chat, e);
	if (!reply)
		return NULL;
	g_hash_table_remove(s->shared->albums, album);
	J *ack = obj();

	json_object_object_add(ack, "deleted", json_object_new_boolean(TRUE));
	return ack;
}

static J *delete_album_photo(Line *s, const char *chat, J *p, GError **e)
{
	const char *album = value(p, "album"), *pid = value(p, "photoId");

	if (!segment(album) ||
	    !g_hash_table_contains(s->shared->albums, album) || !segment(pid))
		return fail(e, "Choose a photo from this album.");
	g_autoptr(J) body = obj();
	J *ids = json_object_new_array();

	json_object_array_add(ids, json_object_new_string(pid));
	json_object_object_add(body, "photoIds", ids);
	g_autofree char *path = g_strdup_printf(
		"/ext/album/api/v6/albums/%s/photos/delete", album);
	g_autoptr(J) reply = request(s, ALBUM, path, "POST", body, chat, e);
	if (!reply)
		return NULL;
	J *ack = obj();

	json_object_object_add(ack, "deleted", json_object_new_boolean(TRUE));
	return ack;
}

static J *notes(Line *s, const char *chat, const char *cursor, gboolean polls,
		GError **e)
{
	g_autofree char *path = NULL;

	g_autoptr(J) query = NULL;
	if (polls) {
		path = g_strdup("/mh/api/v46/search/note.json");
		query = json_tokener_parse(
			"{\"query\":\"poll\",\"queryType\":\"TEXT\",\"postLimit\":20,\"commandId\":16,\"channelId\":\"1341209850\",\"commandType\":188259}");
		json_object_object_add(query, "homeId",
				       json_object_new_string(chat));
	} else {
		g_autofree char *tail = NULL;

		if (cursor) {
			g_auto(GStrv) bits = g_strsplit(cursor, ":", 2);
			guint64 time;

			if (!segment(bits[0]) || !bits[1] ||
			    !g_ascii_string_to_unsigned(
				    bits[1], 10, 1, G_MAXINT64, &time, NULL))
				return fail(e, "The note page has expired.");
			tail = g_strdup_printf(
				"&postId=%s&updatedTime=%" G_GUINT64_FORMAT,
				bits[0], time);
		}
		path = g_strdup_printf(
			"/ext/note/nt/api/v57/post/list.json?homeId=%s&sourceType=TALKROOM%s",
			chat, tail ? tail : "");
	}
	g_autoptr(J) reply =
		request(s, NOTE, path, polls ? "POST" : "GET", query, NULL, e);
	if (!reply)
		return NULL;
	J *rows = field(reply, "posts");

	if (!rows)
		rows = field(reply, "feeds");
	if (!json_object_is_type(rows, json_type_array))
		return fail(e, "LINE did not return notes for this group.");
	J *items = json_object_new_array();
	g_autofree char *next = NULL;

	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);

		if (field(r, "post"))
			r = field(r, "post");
		J *info = field(r, "postInfo"), *user = field(r, "userInfo");
		const char *id = value(info, "postId"),
			   *text = value(field(r, "contents"), "text"),
			   *from = value(user, "displayName");
		if (!id)
			id = value(r, "postId");
		if (!id)
			id = value(r, "id");
		if (!text)
			text = value(r, "text");
		if (!segment(id))
			continue;
		J *it = item(items, id,
			     from ? from : (polls ? "Poll note" : "Group note"),
			     text && *text ? text : "Media or shared content");
		const char *author = value(user, "mid");

		if (author && chat_id(author))
			json_object_object_add(it, "authorMid",
					       json_object_new_string(author));
		json_object_object_add(
			it, "own",
			json_object_new_boolean(author &&
						!g_strcmp0(author, s->mid)));
		json_object_object_add(
			it, "liked",
			json_object_new_boolean(
				json_object_get_boolean(field(info, "liked"))));
		json_object_object_add(
			it, "likeCount",
			json_object_new_int64(json_object_get_int64(
				field(info, "likeCount"))));
		json_object_object_add(
			it, "commentCount",
			json_object_new_int64(json_object_get_int64(
				field(info, "commentCount"))));
		if (i + 1 == json_object_array_length(rows) && !polls) {
			gint64 time = json_object_get_int64(
				field(info, "updatedTime"));
			if (time > 0)
				next = g_strdup_printf("%s:%" G_GINT64_FORMAT,
						       id, time);
		}
	}
	return page(items, g_strcmp0(next, cursor) ? next : NULL);
}

J *line_shared_content(Line *s, const char *method, J *p, GError **e)
{
	const char *chat = value(p, "chat"), *kind = value(p, "kind"),
		   *cursor = value(p, "cursor");
	if (!chat_id(chat) || (field(p, "cursor") && (!strfield(p, "cursor") ||
						      !plain(cursor, 2048))))
		return fail(e, "Choose an available conversation and page.");
	scope(s, chat);
	if (!strcmp(method, "sharedPhotos"))
		return photos(s, chat, value(p, "album"), cursor, e);
	if (!strcmp(method, "sharedCreateAlbum"))
		return create_album(s, chat, p, e);
	if (!strcmp(method, "sharedRenameAlbum"))
		return rename_album(s, chat, p, e);
	if (!strcmp(method, "sharedDeleteAlbum"))
		return delete_album(s, chat, p, e);
	if (!strcmp(method, "sharedDeletePhoto"))
		return delete_album_photo(s, chat, p, e);
	if (!strcmp(method, "sharedList") && !g_strcmp0(kind, "albums"))
		return albums(s, chat, cursor, e);
	if (*chat != 'c')
		return fail(e, "Open a group conversation to use group notes.");
	if (!strcmp(method, "sharedPost"))
		return create_note(s, chat, p, e);
	if (!strcmp(method, "sharedUpdateNote"))
		return update_note(s, chat, p, e);
	if (!strcmp(method, "sharedDeleteNote"))
		return delete_note(s, chat, p, e);
	if (!strcmp(method, "sharedToggleLike"))
		return toggle_note_like(s, chat, p, e);
	if (!strcmp(method, "sharedComments"))
		return note_comments(s, chat, p, e);
	if (!strcmp(method, "sharedCommentPost"))
		return post_comment(s, chat, p, e);
	if (!strcmp(method, "sharedCommentDelete"))
		return delete_comment(s, chat, p, e);
	if (!strcmp(method, "sharedList") &&
	    (!g_strcmp0(kind, "notes") || !g_strcmp0(kind, "polls")))
		return notes(s, chat, cursor, !strcmp(kind, "polls"), e);
	return fail(e, "Unknown shared-content operation.");
}
