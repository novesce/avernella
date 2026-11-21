// SPDX-License-Identifier: GPL-3.0-or-later
#include "shared_content.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <string.h>

static const char group[] = "c00000000000000000000000000000000";
static const char other[] = "c11111111111111111111111111111111";
static const char self[] = "u00000000000000000000000000000000";
static const char *fixture, *expected_path, *logical_method, *expected_channel;
static const char *url, *upload, *expected_mid, *expected_album, *expected_chat;
static struct curl_slist *headers;
static curl_write_callback writer;
static void *userdata;
static guint count, approvals;
static long status;

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	(void)s;
	(void)error;
	g_assert_cmpstr(path, ==, "/CH4");
	g_assert_cmpstr(method, ==, "approveChannelAndIssueChannelToken");
	g_assert_cmpstr(strfield(args, "1:str"), ==, expected_channel);
	approvals++;
	J *reply = obj();

	json_object_object_add(reply, "5",
			       json_object_new_string(expected_channel));
	return reply;
}

CURLcode __wrap_curl_easy_setopt(CURL *c, CURLoption opt, ...)
{
	(void)c;
	va_list a;

	va_start(a, opt);
	switch (opt) {
	case CURLOPT_URL:
		url = va_arg(a, const char *);
		break;
	case CURLOPT_HTTPHEADER:
		headers = va_arg(a, struct curl_slist *);
		break;
	case CURLOPT_POSTFIELDS:
		upload = va_arg(a, const char *);
		break;
	case CURLOPT_WRITEFUNCTION:
		writer = va_arg(a, curl_write_callback);
		break;
	case CURLOPT_WRITEDATA:
		userdata = va_arg(a, void *);
		break;
	case CURLOPT_FOLLOWLOCATION:
		g_assert_cmpint(va_arg(a, long), ==, 0);
		break;
	default:
		break;
	}
	va_end(a);
	return CURLE_OK;
}

static gboolean has(const char *key, const char *v)
{
	g_autofree char *wanted = g_strconcat(key, ": ", v, NULL);

	for (struct curl_slist *entry = headers; entry; entry = entry->next)
		if (!strcmp(entry->data, wanted))
			return TRUE;
	return FALSE;
}

CURLcode __wrap_curl_easy_perform(CURL *curl)
{
	(void)curl;
	count++;
	g_autofree char *expected =
		g_strconcat("https://legy.line-apps.com", expected_path, NULL);
	g_assert_cmpstr(url, ==, expected);
	g_assert_true(has("x-line-access", "offline-token"));
	g_assert_true(has("x-line-mid", expected_mid));
	g_assert_true(has("x-line-channeltoken", expected_channel));
	g_assert_true(has("x-lhm", logical_method));
	if (expected_chat)
		g_assert_true(has("x-line-chat-id", expected_chat));
	if (expected_album)
		g_assert_true(has("x-line-album", expected_album));
	if (!strcmp(logical_method, "GET"))
		g_assert_cmpstr(upload, ==, "");
	if (strstr(expected_path, "post/create")) {
		g_autoptr(J) post = json_tokener_parse(upload);
		J *permission =
			field(field(post, "postInfo"), "readPermission");
		g_assert_cmpstr(strfield(permission, "type"), ==, "GROUP");
		J *gids = field(permission, "gids");

		g_assert_cmpint(json_object_array_length(gids), ==, 1);
		g_assert_cmpstr(json_object_get_string(
					json_object_array_get_idx(gids, 0)),
				==, group);
		g_assert_cmpstr(strfield(field(post, "contents"), "text"), ==,
				"Meet at six.");
	}
	if (strstr(expected_path, "search/note")) {
		g_autoptr(J) query = json_tokener_parse(upload);
		g_assert_cmpstr(strfield(query, "query"), ==, "poll");
		g_assert_cmpstr(strfield(query, "homeId"), ==, group);
	}
	size_t n = strlen(fixture);

	g_assert_cmpuint(writer((char *)fixture, 1, n, userdata), ==, n);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO key, ...)
{
	(void)curl;
	g_assert_cmpint(key, ==, CURLINFO_RESPONSE_CODE);
	va_list a;

	va_start(a, key);
	*va_arg(a, long *) = status;
	va_end(a);
	return CURLE_OK;
}

static void prepare(const char *path, const char *body, const char *method,
		    const char *channel)
{
	expected_path = path;
	fixture = body;
	logical_method = method;
	expected_channel = channel;
	expected_mid = self;
	expected_album = expected_chat = NULL;
	status = 200;
}

static J *params(void)
{
	J *p = obj();

	json_object_object_add(p, "chat", json_object_new_string(group));
	return p;
}

static void albums(void)
{
	Line s = { .mid = (char *)self, .access = "offline-token" };

	g_autoptr(J) p = params();
	json_object_object_add(p, "kind", json_object_new_string("albums"));
	prepare("/ext/album/moa/v2/albums?cursor=&orderBy=createTimeDesc&include=",
		"{\"code\":0,\"result\":{\"nextCursor\":\"a/b=\",\"albums\":[{\"albumId\":42,\"chatId\":\"c00000000000000000000000000000000\",\"title\":\"Hike\",\"photoCount\":1},{\"albumId\":43,\"chatId\":\"c11111111111111111111111111111111\",\"title\":\"Other chat\"}]}}",
		"GET", "1375220249");
	g_autoptr(GError) e = NULL;
	g_autoptr(J) a = line_shared_content(&s, "sharedList", p, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(strfield(a, "next"), ==, "a/b=");
	g_assert_cmpuint(json_object_array_length(field(a, "items")), ==, 1);
	json_object_object_add(p, "album", json_object_new_string("43"));
	g_assert_null(line_shared_content(&s, "sharedPhotos", p, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	json_object_object_add(p, "album", json_object_new_string("42"));
	prepare("/ext/album/api/v6/albums/42/photos?cursor=&pageSize=50&orderBy=createTimeDesc&include=all&filterType=",
		"{\"code\":0,\"result\":{\"photos\":[{\"photoId\":8,\"obsResourceId\":{\"sid\":\"a\",\"oid\":\"m-pic\"},\"width\":320,\"height\":240},{\"oid\":\"m-movie\",\"resourceType\":\"VIDEO\"}]}}",
		"GET", "1375220249");
	expected_chat = group;
	g_autoptr(J) photos = line_shared_content(&s, "sharedPhotos", p, &e);
	g_assert_no_error(e);
	g_assert_cmpuint(json_object_array_length(field(photos, "items")), ==,
			 2);
	g_assert_false(json_object_get_boolean(
		field(json_object_array_get_idx(field(photos, "items"), 1),
		      "viewable")));
	json_object_object_add(p, "id", json_object_new_string("m-movie"));
	g_assert_null(line_shared_photo(&s, p, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	json_object_object_add(p, "id", json_object_new_string("m-pic"));
	prepare("/oa/r/album/a/m-pic", "synthetic-media", "GET", "1375220249");
	expected_mid = group;
	expected_album = "42";
	g_autoptr(GBytes) pic = line_shared_photo(&s, p, &e);
	g_assert_no_error(e);
	g_assert_cmpuint(g_bytes_get_size(pic), ==, 15);
	json_object_object_add(p, "chat", json_object_new_string(other));
	g_assert_null(line_shared_photo(&s, p, &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	line_shared_clear(&s);
	g_assert_null(line_shared_photo(&s, p, &e));
	g_assert_nonnull(e);
}

static void group_notes(void)
{
	Line s = { .mid = (char *)self, .access = "offline-token" };

	g_autoptr(J) p = params();
	json_object_object_add(p, "kind", json_object_new_string("notes"));
	g_autoptr(GError) e = NULL;
	g_autofree char *path = g_strdup_printf(
		"/ext/note/nt/api/v57/post/list.json?homeId=%s&sourceType=TALKROOM",
		group);
	prepare(path,
		"{\"code\":0,\"result\":{\"posts\":[{\"postInfo\":{\"postId\":\"77\",\"updatedTime\":1234567890},\"contents\":{\"text\":\"Thai and 日本語\"},\"userInfo\":{\"displayName\":\"Mali\"}}]}}",
		"GET", "1341209850");
	g_autoptr(J) list = line_shared_content(&s, "sharedList", p, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(strfield(list, "next"), ==, "77:1234567890");
	J *note = json_object_array_get_idx(field(list, "items"), 0);

	g_assert_cmpstr(strfield(note, "title"), ==, "Mali");
	g_assert_cmpstr(strfield(note, "text"), ==, "Thai and 日本語");
	g_autofree char *create = g_strdup_printf(
		"/ext/note/nt/api/v57/post/create.json?homeId=%s&sourceType=TALKROOM",
		group);
	prepare(create, "{\"code\":0,\"result\":{\"postId\":\"78\"}}", "POST",
		"1341209850");
	json_object_object_add(p, "text",
			       json_object_new_string("Meet at six."));
	g_autoptr(J) ack = line_shared_content(&s, "sharedPost", p, &e);
	g_assert_no_error(e);
	g_assert_true(json_object_get_boolean(field(ack, "saved")));
	json_object_object_add(p, "kind", json_object_new_string("polls"));
	prepare("/mh/api/v46/search/note.json",
		"{\"code\":0,\"result\":{\"feeds\":[{\"post\":{\"postId\":\"9\",\"text\":\"Lunch poll\"}}]}}",
		"POST", "1341209850");
	g_autoptr(J) polls = line_shared_content(&s, "sharedList", p, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(
		strfield(json_object_array_get_idx(field(polls, "items"), 0),
			 "text"),
		==, "Lunch poll");
	guint prev = count;

	json_object_object_add(p, "chat", json_object_new_string(self));
	g_assert_null(line_shared_content(&s, "sharedPost", p, &e));
	g_assert_nonnull(e);
	g_assert_cmpuint(count, ==, prev);
	line_shared_clear(&s);
}

static void errors(void)
{
	Line s = { .mid = (char *)self, .access = "offline-token" };

	g_autoptr(J) p = params();
	json_object_object_add(p, "kind", json_object_new_string("albums"));
	json_object_object_add(p, "cursor", json_object_new_string("a/b="));
	prepare("/ext/album/moa/v2/albums?cursor=a%2Fb%3D&orderBy=createTimeDesc&include=",
		"{\"code\":17,\"message\":\"Unavailable\"}", "GET",
		"1375220249");
	g_autoptr(GError) e = NULL;
	g_assert_null(line_shared_content(&s, "sharedList", p, &e));
	g_assert_nonnull(strstr(e->message, "Unavailable"));
	g_clear_error(&e);
	status = 401;
	g_assert_null(line_shared_content(&s, "sharedList", p, &e));
	g_assert_nonnull(strstr(e->message, "401"));
	g_clear_error(&e);
	guint old = approvals;

	status = 200;
	fixture = "{\"code\":0,\"result\":{}}";
	g_assert_null(line_shared_content(&s, "sharedList", p, &e));
	g_assert_nonnull(e);
	g_assert_cmpuint(approvals, ==, old + 1);
	line_shared_clear(&s);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/shared/albums-photos-session", albums);
	g_test_add_func("/shared/notes-and-poll-search", group_notes);
	g_test_add_func("/shared/page-and-service-errors", errors);
	return g_test_run();
}
