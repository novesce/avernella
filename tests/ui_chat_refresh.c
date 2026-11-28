// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_private.h"
#include "ui_friends.h"
#include "state.h"
#include <stdio.h>
#include <unistd.h>

int line_ui(int argc, char **argv);
int __real_SDL_PollEvent(SDL_Event *ev);

static const char chat[] = "u00000000000000000000000000000001";
static const char draft[] = "Still composing…\n";
static const char group[] = "c00000000000000000000000000000002";
static const char pending_text[] = "Timed-out text";
static const char intent[] = "e20b4cb9-7071-4521-b115-2a6f23c1dc83";
static gboolean testing_delivery;
static enum {
	OPEN_SAVED_GROUP,
	CHECK_CACHED_HISTORY,
	WAIT_FOR_DELIVERY,
	CHECK_RECEIPT,
	CHECK_SIGN_OUT,
	DONE_DELIVERY
} delivery_step;
static enum {
	OPEN_CHAT,
	WAIT_FOR_NEWLINE,
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
			printf("{\"id\":%d,\"result\":{\"messages\":[{\"id\":\"101\","
			       "\"time\":1000,\"text\":\"Earlier message\"},"
			       "{\"id\":\"102\",\"time\":1001,\"type\":\"sticker\","
			       "\"hasSound\":true}]}}\n",
			       id);
			puts("{\"event\":\"sync_unavailable\",\"message\":\"Offline\"}");
			puts("{\"event\":\"reconnected\"}");
		} else if (!strcmp(method, "finishCheck")) {
			/* All requests from the last refresh have now arrived. */
			g_assert_cmpint(chats, ==, 4);
			g_assert_cmpint(histories, ==, 3);
			for (int i = 101; i <= 102; i++)
				printf("{\"event\":\"message_unsent\",\"chat\":\"%s\","
				       "\"row\":{\"id\":\"%d\",\"type\":\"unsent\","
				       "\"text\":\"Message unsent\",\"time\":0}}\n",
				       chat, i);
			puts("{\"event\":\"status\",\"message\":\"Check complete\"}");
		} else {
			printf("{\"id\":%d,\"result\":{}}\n", id);
		}
		fflush(stdout);
	}
	return 0;
}

static int delivery_worker(void)
{
	g_autofree char *line = NULL;
	size_t size = 0;
	gboolean checking = FALSE, confirmed = FALSE;

	while (getline(&line, &size, stdin) >= 0) {
		g_autoptr(J) req = json_tokener_parse(line);
		const char *method = ui_string(req, "method", "");
		int id = json_object_get_int(ui_field(req, "id"));
		J *args = ui_field(req, "params");

		g_assert_cmpstr(method, !=, "send");
		if (!strcmp(method, "login")) {
			printf("{\"id\":%d,\"result\":{\"offline\":true,"
			       "\"outbox\":[{\"chat\":\"%s\",\"intentId\":\"%s\","
			       "\"status\":\"uncertain\",\"params\":"
			       "{\"text\":\"%s\",\"chat\":\"%s\"}}]}}\n",
			       id, group, intent, pending_text, group);
		} else if (!strcmp(method, "chats")) {
			printf("{\"id\":%d,\"result\":[{\"id\":\"%s\","
			       "\"name\":\"Cached group\",\"lastId\":\"1\","
			       "\"unread\":1}]}\n",
			       id, group);
		} else if (!strcmp(method, "history")) {
			g_assert_cmpstr(ui_string(args, "chat", NULL), ==,
					group);
			printf("{\"id\":%d,\"result\":[{\"id\":\"1\","
			       "\"text\":\"Cached message\"}]}\n",
			       id);
			if (checking && !confirmed) {
				printf("{\"event\":\"send_outcome\",\"chat\":\"%s\","
				       "\"intentId\":\"%s\",\"status\":\"sent\","
				       "\"receipt\":{\"id\":\"2\"}}\n",
				       group, intent);
				confirmed = TRUE;
			}
		} else if (!strcmp(method, "recoverSend")) {
			g_assert_false(checking);
			g_assert_cmpstr(ui_string(args, "chat", NULL), ==,
					group);
			g_assert_cmpstr(ui_string(args, "intentId", NULL), ==,
					intent);
			g_assert_null(ui_field(args, "text"));
			printf("{\"id\":%d,\"result\":{\"pending\":true,"
			       "\"status\":\"uncertain\",\"intentId\":\"%s\"}}\n",
			       id, intent);
			puts("{\"event\":\"reconnected\"}");
			checking = TRUE;
		} else if (!strcmp(method, "logout")) {
			printf("{\"id\":%d,\"result\":{\"confirmed\":true,"
			       "\"archiveCleanupFailed\":true}}\n",
			       id);
		} else {
			printf("{\"id\":%d,\"result\":{}}\n", id);
		}
		fflush(stdout);
	}
	g_assert_true(confirmed);
	return 0;
}

static int delivery_poll(SDL_Event *event)
{
	Json *room = ui.chats ? json_object_array_get_idx(ui.chats, 0) : NULL;
	Json *row = ui.history ? json_object_array_get_idx(ui.history, 0) :
				 NULL;

	if (delivery_step == OPEN_SAVED_GROUP && room) {
		g_assert_true(ui.offline);
		ui_open_conversation(room);
		g_assert_cmpint(ui.delivery, ==, UI_DELIVERY_UNCERTAIN);
		g_assert_cmpstr(ui.delivery_text, ==, pending_text);
		delivery_step++;
	} else if (delivery_step == CHECK_CACHED_HISTORY && row) {
		g_assert_cmpstr(ui_string(row, "text", NULL), ==,
				"Cached message");
		g_assert_cmpstr(ui.notice, ==, "Offline. Showing saved chats.");
		ui_insert(&ui.message, pending_text);
		*event = (SDL_Event){
			.key = { .type = SDL_KEYDOWN,
				 .keysym = { .sym = SDLK_RETURN } },
		};
		delivery_step++;
		return 1;
	} else if (delivery_step == WAIT_FOR_DELIVERY && !ui.offline) {
		g_assert_cmpint(ui.delivery, ==, UI_DELIVERY_UNCERTAIN);
		ui_insert(&ui.message, draft);
		delivery_step++;
	} else if (delivery_step == CHECK_RECEIPT && !ui.delivery) {
		g_assert_cmpuint(json_object_array_length(ui.history), ==, 2);
		Json *sent = json_object_array_get_idx(ui.history, 1);

		g_assert_cmpstr(ui_string(sent, "id", NULL), ==, "2");
		g_assert_cmpstr(ui_string(sent, "text", NULL), ==,
				pending_text);
		g_assert_true(g_str_has_suffix(ui.message.value->str, draft));
		ui_sign_out();
		delivery_step++;
	} else if (delivery_step == CHECK_SIGN_OUT && !ui.connected) {
		g_assert_cmpstr(
			ui.notice, ==,
			"Signed out. Some offline chat data could not be deleted.");
		*event = (SDL_Event){ .type = SDL_QUIT };
		delivery_step++;
		return 1;
	}
	return __real_SDL_PollEvent(event);
}

/* Run on the UI thread after each input or worker event is consumed. */
int __wrap_SDL_PollEvent(SDL_Event *ev)
{
	if (testing_delivery)
		return delivery_poll(ev);
	Json *room = ui.chats ? json_object_array_get_idx(ui.chats, 0) : NULL;
	Json *row = ui.history ? json_object_array_get_idx(ui.history, 0) :
				 NULL;
	const char *text = ui_string(row, "text", "");
	SDL_Event input = { 0 };

	if (step == WAIT_FOR_NEWLINE) {
		g_assert_cmpstr(ui.message.value->str, ==, draft);
		step = CHECK_RECONNECT;
	}
	if (step > WAIT_FOR_NEWLINE) {
		g_assert_cmpstr(ui.message.value->str, ==, draft);
		g_assert_cmpstr(ui.selected, ==, chat);
	}
	if (step == OPEN_CHAT && room) {
		ui_open_conversation(room);
		ui.focus = F_MESSAGE;
		ui_insert(&ui.message, "Still composing…");
		input.key = (SDL_KeyboardEvent){
			.type = SDL_KEYDOWN,
			.state = SDL_PRESSED,
			.keysym = { .sym = SDLK_RETURN, .mod = KMOD_SHIFT },
		};
		step++;
		*ev = input;
		return 1;
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
		g_assert_cmpuint(json_object_array_length(ui.history), ==, 2);
		for (size_t i = 0; i < 2; i++) {
			Json *message =
				json_object_array_get_idx(ui.history, i);

			g_assert_true(ui_is_chat_event(message));
			g_assert_cmpstr(ui_string(message, "type", ""), ==,
					"unsent");
			g_assert_cmpstr(ui_string(message, "text", ""), ==,
					"Message unsent");
			g_assert_null(ui_field(message, "hasSound"));
			g_assert_cmpint(
				json_object_get_int(ui_field(message, "time")),
				==, 1000 + i);
		}
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

static void recover_group_delivery(void)
{
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 20 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed();
		return;
	}
	g_setenv("AVERNELLA_TEST_OUTBOX", "1", TRUE);
	g_setenv("LANGUAGE", "en", TRUE);
	testing_delivery = TRUE;
	g_assert_cmpint(line_ui(0, NULL), ==, 0);
	g_assert_cmpint(delivery_step, ==, DONE_DELIVERY);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--worker"))
		return g_getenv("AVERNELLA_TEST_OUTBOX") ? delivery_worker() :
							   worker();
	g_setenv("SDL_VIDEODRIVER", "dummy", TRUE);
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	g_setenv("AVERNELLA_VULKAN", "sdl", TRUE);
	g_setenv("LINE_DISABLE_TRAY", "1", TRUE);
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/ui/chat/refresh", refresh_chat);
	g_test_add_func("/ui/chat/recover-group-delivery",
			recover_group_delivery);
	return g_test_run();
}
