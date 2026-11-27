// SPDX-License-Identifier: GPL-3.0-or-later
#include "groups.h"
#include "liff_url.h"
#include "obs_http.h"
#include "profile_image.h"
#include "square.h"
#include "stickers.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <string.h>

#define SQ_MID "s11111111111111111111111111111111"
#define CHAT_MID "m22222222222222222222222222222222"
#define SUB_MID "m33333333333333333333333333333333"
#define ME_MEM "p44444444444444444444444444444444"
#define PEER_MEM "p55555555555555555555555555555555"
#define GRP_MID "c66666666666666666666666666666666"
#define SELF_MID "u77777777777777777777777777777777"

static gboolean force_v1_ticket, force_sq_error, checked_join_code;
static gboolean force_event_directory, force_no_xlt_squares;
static gboolean force_public_search, anonymous_search, force_no_xlt_keywords;
static int rpc_calls, sq_error_code, event_pages;
static char *last_method;
static const char *search_url;
static struct curl_slist *search_headers;
static curl_write_callback search_writer;
static void *search_userdata;

void line_resolve_names(Line *s, J *names, char type)
{
	(void)s;
	(void)names;
	(void)type;
}

gboolean line_received_filename(const char *name)
{
	return name && *name && *name != '.' && !strchr(name, '/');
}

char *line_obs_token(Line *s, GError **error)
{
	(void)s;
	(void)error;
	return g_strdup("fixture-obs-token");
}

gboolean line_obs_upload_profile(const char *token, const char *mid,
				 GBytes *jpeg, GError **error)
{
	(void)token;
	(void)mid;
	(void)jpeg;
	(void)error;
	return TRUE;
}

char *line_obs_upload_square_profile(const char *token, const char *namespace,
				     const char *oid, GBytes *jpeg,
				     GError **error)
{
	(void)error;
	g_assert_cmpstr(token, ==, "fixture-obs-token");
	g_assert_cmpstr(namespace, ==, "member");
	g_assert_cmpstr(oid, ==, ME_MEM);
	g_assert_nonnull(jpeg);
	return g_strdup("0hNewSquareHash");
}

J *line_own_profile(Line *s, GError **error)
{
	(void)s;
	(void)error;
	return obj();
}

J *line_sticker_metadata(Line *s, LineStickers *cache, const char *package_id,
			 const char *sticker_id, GError **error)
{
	(void)s;
	(void)cache;
	(void)error;
	g_assert_cmpstr(package_id, ==, "11537");
	g_assert_cmpstr(sticker_id, ==, "52002734");
	J *meta = obj();

	json_object_object_add(meta, "STKPKGID",
			       json_object_new_string(package_id));
	json_object_object_add(meta, "STKID",
			       json_object_new_string(sticker_id));
	json_object_object_add(meta, "STKVER", json_object_new_string("1"));
	return meta;
}

CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
	(void)curl;
	va_list args;

	va_start(args, option);
	switch (option) {
	case CURLOPT_URL:
		search_url = va_arg(args, const char *);
		break;
	case CURLOPT_HTTPHEADER:
		search_headers = va_arg(args, struct curl_slist *);
		break;
	case CURLOPT_WRITEFUNCTION:
		search_writer = va_arg(args, curl_write_callback);
		break;
	case CURLOPT_WRITEDATA:
		search_userdata = va_arg(args, void *);
		break;
	default:
		break;
	}
	va_end(args);
	return CURLE_OK;
}

static gboolean search_has_header(const char *prefix, const char *contains)
{
	for (struct curl_slist *h = search_headers; h; h = h->next)
		if (g_str_has_prefix(h->data, prefix) &&
		    strstr(h->data, contains))
			return TRUE;
	return FALSE;
}

CURLcode __wrap_curl_easy_perform(CURL *curl)
{
	(void)curl;
	const char *fixture =
		"{\"result\":{\"main\":[{\"body\":{\"collectionType\":\"openchat\","
		"\"area\":\"th\",\"itemList\":[{\"area\":\"th\",\"docId\":\"" SQ_MID
		"\",\"docRank\":1,\"link\":\"line://square/home?encryptedSquareMid=emidTicket_1234567890&utm_source=line-search\","
		"\"thumb\":{\"imageUrl\":\"https://example.com/avatar.png\","
		"\"openchatIconUrl\":\"\"},\"title\":\"Linux Fallback\","
		"\"desc\":\"Public Linux chat\",\"isPrivate\":false,"
		"\"memberCountText\":\"81 members\"}],"
		"\"moreLink\":\"?q=linux&sscode=os.tab.square&start=30&rank=31\"}}]}}";

	if (anonymous_search)
		g_assert_false(search_has_header("x-line-channeltoken: ", ""));
	else
		g_assert_true(search_has_header("x-line-channeltoken: ",
						"search-channel-token"));
	g_assert_true(search_has_header("x-line-info: ", SELF_MID));
	g_assert_true(search_has_header("x-line-info: ", "ANDROID"));
	g_assert_true(search_has_header("x-line-info: ", "\"region\":\"TH\""));
	if (strstr(search_url, "rank=31"))
		g_assert_cmpstr(
			search_url, ==,
			"https://legy.line-apps.com/lus/v2/search?q=linux&sscode=os.tab.square&start=30&rank=31");
	else
		g_assert_cmpstr(
			search_url, ==,
			"https://legy.line-apps.com/lus/v2/search?q=linux&sscode=os.tab.square");
	size_t n = strlen(fixture);

	g_assert_cmpuint(search_writer((char *)fixture, 1, n, search_userdata),
			 ==, n);
	return CURLE_OK;
}

CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO option, ...)
{
	(void)curl;
	va_list args;

	va_start(args, option);
	if (option == CURLINFO_RESPONSE_CODE)
		*va_arg(args, long *) = 200;
	else if (option == CURLINFO_RETRY_AFTER)
		*va_arg(args, curl_off_t *) = 0;
	else
		g_assert_not_reached();
	va_end(args);
	return CURLE_OK;
}

J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	rpc_calls++;
	g_free(last_method);
	last_method = g_strdup(method);
	/* Verify every call serializes through Thrift cleanly. */
	g_autoptr(GError) enc_err = NULL;
	g_autoptr(GBytes) wire = line_encode_call(method, args, &enc_err);

	g_assert_no_error(enc_err);
	g_assert_nonnull(wire);

	if (!strcmp(path, "/CH4")) {
		g_assert_cmpstr(method, ==,
				"approveChannelAndIssueChannelToken");
		g_assert_cmpstr(strfield(args, "1:str"), ==, "1557852768");
		if (anonymous_search) {
			g_set_error_literal(error, LINE_ERROR, 1,
					    "Channel permission denied");
			return NULL;
		}
		return json_tokener_parse("{\"5\":\"search-channel-token\"}");
	}
	if (!strcmp(path, "/S4")) {
		if (!strcmp(method, "getProfile"))
			return json_tokener_parse("{\"12\":\"TH\"}");
		J *req = field(args, "1:obj");

		if (!strcmp(method, "findChatByTicket")) {
			g_assert_cmpstr(strfield(req, "1:str"), ==,
					"grpTicket_123");
			return json_tokener_parse(
				"{\"1\":{\"1\":0,\"2\":\"" GRP_MID
				"\",\"6\":\"Weekend Hikers\"}}");
		}
		if (!strcmp(method, "acceptChatInvitationByTicket")) {
			g_assert_cmpint(
				json_object_get_int(field(req, "1:i32")), ==,
				9);
			g_assert_cmpstr(strfield(req, "2:str"), ==, GRP_MID);
			g_assert_cmpstr(strfield(req, "3:str"), ==,
					"grpTicket_123");
			return obj();
		}
		g_assert_not_reached();
	}

	g_assert_cmpstr(path, ==, "/SQ1");
	J *req = field(args, "1:obj");

	g_assert_nonnull(req);
	if (force_sq_error) {
		force_sq_error = FALSE;
		s->service_code = sq_error_code;
		g_set_error_literal(error, LINE_ERROR, 1, "Square error");
		return NULL;
	}
	if (!strcmp(method, "getJoinedSquares")) {
		if (force_no_xlt_squares) {
			s->service_code = 501;
			g_set_error_literal(error, LINE_ERROR, 1,
					    "NO XLT:Not yet implemented");
			return NULL;
		}
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"" SQ_MID
			"\",\"2\":\"Linux Club\",\"3\":\"Welcome!\",\"4\":\"0hSqHash\","
			"\"5\":\"Open source chat\",\"6\":true,\"8\":1,\"9\":\"https://line.me/ti/g2/inv123\",\"11\":true}],"
			"\"2\":{\"" SQ_MID "\":{\"1\":\"" ME_MEM
			"\",\"2\":\"" SQ_MID
			"\",\"3\":\"Novesce\",\"4\":\"0hMeHash\",\"7\":1,\"8\":1,\"9\":5}},"
			"\"3\":{\"" SQ_MID
			"\":{\"2\":1,\"4\":2,\"6\":2,\"8\":2,\"9\":1,\"12\":2}},"
			"\"4\":{\"" SQ_MID "\":{\"1\":42,\"2\":1,\"4\":2}}}");
	}
	if (!strcmp(method, "getJoinedSquareChats")) {
		if (force_event_directory) {
			s->service_code = 501;
			g_set_error_literal(error, LINE_ERROR, 1,
					    "NO XLT:Not yet implemented");
			return NULL;
		}
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"" CHAT_MID "\",\"2\":\"" SQ_MID
			"\",\"3\":1,\"4\":\"Main Room\",\"5\":\"0hChatHash\",\"7\":3}],"
			"\"3\":{\"" CHAT_MID
			"\":{\"3\":{\"1\":{\"1\":\"" PEER_MEM
			"\",\"2\":\"" CHAT_MID
			"\",\"4\":\"1002\",\"5\":1700000002000,\"10\":\"Hello\"},\"2\":\"Alice\"},"
			"\"5\":{\"1\":10,\"2\":2,\"3\":\"1000\"}}}}");
	}
	if (!strcmp(method, "fetchSquareChatEvents")) {
		const char *tok = strfield(req, "3:str");
		int dir = json_object_get_int(field(req, "5:i32"));
		int limit = json_object_get_int(field(req, "4:i32"));

		g_assert_cmpstr(strfield(req, "2:str"), ==, CHAT_MID);
		g_assert_nonnull(tok);
		g_assert_cmpint(json_object_get_int64(field(req, "1:i64")), ==,
				0);
		g_assert_cmpint(json_object_get_int(field(req, "6:i32")), ==,
				1);
		if (dir == 1 && limit == 1 && !*tok)
			return json_tokener_parse(
				"{\"2\":[],\"3\":\"read-sync-0\",\"4\":\"read-sync-0\"}");
		if (dir == 1 && !strcmp(tok, "read-sync-0"))
			return json_tokener_parse(
				"{\"2\":[{\"2\":0,\"3\":6,\"4\":{\"7\":{\"1\":\"" CHAT_MID
				"\",\"2\":\"" ME_MEM
				"\",\"4\":\"9999\"}}},{\"2\":0,\"3\":6,\"4\":{\"7\":{\"1\":\"" CHAT_MID
				"\",\"2\":\"" PEER_MEM
				"\",\"4\":\"1005\"}}}],\"3\":\"read-sync-1\"}");
		if (dir == 1 && !strcmp(tok, "read-sync-1"))
			return json_tokener_parse(
				"{\"2\":[],\"3\":\"read-sync-1\"}");
		if (dir == 1 && !strcmp(tok, "read-sync-live"))
			return json_tokener_parse(
				"{\"2\":[{\"2\":0,\"3\":6,\"4\":{\"7\":{\"1\":\"" CHAT_MID
				"\",\"2\":\"" PEER_MEM
				"\",\"4\":\"1010\"}}}],\"3\":\"read-sync-2\"}");
		if (dir == 1 && !strcmp(tok, "read-sync-2"))
			return json_tokener_parse(
				"{\"2\":[],\"3\":\"read-sync-2\"}");
		g_assert_cmpint(dir, ==, *tok ? 2 : 1);
		return json_tokener_parse(
			"{\"2\":[{\"2\":1700000001000,\"3\":0,\"4\":{\"1\":{\"1\":\"" CHAT_MID
			"\",\"2\":{\"1\":{\"1\":\"" PEER_MEM
			"\",\"2\":\"" CHAT_MID
			"\",\"4\":\"1001\",\"5\":1700000001000,\"10\":\"Check line://ti/g2/LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg\","
			"\"15\":0}},\"3\":\"Alice\"}}},"
			"{\"2\":1700000002000,\"3\":1,\"4\":{\"2\":{\"1\":\"" CHAT_MID
			"\",\"2\":{\"1\":{\"1\":\"" ME_MEM
			"\",\"2\":\"" CHAT_MID
			"\",\"4\":\"1002\",\"5\":1700000002000,\"10\":\"Deleted\"}}}}},"
			"{\"2\":1700000003000,\"3\":5,\"4\":{\"6\":{\"1\":\"" CHAT_MID
			"\",\"3\":\"1002\"}}}],"
			"\"3\":\"sync-1\",\"4\":\"next-tok\"}");
	}
	if (!strcmp(method, "sendMessage")) {
		J *sq_msg = field(req, "3:obj");
		J *msg = field(sq_msg, "1:obj");

		g_assert_cmpstr(strfield(req, "2:str"), ==, CHAT_MID);
		g_assert_cmpstr(strfield(msg, "2:str"), ==, CHAT_MID);
		if (!g_strcmp0(strfield(msg, "10:str"), "@Alice hi"))
			g_assert_cmpstr(
				strfield(field(msg, "18:map:str:str"),
					 "MENTION"),
				==,
				"{\"MENTIONEES\":[{\"S\":\"0\",\"E\":\"6\",\"M\":\"" PEER_MEM
				"\"}]}");
		return json_tokener_parse(
			"{\"1\":{\"1\":{\"4\":\"1009\",\"5\":1700000009000}}}");
	}
	if (!strcmp(method, "reactToMessage")) {
		g_assert_cmpstr(strfield(req, "2:str"), ==, CHAT_MID);
		g_assert_cmpstr(strfield(req, "3:str"), ==, "1001");
		return obj();
	}
	if (!strcmp(method, "unsendMessage") ||
	    !strcmp(method, "destroyMessage") ||
	    !strcmp(method, "markAsRead")) {
		g_assert_cmpstr(strfield(req, "2:str"), ==, CHAT_MID);
		return obj();
	}
	if (!strcmp(method, "getSquareChatAnnouncements")) {
		return json_tokener_parse(
			"{\"1\":[{\"1\":77,\"2\":0,\"3\":{\"1\":{\"1\":\"1001\",\"2\":\"Pinned notice\"}}}]}");
	}
	if (!strcmp(method, "createSquareChatAnnouncement")) {
		return json_tokener_parse(
			"{\"1\":{\"1\":78,\"2\":0,\"3\":{\"1\":{\"1\":\"1001\",\"2\":\"New notice\"}}}}");
	}
	if (!strcmp(method, "deleteSquareChatAnnouncement")) {
		g_assert_cmpint(json_object_get_int64(field(req, "3:i64")), ==,
				77);
		return obj();
	}
	if (!strcmp(method, "getSquareCategories")) {
		return json_tokener_parse(
			"{\"1\":[{\"1\":1,\"2\":\"General\"},{\"1\":2,\"2\":\"Tech\"}]}");
	}
	if (!strcmp(method, "findSquareByEmid")) {
		g_assert_cmpstr(strfield(req, "1:str"), ==,
				"emidTicket_1234567890");
		return json_tokener_parse(
			"{\"1\":{\"1\":\"" SQ_MID
			"\",\"2\":\"Encrypted Club\",\"5\":\"Encrypted desc\","
			"\"14\":{\"1\":2}},\"4\":{\"1\":86}}");
	}
	if (!strcmp(method, "findSquareByInvitationTicketV2")) {
		if (force_v1_ticket) {
			g_set_error_literal(error, LINE_ERROR,
					    LINE_METHOD_UNAVAILABLE,
					    "Unavailable");
			return NULL;
		}
		g_assert_cmpstr(strfield(req, "1:str"), ==,
				"LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg");
		return json_tokener_parse(
			"{\"1\":{\"1\":\"" SQ_MID
			"\",\"2\":\"Linux Club\",\"5\":\"Open source\",\"14\":{\"1\":2}},"
			"\"4\":{\"1\":99},\"7\":{\"1\":\"" CHAT_MID
			"\",\"2\":\"" SQ_MID "\",\"4\":\"Main Room\"}}");
	}
	if (!strcmp(method, "findSquareByInvitationTicket")) {
		g_assert_cmpstr(strfield(req, "2:str"), ==, "fallbackTicket");
		return json_tokener_parse(
			"{\"1\":{\"1\":\"" SQ_MID
			"\",\"2\":\"Fallback Club\",\"14\":{\"1\":1,\"2\":{\"1\":{\"1\":\"Why join?\"}}}},"
			"\"4\":{\"1\":12},\"7\":{\"1\":\"" CHAT_MID
			"\",\"2\":\"" SQ_MID "\",\"4\":\"Lobby\"}}");
	}
	if (!strcmp(method, "checkJoinCode")) {
		checked_join_code = TRUE;
		g_assert_cmpstr(strfield(req, "2:str"), ==, SQ_MID);
		g_assert_cmpstr(strfield(req, "3:str"), ==, "secret");
		return obj();
	}
	if (!strcmp(method, "joinSquare")) {
		J *jv = field(req, "5:obj");
		gboolean approval = jv && field(jv, "1:obj") != NULL;

		return approval ?
			       json_tokener_parse("{\"1\":{\"1\":\"" SQ_MID
						  "\",\"2\":\"Fallback Club\"},"
						  "\"4\":{\"1\":\"" ME_MEM
						  "\",\"2\":\"" SQ_MID
						  "\",\"7\":1,\"8\":10}}") :
			       json_tokener_parse("{\"1\":{\"1\":\"" SQ_MID
						  "\",\"2\":\"Linux Club\"},"
						  "\"4\":{\"1\":\"" ME_MEM
						  "\",\"2\":\"" SQ_MID
						  "\",\"7\":2,\"8\":10},"
						  "\"7\":{\"1\":\"" CHAT_MID
						  "\",\"2\":\"" SQ_MID
						  "\",\"4\":\"Main Room\"}}");
	}
	if (!strcmp(method, "createSquare")) {
		J *sq = field(req, "2:obj");
		J *creator = field(req, "3:obj");

		g_assert_nonnull(strfield(sq, "4:str"));
		g_assert_null(field(sq, "12:i32"));
		g_assert_null(field(creator, "7:i32"));
		g_assert_null(field(creator, "8:i32"));
		return json_tokener_parse(
			"{\"1\":{\"1\":\"" SQ_MID "\",\"2\":\"New Club\"},"
			"\"2\":{\"1\":\"" ME_MEM "\",\"2\":\"" SQ_MID
			"\",\"3\":\"Owner\",\"7\":2,\"8\":1},"
			"\"7\":{\"1\":\"" CHAT_MID "\",\"2\":\"" SQ_MID
			"\",\"4\":\"New Club\"}}");
	}
	if (!strcmp(method, "createSquareChat")) {
		return json_tokener_parse("{\"1\":{\"1\":\"" SUB_MID
					  "\",\"2\":\"" SQ_MID
					  "\",\"4\":\"Off-topic\"}}");
	}
	if (!strcmp(method, "joinSquareChat")) {
		return json_tokener_parse("{\"1\":{\"1\":\"" SUB_MID
					  "\",\"2\":\"" SQ_MID
					  "\",\"4\":\"Off-topic\"}}");
	}
	if (!strcmp(method, "leaveSquare")) {
		g_assert_cmpstr(strfield(req, "2:str"), ==, SQ_MID);
		return obj();
	}
	if (!strcmp(method, "getSquare")) {
		return json_tokener_parse(
			"{\"1\":{\"1\":\"" SQ_MID
			"\",\"2\":\"Linux Club\",\"3\":\"Welcome!\",\"5\":\"Desc\",\"6\":true,\"11\":true},"
			"\"2\":{\"1\":\"" ME_MEM "\",\"2\":\"" SQ_MID
			"\",\"3\":\"Novesce\",\"7\":1,\"8\":1,\"9\":5,\"13\":\"Hi\"},"
			"\"3\":{\"2\":1,\"4\":2,\"6\":2,\"8\":2,\"9\":1,\"12\":2},"
			"\"4\":{\"1\":42,\"2\":1,\"4\":2}}");
	}
	if (!strcmp(method, "getSquareChat")) {
		if (force_event_directory)
			return json_tokener_parse(
				"{\"1\":{\"1\":\"" CHAT_MID "\",\"2\":\"" SQ_MID
				"\",\"4\":\"Main Room\",\"7\":3},"
				"\"2\":{\"1\":\"" ME_MEM "\",\"2\":\"" CHAT_MID
				"\",\"4\":1},\"3\":{\"5\":{\"1\":10,\"2\":2}}}");
		return json_tokener_parse("{\"1\":{\"1\":\"" CHAT_MID
					  "\",\"2\":\"" SQ_MID
					  "\",\"4\":\"Main Room\",\"7\":3}}");
	}
	if (!strcmp(method, "getJoinableSquareChats")) {
		return json_tokener_parse("{\"1\":[{\"1\":\"" SUB_MID
					  "\",\"2\":\"" SQ_MID
					  "\",\"4\":\"Off-topic\"}]}");
	}
	if (!strcmp(method, "getInvitationTicketUrl")) {
		return json_tokener_parse(
			"{\"1\":\"https://line.me/ti/g2/freshTicket99\"}");
	}
	if (!strcmp(method, "updateSquare")) {
		return json_tokener_parse(
			"{\"2\":{\"1\":\"" SQ_MID
			"\",\"2\":\"Renamed Club\",\"5\":\"New desc\",\"6\":false,\"11\":true}}");
	}
	if (!strcmp(method, "updateSquareChat")) {
		return json_tokener_parse("{\"2\":{\"1\":\"" CHAT_MID
					  "\",\"2\":\"" SQ_MID
					  "\",\"4\":\"Renamed Room\"}}");
	}
	if (!strcmp(method, "updateSquareMember")) {
		return json_tokener_parse(
			"{\"2\":{\"1\":\"" ME_MEM "\",\"2\":\"" SQ_MID
			"\",\"3\":\"NewNick\",\"4\":\"0hNewSquareHash\",\"7\":1,\"8\":1,\"9\":6,\"13\":\"Bio\"}}");
	}
	if (!strcmp(method, "searchSquareMembers")) {
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"" PEER_MEM "\",\"2\":\"" SQ_MID
			"\",\"3\":\"Alice\",\"7\":1,\"8\":10,\"9\":2,\"11\":\"Let me in\"}],"
			"\"3\":\"page2\"}");
	}
	if (!strcmp(method, "getSquareMember")) {
		return json_tokener_parse(
			"{\"1\":{\"1\":\"" PEER_MEM "\",\"2\":\"" SQ_MID
			"\",\"3\":\"Alice\",\"7\":1,\"8\":10,\"9\":2}}");
	}
	if (!strcmp(method, "approveSquareMembers") ||
	    !strcmp(method, "rejectSquareMembers")) {
		return obj();
	}
	if (!strcmp(method, "manualRepair")) {
		g_assert_true(force_event_directory);
		event_pages++;
		if (!strfield(req, "3:str")) {
			g_assert_null(field(req, "1:str"));
			return json_tokener_parse(
				"{\"1\":[{\"3\":15,\"4\":{\"16\":{\"1\":{\"1\":\"" SQ_MID
				"\"}}}},{\"3\":16,\"4\":{\"17\":{\"1\":{\"1\":\"" CHAT_MID
				"\",\"2\":\"" SQ_MID
				"\",\"4\":\"Main Room\"}}}}],"
				"\"2\":\"bootstrap-sync\",\"3\":\"events-page-2\"}");
		}
		g_assert_cmpstr(strfield(req, "3:str"), ==, "events-page-2");
		return json_tokener_parse(
			"{\"1\":[],\"2\":\"bootstrap-sync-2\"}");
	}
	if (!strcmp(method, "fetchMyEvents")) {
		return json_tokener_parse(
			"{\"1\":{\"1\":555},\"2\":[{\"3\":29,\"4\":{\"30\":{\"1\":\"" CHAT_MID
			"\",\"2\":{\"1\":{\"1\":\"" PEER_MEM
			"\",\"2\":\"" CHAT_MID
			"\",\"4\":\"1010\",\"5\":1700000010000,\"10\":\"Ping\"}}}}}],"
			"\"3\":\"sync-2\"}");
	}
	if (!strcmp(method, "getPopularKeywords")) {
		if (force_no_xlt_keywords) {
			s->service_code = 501;
			g_set_error_literal(error, LINE_ERROR, 1,
					    "NO XLT:Not yet implemented");
			return NULL;
		}
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"Linux\"},{\"1\":\"Music\"}]}");
	}
	if (!strcmp(method, "searchSquares")) {
		g_assert_cmpstr(strfield(req, "2:str"), ==, "linux");
		if (force_public_search) {
			s->service_code = 501;
			g_set_error_literal(error, LINE_ERROR, 1,
					    "NO XLT:Not yet implemented");
			return NULL;
		}
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"" SQ_MID
			"\",\"2\":\"Linux Club\",\"5\":\"Open source\","
			"\"9\":\"https://line.me/ti/g2/inv123\",\"10\":7,"
			"\"14\":{\"1\":2}}],"
			"\"2\":{\"" SQ_MID "\":{\"1\":42}},\"4\":\"next2\"}");
	}
	if (!strcmp(method, "searchSquareChatMentionables")) {
		g_assert_cmpstr(strfield(req, "1:str"), ==, CHAT_MID);
		return json_tokener_parse(
			"{\"1\":[{\"1\":\"" PEER_MEM
			"\",\"2\":\"Alice\",\"3\":\"0hAlice\"}]}");
	}
	g_error("Unexpected RPC method: %s", method);
	return NULL;
}

static void test_tickets(void)
{
	g_autofree char *t1 = line_square_parse_ticket(
		"line://ti/g2/LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg");
	g_assert_cmpstr(t1, ==, "LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg");

	g_autofree char *t2 = line_square_parse_ticket(
		"https://line.me/ti/g2/LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg?utm_source=invitation");
	g_assert_cmpstr(t2, ==, "LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg");

	g_autofree char *t3 =
		line_square_parse_ticket("https://line.me/R/ti/g2/abcDEF_1234");
	g_assert_cmpstr(t3, ==, "abcDEF_1234");

	g_autofree char *t4 =
		line_square_parse_ticket("lineb://ti/g2/xyz987-654");
	g_assert_cmpstr(t4, ==, "xyz987-654");

	g_autofree char *t5 = line_square_parse_ticket(
		"  LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg  ");
	g_assert_cmpstr(t5, ==, "LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg");

	g_assert_null(line_square_parse_ticket("https://example.com/nope"));
	g_assert_null(line_square_parse_ticket("ab"));
	g_assert_null(line_square_parse_ticket("bad ticket with spaces"));

	g_autofree char *g1 =
		line_group_parse_ticket("line://ti/g/grpTicket_123");
	g_assert_cmpstr(g1, ==, "grpTicket_123");

	g_autofree char *g2 = line_group_parse_ticket(
		"https://line.me/ti/g/grpTicket_123?a=1");
	g_assert_cmpstr(g2, ==, "grpTicket_123");

	g_autofree char *g3 =
		line_group_parse_ticket("https://line.me/R/ti/g/grpTicket_123");
	g_assert_cmpstr(g3, ==, "grpTicket_123");

	g_assert_null(line_group_parse_ticket(
		"line://ti/g2/LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg"));
	g_assert_null(line_group_parse_ticket("https://example.com/ti/p/abc"));
}

static void test_group_ticket(void)
{
	Line s = { .mid = SELF_MID };
	g_autofree char *ticket = NULL;

	g_autoptr(GError) error = NULL;
	g_autoptr(J) found = line_group_find_ticket(
		&s, "line://ti/g/grpTicket_123", &ticket, &error);

	g_assert_no_error(error);
	g_assert_nonnull(found);
	g_assert_cmpstr(ticket, ==, "grpTicket_123");
	g_assert_cmpstr(strfield(found, "id"), ==, GRP_MID);
	g_assert_cmpstr(strfield(found, "name"), ==, "Weekend Hikers");

	g_autoptr(J) joined =
		line_group_join_ticket(&s, GRP_MID, ticket, 9, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(joined, "id"), ==, GRP_MID);
}

static void test_chats_and_history(void)
{
	Line s = { .mid = SELF_MID, .pictures = obj() };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) squares = NULL, members = NULL, auths = NULL, c2s = NULL;
	g_autoptr(J) chats =
		line_square_chats(&s, &squares, &members, &auths, &c2s, &error);

	g_assert_no_error(error);
	g_assert_nonnull(chats);
	g_assert_cmpuint(json_object_array_length(chats), ==, 1);
	J *row = json_object_array_get_idx(chats, 0);

	g_assert_cmpstr(strfield(row, "id"), ==, CHAT_MID);
	g_assert_cmpstr(strfield(row, "squareMid"), ==, SQ_MID);
	g_assert_cmpstr(strfield(row, "name"), ==, "Main Room");
	g_assert_cmpint(json_object_get_int(field(row, "unread")), ==, 2);
	g_assert_cmpstr(strfield(c2s, CHAT_MID), ==, SQ_MID);
	g_assert_cmpstr(strfield(s.pictures, CHAT_MID), ==, "/0hChatHash");

	g_autofree char *next_tok = NULL;
	gboolean more = FALSE;

	g_autoptr(J) raw = NULL;
	g_autoptr(J) hist = line_square_history(&s, CHAT_MID, ME_MEM, NULL,
						&raw, &next_tok, &more, &error);
	g_assert_no_error(error);
	g_assert_nonnull(hist);
	g_assert_cmpuint(json_object_array_length(hist), ==, 1);
	g_assert_cmpstr(next_tok, ==, "next-tok");
	g_assert_true(more);
	J *m = json_object_array_get_idx(hist, 0);

	g_assert_cmpstr(strfield(m, "id"), ==, "1001");
	g_assert_cmpstr(strfield(m, "from"), ==, "Alice");
	g_assert_cmpstr(strfield(m, "senderId"), ==, PEER_MEM);
	json_object_put(s.pictures);
}

static void test_event_directory(void)
{
	Line s = { .mid = SELF_MID, .pictures = obj() };
	g_autoptr(GError) error = NULL;
	g_autoptr(J) squares = NULL, members = NULL, auths = NULL, c2s = NULL;

	force_event_directory = TRUE;
	event_pages = 0;
	g_autoptr(J) chats =
		line_square_chats(&s, &squares, &members, &auths, &c2s, &error);
	force_event_directory = FALSE;
	g_assert_no_error(error);
	g_assert_nonnull(chats);
	g_assert_cmpint(event_pages, ==, 2);
	g_assert_cmpuint(json_object_array_length(chats), ==, 1);
	J *row = json_object_array_get_idx(chats, 0);

	g_assert_cmpstr(strfield(row, "id"), ==, CHAT_MID);
	g_assert_cmpstr(strfield(row, "name"), ==, "Main Room");
	g_assert_cmpint(json_object_get_int(field(row, "unread")), ==, 2);
	g_assert_cmpstr(strfield(c2s, CHAT_MID), ==, SQ_MID);
	g_assert_cmpint(s.service_code, ==, -1);

	/*
	 * LINE 26.19+ also returns 501 NO XLT on getJoinedSquares itself;
	 * verify manualRepair + getSquareChat + getSquare populates all maps.
	 */
	g_autoptr(J) sq2 = NULL, mem2 = NULL, auth2 = NULL, c2s2 = NULL;

	force_event_directory = TRUE;
	force_no_xlt_squares = TRUE;
	event_pages = 0;
	g_autoptr(J) chats2 =
		line_square_chats(&s, &sq2, &mem2, &auth2, &c2s2, &error);
	force_no_xlt_squares = FALSE;
	force_event_directory = FALSE;
	g_assert_no_error(error);
	g_assert_nonnull(chats2);
	g_assert_cmpint(event_pages, ==, 2);
	g_assert_cmpuint(json_object_array_length(chats2), ==, 1);
	g_assert_nonnull(field(sq2, SQ_MID));
	g_assert_nonnull(field(mem2, SQ_MID));
	g_assert_nonnull(field(auth2, SQ_MID));
	g_assert_cmpstr(strfield(c2s2, CHAT_MID), ==, SQ_MID);
	g_assert_cmpint(s.service_code, ==, -1);
	json_object_put(s.pictures);
}

static void test_messaging(void)
{
	Line s = { .mid = SELF_MID };

	g_autoptr(GError) error = NULL;
	g_autoptr(J) mentions = json_tokener_parse(
		"[{\"id\":\"" PEER_MEM "\",\"start\":0,\"length\":6}]");
	g_autoptr(J) sent = line_square_send_text(&s, CHAT_MID, ME_MEM,
						  "@Alice hi", "1001", NULL,
						  mentions, 1, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(sent, "id"), ==, "1009");

	g_autoptr(J) stk = line_square_send_sticker(&s, NULL, CHAT_MID, ME_MEM,
						    "11537", "52002734", NULL,
						    2, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(stk, "id"), ==, "1009");

	g_autoptr(J) r1 =
		line_square_react(&s, CHAT_MID, "1001", "nice", 3, &error);
	g_assert_no_error(error);
	g_assert_nonnull(r1);

	g_autoptr(J) u1 = line_square_unsend(&s, CHAT_MID, "1009", &error);
	g_assert_no_error(error);
	g_assert_nonnull(u1);

	g_autoptr(J) d1 =
		line_square_destroy_message(&s, CHAT_MID, "1001", &error);
	g_assert_no_error(error);
	g_assert_nonnull(d1);

	g_autoptr(J) mr = line_square_mark_read(&s, CHAT_MID, "1009", &error);
	g_assert_no_error(error);
	g_assert_nonnull(mr);

	g_autoptr(J) anns =
		line_square_announcements(&s, CHAT_MID, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(anns), ==, 1);

	g_autoptr(J) created = line_square_announce_message(
		&s, CHAT_MID, "1001", "New notice", PEER_MEM, 1700000001000, 4,
		&error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(created, "id"), ==, "78");

	g_autoptr(J) removed =
		line_square_remove_announcement(&s, CHAT_MID, "77", &error);
	g_assert_no_error(error);
	g_assert_nonnull(removed);
}

static void test_lifecycle_and_sync(void)
{
	Line s = { .mid = SELF_MID, .pictures = obj() };

	g_autoptr(GError) error = NULL;

	g_autoptr(J) cats = line_square_categories(&s, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(cats), ==, 2);

	g_autoptr(J) prev = line_square_find_ticket(
		&s, "line://ti/g2/LgnHjUWR4XpyFJ5E2a-9wq5ROBAgg62s0OBrKg",
		&error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(prev, "squareMid"), ==, SQ_MID);
	g_assert_cmpstr(strfield(prev, "joinMethod"), ==, "code");

	g_autoptr(J) encrypted = line_square_find_ticket(
		&s,
		"line://square/home?encryptedSquareMid=emidTicket_1234567890&utm_source=line-search",
		&error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(encrypted, "squareMid"), ==, SQ_MID);
	g_assert_cmpstr(strfield(encrypted, "name"), ==, "Encrypted Club");
	g_assert_cmpstr(strfield(encrypted, "joinMethod"), ==, "code");

	/* V2 unavailable falls back to V1. */
	force_v1_ticket = TRUE;
	g_autoptr(J) prev_v1 =
		line_square_find_ticket(&s, "fallbackTicket", &error);
	force_v1_ticket = FALSE;
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(prev_v1, "joinMethod"), ==, "approval");

	checked_join_code = FALSE;
	g_autoptr(J) joined = line_square_join(&s, SQ_MID, CHAT_MID, "Novesce",
					       "secret", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(checked_join_code);
	g_assert_false(json_object_get_boolean(field(joined, "pending")));

	g_autoptr(J) pending = line_square_join(&s, SQ_MID, NULL, "Novesce",
						NULL, "Hello!", &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(pending, "pending")));

	g_autoptr(J) created = line_square_create(&s, "New Club", "Desc",
						  "Welcome", "Owner", 1, TRUE,
						  0, NULL, NULL, 5, &error);
	g_assert_no_error(error);
	g_assert_nonnull(field(created, "chat"));

	g_autoptr(J) sub =
		line_square_create_chat(&s, SQ_MID, "Off-topic", 6, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(sub, "id"), ==, SUB_MID);

	g_autoptr(J) jsub = line_square_join_chat(&s, SUB_MID, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(jsub, "id"), ==, SUB_MID);

	g_autoptr(J) det = line_square_detail(&s, SQ_MID, CHAT_MID, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(det, "name"), ==, "Linux Club");
	g_assert_true(json_object_get_boolean(field(det, "canEdit")));

	g_autoptr(J) link = line_square_invite_link(&s, CHAT_MID, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(link, "url"), ==,
			"https://line.me/ti/g2/freshTicket99");

	g_autoptr(J) cur_sq = json_tokener_parse(
		"{\"1\":\"" SQ_MID "\",\"2\":\"Linux Club\",\"9\":1}");
	g_autoptr(J) upd_p = json_tokener_parse(
		"{\"name\":\"Renamed Club\",\"searchable\":false}");
	g_autoptr(J) upd = line_square_update(&s, cur_sq, upd_p, &error);
	g_assert_no_error(error);
	g_assert_nonnull(upd);

	g_autoptr(J) cur_ch = json_tokener_parse("{\"1\":\"" CHAT_MID
						 "\",\"2\":\"" SQ_MID
						 "\",\"4\":\"Main\",\"7\":3}");
	g_autoptr(J) upd_ch =
		line_square_update_chat(&s, cur_ch, "Renamed Room", &error);
	g_assert_no_error(error);
	g_assert_nonnull(upd_ch);

	g_autoptr(J) cur_mem = json_tokener_parse("{\"1\":\"" ME_MEM
						  "\",\"2\":\"" SQ_MID
						  "\",\"3\":\"Old\",\"9\":5}");
	g_autoptr(J) upd_mem = line_square_update_my_profile(
		&s, cur_mem, "NewNick", "Bio", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(upd_mem, "myName"), ==, "NewNick");

	g_autoptr(J) mems =
		line_square_members(&s, SQ_MID, NULL, NULL, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(mems, "cursor"), ==, "page2");

	g_autoptr(J) mod = line_square_moderate_member(&s, SQ_MID, PEER_MEM,
						       "promote", &error);
	g_assert_no_error(error);
	g_assert_nonnull(mod);

	g_autoptr(J) appr = line_square_answer_join_request(
		&s, SQ_MID, PEER_MEM, TRUE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(appr);

	g_autoptr(J) left = line_square_leave(&s, SQ_MID, &error);
	g_assert_no_error(error);
	g_assert_nonnull(left);

	/* Live sync via fetchMyEvents updates subscription_id and sync_token. */
	gint64 sub_id = 0;
	g_autofree char *sync_tok = NULL;

	g_autoptr(J) my_map =
		json_tokener_parse("{\"" SQ_MID "\":{\"1\":\"" ME_MEM "\"}}");
	g_autoptr(J) evs =
		line_square_poll_events(&s, &sub_id, &sync_tok, my_map, &error);
	g_assert_no_error(error);
	g_assert_cmpint(sub_id, ==, 555);
	g_assert_cmpstr(sync_tok, ==, "sync-2");
	g_assert_cmpuint(json_object_array_length(evs), ==, 1);

	/* SquareException resets s->service_code to -1. */
	force_sq_error = TRUE;
	sq_error_code = 403;
	g_assert_null(line_square_leave(&s, SQ_MID, &error));
	g_assert_nonnull(error);
	g_assert_cmpstr(error->message, ==,
			"You do not have permission for this OpenChat action.");
	g_assert_cmpint(s.service_code, ==, -1);

	json_object_put(s.pictures);
	g_clear_pointer(&last_method, g_free);
}

static void test_discover_and_forward(void)
{
	Line s = { .mid = SELF_MID,
		   .access = "offline-token",
		   .pictures = obj() };

	g_autoptr(GError) error = NULL;

	g_autoptr(J) kws = line_square_popular_keywords(&s, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(kws), ==, 2);
	g_assert_cmpstr(
		json_object_get_string(json_object_array_get_idx(kws, 0)), ==,
		"Linux");

	force_no_xlt_keywords = TRUE;
	g_autoptr(J) kws_fallback = line_square_popular_keywords(&s, &error);
	force_no_xlt_keywords = FALSE;
	g_assert_no_error(error);
	g_assert_nonnull(kws_fallback);
	g_assert_cmpuint(json_object_array_length(kws_fallback), ==, 0);

	g_autoptr(J) found = line_square_search(&s, "linux", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(found, "cursor"), ==, "next2");
	J *items = field(found, "squares");

	g_assert_cmpuint(json_object_array_length(items), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(items, 0), "name"),
			==, "Linux Club");
	g_assert_cmpstr(strfield(json_object_array_get_idx(items, 0),
				 "joinMethod"),
			==, "code");

	force_public_search = TRUE;
	g_autoptr(J) public = line_square_search(&s, "linux", NULL, &error);
	force_public_search = FALSE;
	g_assert_no_error(error);
	g_assert_nonnull(public);
	J *public_items = field(public, "squares");

	g_assert_cmpuint(json_object_array_length(public_items), ==, 1);
	J *public_row = json_object_array_get_idx(public_items, 0);

	g_assert_cmpstr(
		strfield(public_row, "invitationUrl"), ==,
		"line://square/home?encryptedSquareMid=emidTicket_1234567890&utm_source=line-search");
	g_assert_cmpstr(strfield(public_row, "memberCountText"), ==,
			"81 members");
	g_assert_cmpstr(strfield(public, "cursor"), ==,
			"?q=linux&sscode=os.tab.square&start=30&rank=31");
	g_autoptr(J) page2 = line_square_search_service(
		&s, "linux", strfield(public, "cursor"), &error);

	g_assert_no_error(error);
	g_assert_nonnull(page2);
	g_assert_cmpuint(json_object_array_length(field(page2, "squares")), ==,
			 1);
	anonymous_search = TRUE;
	g_autoptr(J) anonymous =
		line_square_search_service(&s, "linux", NULL, &error);
	anonymous_search = FALSE;
	g_assert_no_error(error);
	g_assert_nonnull(anonymous);
	g_assert_cmpuint(json_object_array_length(field(anonymous, "squares")),
			 ==, 1);
	g_assert_null(line_square_search_service(
		&s, "linux", "https://example.com/nope", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_autoptr(J) ments =
		line_square_search_mentionables(&s, CHAT_MID, "Al", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_array_length(ments), ==, 1);
	g_assert_cmpstr(strfield(json_object_array_get_idx(ments, 0), "id"), ==,
			PEER_MEM);

	g_autoptr(J) raw_stk = json_tokener_parse(
		"{\"15\":7,\"18\":{\"STKID\":\"55\",\"STKPKGID\":\"7\",\"STKVER\":\"1\"}}");
	g_autoptr(J) fs = line_square_forward_sticker(&s, CHAT_MID, ME_MEM,
						      raw_stk, 10, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(fs, "id"), ==, "1009");

	g_autoptr(J) raw_loc = json_tokener_parse(
		"{\"15\":15,\"11\":{\"1\":\"Park\",\"2\":\"Main\",\"3\":1.5,\"4\":2.5}}");
	g_autoptr(J) fl = line_square_forward_location(&s, CHAT_MID, ME_MEM,
						       raw_loc, 11, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(fl, "id"), ==, "1009");

	g_autoptr(J) raw_cnt =
		json_tokener_parse("{\"15\":13,\"18\":{\"mid\":\"" SELF_MID
				   "\",\"displayName\":\"Me\"}}");
	g_autoptr(J) fc = line_square_forward_contact(&s, CHAT_MID, ME_MEM,
						      raw_cnt, 12, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(strfield(fc, "id"), ==, "1009");

	json_object_put(s.pictures);
	g_clear_pointer(&last_method, g_free);
}

static void test_peer_reads(void)
{
	Line s = { .mid = SELF_MID, .pictures = obj() };
	g_autoptr(GError) error = NULL;
	g_autofree char *next_sync = NULL;
	gboolean changed = FALSE;
	g_autoptr(J) ranges = line_square_peer_ranges(
		&s, CHAT_MID, ME_MEM, NULL, &next_sync, &changed, &error);

	g_assert_no_error(error);
	g_assert_nonnull(ranges);
	g_assert_false(changed);
	g_assert_cmpstr(next_sync, ==, "read-sync-1");
	g_assert_null(field(ranges, ME_MEM));
	g_assert_cmpint(
		json_object_get_int64(field(field(ranges, PEER_MEM), "2")), ==,
		1005);

	g_autofree char *live_sync = NULL;
	g_autoptr(J) delta =
		line_square_peer_ranges(&s, CHAT_MID, ME_MEM, "read-sync-live",
					&live_sync, &changed, &error);
	g_assert_no_error(error);
	g_assert_nonnull(delta);
	g_assert_cmpstr(live_sync, ==, "read-sync-2");
	g_assert_cmpint(
		json_object_get_int64(field(field(delta, PEER_MEM), "2")), ==,
		1010);
	json_object_put(s.pictures);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/square/tickets", test_tickets);
	g_test_add_func("/square/group-ticket", test_group_ticket);
	g_test_add_func("/square/chats-and-history", test_chats_and_history);
	g_test_add_func("/square/event-directory", test_event_directory);
	g_test_add_func("/square/messaging", test_messaging);
	g_test_add_func("/square/lifecycle-and-sync", test_lifecycle_and_sync);
	g_test_add_func("/square/discover-and-forward",
			test_discover_and_forward);
	g_test_add_func("/square/peer-reads", test_peer_reads);
	return g_test_run();
}
