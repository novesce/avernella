// SPDX-License-Identifier: GPL-3.0-or-later
#include "liff.h"
#include "liff_url.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIFF_ID "1661444788-rnvmoOo8"
#define CHAT "u00000000000000000000000000000001"

struct rpc_step {
	const char *path, *method, *request, *reply;
};
static const struct rpc_step *script;
static guint calls, nsteps;

static void use(const struct rpc_step *steps, guint n)
{
	script = steps;
	nsteps = n;
	calls = 0;
}

/* Only HTTP is substituted; replies go through the real Thrift decoder. */
J *line_rpc(Line *s, const char *path, const char *method, J *args,
	    GError **error)
{
	g_assert_cmpuint(calls, <, nsteps);
	const struct rpc_step *step = &script[calls++];

	g_assert_cmpstr(path, ==, step->path);
	g_assert_cmpstr(method, ==, step->method);
	g_autoptr(GError) validation = NULL;
	g_autoptr(GBytes) call = line_encode_call(method, args, &validation);
	g_assert_no_error(validation);
	gsize bytes;
	const guint8 *wire = g_bytes_get_data(call, &bytes);
	gsize header = 4 + strlen(method);

	g_assert_cmpuint(bytes, >, header);
	g_autoptr(J) request =
		line_decode_struct(wire + header, bytes - header, &validation);
	g_assert_no_error(validation);
	g_autoptr(J) expected = json_tokener_parse(step->request);
	if (!json_object_equal(expected, request))
		g_error("Unexpected arguments for %s: %s", method,
			json_object_to_json_string(request));
	s->service_code = -1;
	g_autoptr(J) synthetic = json_tokener_parse(step->reply);
	g_autoptr(GBytes) result =
		line_encode_call(method, synthetic, &validation);
	g_assert_no_error(validation);
	wire = g_bytes_get_data(result, &bytes);
	g_autofree guint8 *response = g_memdup2(wire, bytes);

	response[1] = 0x41; /* reply */
	return line_decode_reply(method, response, bytes, &s->service_code,
				 error);
}

static void test_recognizes_liff_urls(void)
{
	static const char *const yes[] = {
		"https://liff.line.me/" LIFF_ID,
		"https://LIFF.line.me/" LIFF_ID "/coupon?id=7#top",
		"https://miniapp.line.me/" LIFF_ID "?x=1",
		"https://line.me/R/app/" LIFF_ID "/a",
		"line://app/" LIFF_ID,
	};
	static const char *const no[] = {
		"https://liff.line.me/",
		"https://liff.line.me/abc",
		"https://liff.line.me/1661444788-",
		"https://liff.line.me/-rnvmoOo8",
		"https://liff.line.me/" LIFF_ID "x-y!",
		"https://liff.line.me/" LIFF_ID "/a b",
		"http://liff.line.me/" LIFF_ID,
		"https://liff.line.me.evil.test/" LIFF_ID,
		"https://crm.a-ramen.com/alertPage",
		"line://ti/p/@ramen",
		NULL,
	};

	for (gsize i = 0; i < G_N_ELEMENTS(yes); i++)
		g_assert_true(line_liff_url(yes[i], NULL, NULL));
	for (gsize i = 0; i < G_N_ELEMENTS(no); i++)
		g_assert_false(line_liff_url(no[i], NULL, NULL));
	g_autofree char *id = NULL, *rest = NULL;

	g_assert_true(line_liff_url(yes[1], &id, &rest));
	g_assert_cmpstr(id, ==, LIFF_ID);
	g_assert_cmpstr(rest, ==, "/coupon?id=7#top");
	g_assert_true(line_liff_line_scheme("line://ti/p/@ramen"));
	g_assert_false(line_liff_line_scheme("https://line.me/"));
}

static void test_builds_launch_url(void)
{
	g_autoptr(J) tokens = json_tokener_parse(
		"{\"2\":\"ctx.eyJ0IjoidXRvdSJ9.sig\",\"3\":\"acc+/=\","
		"\"4\":\"feat\",\"6\":\"1661444788\",\"7\":\"id.tok.en\"}");
	g_autofree char *plain = line_liff_launch_url(
		"https://crm.a-ramen.com/liff/", "", tokens);

	g_assert_cmpstr(
		plain, ==,
		"https://crm.a-ramen.com/liff/#context_token=ctx.eyJ0IjoidXRvdSJ9.sig"
		"&feature_token=feat&access_token=acc%2B%2F%3D&id_token=id.tok.en"
		"&client_id=1661444788");
	g_autofree char *deep =
		line_liff_launch_url("https://crm.a-ramen.com/liff/?shop=1",
				     "/coupon/7?from=oa#c", tokens);

	g_assert_true(g_str_has_prefix(
		deep, "https://crm.a-ramen.com/liff/coupon/7?shop=1&from=oa"
		      "#context_token="));
	g_assert_true(g_str_has_suffix(deep, "&client_id=1661444788&c"));
	g_assert_null(
		line_liff_launch_url("http://crm.a-ramen.com/", "", tokens));
	g_assert_null(line_liff_launch_url("javascript:alert(1)", "", tokens));
	g_assert_null(line_liff_launch_url(NULL, "", tokens));
}

#define VIEW_REQUEST(context)                                    \
	"{\"1\":{\"1\":\"" LIFF_ID "\",\"2\":" context ",\"3\":" \
	"\"ja_JP\"}}"

static J *open_view(Line *s, const char *url, const char *chat, GError **error)
{
	g_autoptr(GHashTable) chats = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(J) params = obj();

	g_hash_table_add(chats, (gpointer)CHAT);
	json_object_object_add(params, "url", json_object_new_string(url));
	if (chat)
		json_object_object_add(params, "chat",
				       json_object_new_string(chat));
	return line_liff_view(s, chats, params, error);
}

/* Unknown, former and malformed contexts are rejected before the RPC. */
static void test_chat_authorization(void)
{
	g_autoptr(GHashTable) chats = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(J) params = obj();
	Line s = { 0 };
	J *contexts[] = {
		json_object_new_string(CHAT),
		json_object_new_string(""),
		json_object_new_int(1),
		NULL,
		json_object_new_string_len(CHAT "\0x", sizeof(CHAT) + 1),
	};

	json_object_object_add(params, "url",
			       json_object_new_string("line://app/" LIFF_ID));
	use(NULL, 0);
	for (gsize i = 0; i < G_N_ELEMENTS(contexts); i++) {
		g_autoptr(GError) error = NULL;

		if (i)
			g_hash_table_add(chats, (gpointer)CHAT);
		json_object_object_add(params, "chat", contexts[i]);
		g_assert_null(line_liff_view(&s, chats, params, &error));
		g_assert_error(error, LINE_ERROR, 1);
	}
	g_hash_table_remove(chats, CHAT);
	json_object_object_add(params, "chat", json_object_new_string(CHAT));
	g_autoptr(GError) error = NULL;

	g_assert_null(line_liff_view(&s, chats, params, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_assert_cmpuint(calls, ==, 0);
}

static void test_issues_view(void)
{
	static const struct rpc_step steps[] = {
		{ "/LIFF1", "issueLiffView",
		  VIEW_REQUEST("{\"2\":{\"1\":\"" CHAT "\"}}"),
		  "{\"0:obj\":{\"1:obj\":{\"1:str\":\"TALL\",\"2:str\":"
		  "\"https://crm.a-ramen.com/liff\",\"22:str\":\"A Ramen\"},"
		  "\"2:str\":\"ctx\",\"3:str\":\"acc\",\"4:str\":\"feat\","
		  "\"6:str\":\"1661444788\",\"7:str\":\"idt\","
		  "\"15:str\":\"" LIFF_ID "\"}}" },
	};
	g_autoptr(GError) error = NULL;
	Line s = { 0 };

	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(J) view = open_view(
		&s, "https://liff.line.me/" LIFF_ID "/coupon", CHAT, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(calls, ==, 1);
	g_assert_cmpstr(strfield(view, "url"), ==,
			"https://crm.a-ramen.com/liff/coupon#context_token=ctx"
			"&feature_token=feat&access_token=acc&id_token=idt"
			"&client_id=1661444788");
	g_assert_cmpstr(strfield(view, "size"), ==, "tall");
	g_assert_cmpstr(strfield(view, "title"), ==, "A Ramen");
}

static void test_opens_consent_page(void)
{
	static const struct rpc_step steps[] = {
		{ "/LIFF1", "issueLiffView", VIEW_REQUEST("{\"1\":{}}"),
		  "{\"1:obj\":{\"1:i32\":3,\"2:str\":\"consent required\","
		  "\"3:obj\":{\"3:obj\":{\"1:str\":\"1661444788\",\"2:str\":"
		  "\"https://access.line.me/oauth2/v2.1/consent?x=1\"}}}}" },
		{ "/LIFF1", "issueLiffView", VIEW_REQUEST("{\"1\":{}}"),
		  "{\"1:obj\":{\"1:i32\":3,\"3:obj\":{\"3:obj\":{\"2:str\":"
		  "\"javascript:alert(1)\"}}}}" },
		{ "/LIFF1", "issueLiffView", VIEW_REQUEST("{\"1\":{}}"),
		  "{\"1:obj\":{\"1:i32\":9,\"2:str\":\"terminated\"}}" },
		{ "/LIFF1", "issueLiffView", VIEW_REQUEST("{\"1\":{}}"),
		  "{\"0:obj\":{\"1:obj\":{\"2:str\":\"http://plain.test/\"}}}" },
	};
	Line s = { 0 };

	use(steps, G_N_ELEMENTS(steps));
	g_autoptr(GError) error = NULL;
	g_autoptr(J) consent =
		open_view(&s, "line://app/" LIFF_ID, NULL, &error);

	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean(field(consent, "web")));
	g_assert_cmpstr(strfield(consent, "url"), ==,
			"https://access.line.me/oauth2/v2.1/consent?x=1");
	g_autoptr(J) unsafe =
		open_view(&s, "line://app/" LIFF_ID, NULL, &error);

	g_assert_null(unsafe);
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_null(open_view(&s, "line://app/" LIFF_ID, NULL, &error));
	g_assert_cmpstr(error->message, ==,
			"This LINE app has ended its service.");
	g_clear_error(&error);
	g_assert_null(open_view(&s, "line://app/" LIFF_ID, NULL, &error));
	g_assert_error(error, LINE_ERROR, 1);
	g_clear_error(&error);
	g_assert_null(open_view(&s, "https://example.test/", NULL, &error));
	g_assert_cmpuint(calls, ==, 4);
}

#define ACCOUNT_A \
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ACCOUNT_B \
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void test_storage_accounts(void)
{
	g_autofree char *base = g_dir_make_tmp("liff-store-XXXXXX", NULL);
	g_autofree char *home = g_build_filename(base, "avernella", NULL);
	g_autofree char *legacy = g_build_filename(home, "liff", NULL);
	g_autofree char *shared = g_build_filename(legacy, "marker", NULL);
	g_autofree char *profiles =
		g_build_filename(home, "liff-profiles", NULL);
	struct stat st;

	g_assert_cmpint(g_mkdir_with_parents(legacy, 0700), ==, 0);
	g_assert_true(g_file_set_contents(shared, "old", 3, NULL));
	g_assert_null(line_liff_storage_dir(base, NULL));
	g_assert_null(line_liff_storage_dir(base, "../liff"));
	g_assert_false(g_file_test(profiles, G_FILE_TEST_EXISTS));
	g_autofree char *a = line_liff_storage_dir(base, ACCOUNT_A);
	g_autofree char *b = line_liff_storage_dir(base, ACCOUNT_B);
	g_autofree char *again = line_liff_storage_dir(base, ACCOUNT_A);

	g_assert_nonnull(a);
	g_assert_nonnull(b);
	g_assert_cmpstr(a, !=, b);
	g_assert_cmpstr(again, ==, a);
	g_autofree char *first = g_build_filename(a, "marker", NULL);
	g_autofree char *second = g_build_filename(b, "marker", NULL);

	g_assert_false(g_file_test(first, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_set_contents(first, "test", 4, NULL));
	g_assert_false(g_file_test(second, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(shared, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(lstat(a, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(lstat(profiles, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(g_remove(first), ==, 0);
	g_assert_cmpint(g_remove(shared), ==, 0);
	g_assert_cmpint(g_rmdir(a), ==, 0);
	g_assert_cmpint(g_rmdir(b), ==, 0);
	g_assert_cmpint(g_rmdir(profiles), ==, 0);
	g_assert_cmpint(g_rmdir(legacy), ==, 0);
	g_assert_cmpint(g_rmdir(home), ==, 0);
	g_assert_cmpint(g_rmdir(base), ==, 0);
}

static void test_storage_validation(void)
{
	g_autofree char *base = g_dir_make_tmp("liff-store-XXXXXX", NULL);
	g_autofree char *home = g_build_filename(base, "avernella", NULL);
	g_autofree char *root = g_build_filename(home, "liff-profiles", NULL);
	g_autofree char *target = g_build_filename(root, ACCOUNT_A, NULL);
	g_autofree char *elsewhere = g_build_filename(base, "elsewhere", NULL);
	g_autofree char *made = line_liff_storage_dir(base, ACCOUNT_A);
	struct stat st;

	g_assert_cmpstr(made, ==, target);
	g_assert_cmpint(chmod(target, 0755), ==, 0);
	g_autofree char *loose = line_liff_storage_dir(base, ACCOUNT_A);

	g_assert_cmpstr(loose, ==, target);
	g_assert_cmpint(lstat(target, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(chmod(target, 0777), ==, 0);
	g_assert_null(line_liff_storage_dir(base, ACCOUNT_A));
	g_assert_cmpint(g_rmdir(target), ==, 0);
	g_assert_cmpint(g_mkdir(elsewhere, 0700), ==, 0);
	g_assert_cmpint(symlink(elsewhere, target), ==, 0);
	g_assert_null(line_liff_storage_dir(base, ACCOUNT_A));
	g_assert_cmpint(g_remove(target), ==, 0);
	g_assert_true(g_file_set_contents(target, "", 0, NULL));
	g_assert_null(line_liff_storage_dir(base, ACCOUNT_A));
	g_assert_cmpint(g_remove(target), ==, 0);
	g_assert_cmpint(g_rmdir(root), ==, 0);
	g_assert_cmpint(symlink(elsewhere, root), ==, 0);
	g_assert_null(line_liff_storage_dir(base, ACCOUNT_A));
	g_assert_cmpint(g_remove(root), ==, 0);
	g_assert_cmpint(chmod(home, 0777), ==, 0);
	g_assert_null(line_liff_storage_dir(base, ACCOUNT_A));
	g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_rmdir(home), ==, 0);
	g_assert_cmpint(symlink(elsewhere, home), ==, 0);
	g_assert_null(line_liff_storage_dir(base, ACCOUNT_A));
	g_assert_cmpint(g_remove(home), ==, 0);
	g_assert_cmpint(g_rmdir(elsewhere), ==, 0);
	g_assert_cmpint(g_rmdir(base), ==, 0);
}

static void test_navigation_policy(void)
{
	static const char *const debug_ok[] = {
		"http://127.0.0.1:8080/",
		"http://127.0.0.1:65535",
		"HTTP://127.0.0.1:1024/page?x=1#frag",
	};
	static const char *const debug_bad[] = {
		"http://127.0.0.1:80@example.com/",
		"http://127.0.0.1:8080@example.com/",
		"http://127.0.0.1:8080\\@example.com/",
		"http://127.0.0.1:8080.example.com/",
		"http://127.0.0.1:/",
		"http://127.0.0.1:80/",
		"http://127.0.0.1:65536/",
		"http://127.0.0.1:123456/",
		"http://127.0.0.1:8080 /",
		"http://user@127.0.0.1:8080/",
		"http://127.0.0.10:8080/",
		"https://127.0.0.1:8080/",
		"http://localhost:8080/",
		"http://example.com/",
		"",
		NULL,
	};

	for (gsize i = 0; i < G_N_ELEMENTS(debug_ok); i++) {
		g_assert_true(line_liff_debug_url(debug_ok[i]));
		g_assert_true(line_liff_web_url(debug_ok[i], TRUE));
		g_assert_false(line_liff_web_url(debug_ok[i], FALSE));
	}
	for (gsize i = 0; i < G_N_ELEMENTS(debug_bad); i++) {
		g_assert_false(line_liff_debug_url(debug_bad[i]));
		if (debug_bad[i] && !g_str_has_prefix(debug_bad[i], "https:"))
			g_assert_false(line_liff_web_url(debug_bad[i], TRUE));
	}
	/* The page stays on https; WebKit's own documents still load. */
	g_assert_true(line_liff_web_url("https://liff.line.me/1-a", FALSE));
	g_assert_true(line_liff_web_url("HTTPS://example.com/", FALSE));
	g_assert_true(line_liff_web_url("about:blank", FALSE));
	g_assert_true(line_liff_web_url("data:text/html,", FALSE));
	g_assert_true(line_liff_web_url("blob:https://example.com/1", FALSE));
	g_assert_false(line_liff_web_url("http://example.com/", FALSE));
	g_assert_false(line_liff_web_url("http://example.com/", TRUE));
	g_assert_false(line_liff_web_url("HTTP://example.com/", FALSE));
	g_assert_false(line_liff_web_url("ftp://example.com/", FALSE));
	g_assert_false(line_liff_web_url("file:///etc/passwd", TRUE));
	g_assert_false(line_liff_web_url("javascript:alert(1)", FALSE));
	g_assert_false(line_liff_web_url(NULL, FALSE));
}

int main(int argc, char **argv)
{
	g_setenv("LANGUAGE", "ja_JP.UTF-8", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/liff/recognizes-liff-urls",
			test_recognizes_liff_urls);
	g_test_add_func("/liff/builds-launch-url", test_builds_launch_url);
	g_test_add_func("/liff/issues-view", test_issues_view);
	g_test_add_func("/liff/chat-authorization", test_chat_authorization);
	g_test_add_func("/liff/opens-consent-page", test_opens_consent_page);
	g_test_add_func("/liff/storage-accounts", test_storage_accounts);
	g_test_add_func("/liff/storage-validation", test_storage_validation);
	g_test_add_func("/liff/navigation-policy", test_navigation_policy);
	return g_test_run();
}
