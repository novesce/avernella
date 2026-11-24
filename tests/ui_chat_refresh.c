// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "ui_friends.h"
#include "state.h"
#include <stdio.h>
#include <unistd.h>

int line_ui(int argc, char **argv);
int __real_SDL_PollEvent(SDL_Event *ev);

static const char chat[] = "u00000000000000000000000000000001";
static const char draft[] = "Still composing…";
static enum {
	OPEN_CHAT,
	CHECK_RECONNECT,
	CHECK_MANUAL_REFRESH,
	WAIT_FOR_OUTAGE,
	WAIT_FOR_RECONNECT,
	CHECK_OLDER_PAGE,
	WAIT_FOR_BARRIER,
	FINISHED
} step;

gboolean line_store_has_session(void)
{
	return TRUE;
}

/* The UI spawns this executable with --worker using its normal IPC transport. */
static int worker(void)
{
	g_autofree char *line = NULL;
	size_t size = 0;
	int chats = 0, histories = 0;

	while (getline(&line, &size, stdin) >= 0) {
		g_autoptr(J) req = json_tokener_parse(line);
		const char *method = ui_string(req, "method", "");
		int id = json_object_get_int(ui_field(req, "id"));

		if (!strcmp(method, "chats")) {
			chats++;
			printf("{\"id\":%d,\"result\":[{\"id\":\"%s\","
			       "\"name\":\"Refresh test\",\"lastId\":\"%d\"}]}\n",
			       id, chat, MIN(chats, 3));
		} else if (!strcmp(method, "history")) {
			int rows = ++histories == 3 ? 50 : 1;

			g_assert_cmpstr(ui_string(ui_field(req, "params"),
						  "chat", NULL),
					==, chat);
			g_assert_cmpint(histories, ==, chats);
			g_assert_cmpint(histories, <=, 3);
			printf("{\"id\":%d,\"result\":[", id);
			for (int i = 0; i < rows; i++)
				printf("%s{\"id\":\"%d\",\"text\":\"Page %d\"}",
				       i ? "," : "", i + 1, histories);
			puts("]}");
			if (histories == 1)
				puts("{\"event\":\"reconnected\"}");
		} else if (!strcmp(method, "olderHistory")) {
			printf("{\"id\":%d,\"result\":{\"messages\":[{\"id\":\"0\","
			       "\"text\":\"Earlier message\"}]}}\n",
			       id);
			puts("{\"event\":\"sync_unavailable\",\"message\":\"Offline\"}");
			puts("{\"event\":\"reconnected\"}");
		} else if (!strcmp(method, "finishCheck")) {
			/* All requests from the last refresh have now arrived. */
			g_assert_cmpint(chats, ==, 4);
			g_assert_cmpint(histories, ==, 3);
			puts("{\"event\":\"status\",\"message\":\"Check complete\"}");
		} else {
			printf("{\"id\":%d,\"result\":{}}\n", id);
		}
		fflush(stdout);
	}
	return 0;
}

/* Run on the UI thread after each input or worker event is consumed. */
int __wrap_SDL_PollEvent(SDL_Event *ev)
{
	Json *room = ui.chats ? json_object_array_get_idx(ui.chats, 0) : NULL;
	Json *row = ui.history ? json_object_array_get_idx(ui.history, 0) :
				 NULL;
	const char *text = ui_string(row, "text", "");
	SDL_Event input = { 0 };

	if (step != OPEN_CHAT) {
		g_assert_cmpstr(ui.message.value->str, ==, draft);
		g_assert_cmpstr(ui.selected, ==, chat);
	}
	if (step == OPEN_CHAT && room) {
		ui_open_conversation(room);
		ui_insert(&ui.message, draft);
		step++;
	} else if (step == CHECK_RECONNECT && !strcmp(text, "Page 2")) {
		g_assert_cmpstr(ui_string(room, "lastId", NULL), ==, "2");
		input.type = SDL_MOUSEBUTTONDOWN;
		input.button.button = SDL_BUTTON_LEFT;
		input.button.x = ui.reload.x + ui.reload.w / 2;
		input.button.y = ui.reload.y + ui.reload.h / 2;
		step++;
	} else if (step == CHECK_MANUAL_REFRESH && !strcmp(text, "Page 3")) {
		g_assert_true(ui.has_older);
		input.type = SDL_KEYDOWN;
		input.key.keysym.sym = SDLK_PAGEUP;
		input.key.keysym.mod = KMOD_CTRL;
		step++;
	} else if (step == WAIT_FOR_OUTAGE &&
		   !g_strcmp0(ui.notice, "Offline")) {
		step++;
	} else if (step == WAIT_FOR_RECONNECT && ui.getting_chats) {
		step++;
	} else if (step == CHECK_OLDER_PAGE && !ui.getting_chats) {
		g_assert_cmpstr(ui.notice, ==, "");
		g_assert_cmpstr(ui_string(room, "lastId", NULL), ==, "3");
		g_assert_true(ui.older_page);
		g_assert_cmpstr(text, ==, "Earlier message");
		ui_rpc("finishCheck", json_object_new_object(), 0);
		step++;
	} else if (step == WAIT_FOR_BARRIER &&
		   !g_strcmp0(ui.notice, "Check complete")) {
		g_assert_true(ui.older_page);
		g_assert_cmpstr(text, ==, "Earlier message");
		input.type = SDL_QUIT;
		step++;
	}
	if (input.type)
		g_assert_cmpint(SDL_PushEvent(&input), ==, 1);
	return __real_SDL_PollEvent(ev);
}

static void refresh_chat(void)
{
	alarm(20);
	g_assert_cmpint(line_ui(0, NULL), ==, 0);
	alarm(0);
	g_assert_cmpint(step, ==, FINISHED);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--worker"))
		return worker();
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_setenv("AVERNELLA_VULKAN", "sdl", TRUE);
	g_setenv("LINE_DISABLE_TRAY", "1", TRUE);
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui/chat/refresh", refresh_chat);
	return g_test_run();
}
