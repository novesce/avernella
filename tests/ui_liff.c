// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_liff.h"
#include "ui_private.h"
#include "liff_url.h"
#include "line.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define APP_LINK "line://app/1234-test"
#define CHAT_A "u00000000000000000000000000000001"
#define CHAT_B "u00000000000000000000000000000002"

Interface ui;
static char *fake_self;
static Json *request;
static guint request_tag;

ssize_t __real_readlink(const char *path, char *buf, size_t size);

ssize_t __wrap_readlink(const char *path, char *buf, size_t size)
{
	if (fake_self && !strcmp(path, "/proc/self/exe")) {
		size_t len = MIN(strlen(fake_self), size);

		memcpy(buf, fake_self, len);
		return len;
	}
	return __real_readlink(path, buf, size);
}

void ui_rpc(const char *method, Json *params, guint tag)
{
	g_assert_cmpstr(method, ==, "liffView");
	g_clear_pointer(&request, json_object_put);
	request = params;
	request_tag = tag;
}

const char *ui_tr(const char *english)
{
	return english;
}

Json *ui_field(Json *obj, const char *key)
{
	return field(obj, key);
}

const char *ui_string(Json *obj, const char *key, const char *fallback)
{
	const char *s = strfield(obj, key);

	return s ? s : fallback;
}

void ui_status(const char *s)
{
	g_free(ui.notice);
	ui.notice = g_strdup(s);
}

gboolean line_flex_link_allowed(const char *url)
{
	(void)url;
	return FALSE;
}

void ui_openchat_open_link(const char *url)
{
	(void)url;
	g_assert_not_reached();
}

void ui_openchat_open_group_link(const char *url)
{
	(void)url;
	g_assert_not_reached();
}

void ui_friends_open_link(const char *url)
{
	(void)url;
	g_assert_not_reached();
}

void ui_select_chat_id(const char *id)
{
	(void)id;
	g_assert_not_reached();
}

static int helper(const char *profile)
{
	static const char ready[] = "B\tready";
	static const char link[] = "O\t" APP_LINK;

	g_autofree char *expected =
		g_compute_checksum_for_string(G_CHECKSUM_SHA256, CHAT_A, -1);
	char packet[128];
	ssize_t n = recv(LINE_LIFF_FD, packet, sizeof(packet) - 1, 0);

	if (n <= 0 || strcmp(profile, expected))
		return 1;
	packet[n] = 0;
	if ((strcmp(packet, "L\tfull\thttps://example.test/") &&
	     strcmp(packet, "W\tfull\thttps://example.test/consent")) ||
	    send(LINE_LIFF_FD, ready, sizeof(ready) - 1, 0) < 0 ||
	    send(LINE_LIFF_FD, link, sizeof(link) - 1, 0) < 0)
		return 1;
	/* The real parent closes its end when a session ends. */
	return recv(LINE_LIFF_FD, packet, sizeof(packet), 0) == 0 ? 0 : 1;
}

static void wait_for_link(guint tag)
{
	gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	while (request_tag == tag && g_get_monotonic_time() < deadline) {
		SDL_Event event;

		if (SDL_WaitEventTimeout(&event, 200))
			ui_liff_event(&event);
	}
	g_assert_cmpuint(request_tag, !=, tag);
	g_assert_null(ui_field(request, "chat"));
}

static void test_chat_context(void)
{
	g_autofree char *binary = g_file_read_link("/proc/self/exe", NULL);
	g_autofree char *dir = g_dir_make_tmp("avernella-context-XXXXXX", NULL);
	g_autofree char *browser =
		g_build_filename(dir, "avernella-liff-gtk", NULL);

	g_assert_nonnull(binary);
	g_assert_nonnull(dir);
	g_assert_cmpint(symlink(binary, browser), ==, 0);
	fake_self = g_build_filename(dir, "avernella", NULL);
	g_assert_cmpint(SDL_Init(SDL_INIT_EVENTS), ==, 0);
	ui.connected = TRUE;
	ui.self_id = g_strdup(CHAT_A);
	ui.selected = g_strdup(CHAT_A);
	ui_liff_open_link(APP_LINK);
	g_assert_nonnull(request);
	g_assert_cmpstr(ui_string(request, "chat", NULL), ==, CHAT_A);
	guint original = request_tag;

	/* Switching chats while the worker opens the first one is harmless. */
	g_free(ui.selected);
	ui.selected = g_strdup(CHAT_B);
	g_autoptr(J) page = json_tokener_parse(
		"{\"url\":\"https://example.test/\",\"size\":\"full\"}");
	g_assert_true(ui_liff_reply("liffView", original, page, NULL));
	wait_for_link(original);
	/* The consent view gets a fresh, ordinary helper. */
	g_autoptr(J) consent = json_tokener_parse(
		"{\"url\":\"https://example.test/consent\",\"web\":true}");
	guint next = request_tag;

	g_assert_true(ui_liff_reply("liffView", next, consent, NULL));
	wait_for_link(next);

	/* External activation also carries no selected conversation. */
	ui_open_chat_link(APP_LINK, NULL);
	g_assert_null(ui_field(request, "chat"));
	guint external = request_tag;

	ui_liff_open_link(APP_LINK);
	g_assert_cmpstr(ui_string(request, "chat", NULL), ==, CHAT_B);
	g_assert_true(ui_liff_reply("liffView", external, NULL, "stale"));
	g_assert_cmpstr(ui.notice, !=, "stale");
	g_assert_true(ui_liff_reply("liffView", request_tag, NULL, "current"));
	g_assert_cmpstr(ui.notice, ==, "current");
	ui_liff_close();
	g_assert_true(ui_liff_reply("liffView", request_tag, NULL, "ended"));
	g_assert_cmpstr(ui.notice, ==, "current");

	SDL_Event event;

	while (SDL_PollEvent(&event))
		ui_liff_event(&event);
	SDL_Quit();
	g_clear_pointer(&request, json_object_put);
	g_clear_pointer(&ui.selected, g_free);
	g_clear_pointer(&ui.self_id, g_free);
	g_clear_pointer(&ui.notice, g_free);
	g_clear_pointer(&fake_self, g_free);
	g_assert_cmpint(g_remove(browser), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[0], "avernella-liff"))
		return helper(argv[2]);
	g_unsetenv("WAYLAND_DISPLAY");
	g_unsetenv("WAYLAND_SOCKET");
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui/liff/chat-context", test_chat_context);
	return g_test_run();
}
