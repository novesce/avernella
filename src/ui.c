// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui_private.h"
#include "ui_gpu.h"
#include "line.h"
#include "login_retry.h"
#include "state.h"
#include "media_io.h"
#include "line_build_commit.h"
#include "flex_asset.h"
#include "ui_chat_lists.h"
#include "ui_tray.h"
#include "ui_instance.h"
#include "ui_chat_tools.h"
#include "ui_emoji.h"
#include "ui_friends.h"
#include "ui_find.h"
#include "ui_groups.h"
#include "ui_shared_content.h"
#include "ui_motion.h"
#include "ui_notifications.h"
#include "ui_profile.h"
#include "ui_liff.h"
#include "ui_sounds.h"
#include "liff_url.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum ui_request_method {
	LOGIN,
	CHATS,
	HISTORY,
	SEND,
	LOGOUT,
	MEDIA,
	OLDER,
	MARK_READ
};

struct ui_request {
	enum ui_request_method method;
	char *chat, *text, *action;
	Json *params;
	guint tag;
};

/* Only the UI thread mutates interface or request state. */
Interface ui;
static UiFrameClock frame_clock;
static UiScroll list_scroll, history_scroll;

struct draft_mention {
	int start, end;
	char id[34];
};

static GHashTable *mentions;

static GArray *mention_list(const char *chat)
{
	return mentions && chat ? g_hash_table_lookup(mentions, chat) : NULL;
}

static void forget_mentions(const char *chat)
{
	if (mentions && chat)
		g_hash_table_remove(mentions, chat);
}

static void edit_mentions(int left, int right, int inserted)
{
	GArray *list = mention_list(ui.selected);

	for (guint i = 0; list && i < list->len;) {
		struct draft_mention *m =
			&g_array_index(list, struct draft_mention, i);

		if (right <= m->start) {
			m->start += inserted - (right - left);
			m->end += inserted - (right - left);
		} else if (left < m->end) {
			g_array_remove_index(list, i);
			continue;
		}
		i++;
	}
}

static int utf16_units(const char *text)
{
	int units = 0;

	for (const char *p = text; *p; p = g_utf8_next_char(p))
		units += g_utf8_get_char(p) > 0xffff ? 2 : 1;
	return units;
}

static Json *mention_ranges(const char *chat, const char *text)
{
	GArray *list = mention_list(chat);
	Json *out = NULL;
	int units = 0, offset = 0, size = strlen(text);

	for (guint i = 0; list && i < list->len; i++) {
		const struct draft_mention *m =
			&g_array_index(list, struct draft_mention, i);

		if (m->end > size || m->start < offset || text[m->start] != '@')
			continue;
		g_autofree char *before =
			ui_emoji_export(chat, text, offset, m->start, NULL);
		g_autofree char *span =
			g_strndup(text + m->start, m->end - m->start);
		int width = utf16_units(span);
		Json *item = json_object_new_object();

		units += utf16_units(before);
		json_object_object_add(item, "id",
				       json_object_new_string(m->id));
		json_object_object_add(item, "start",
				       json_object_new_int(units));
		json_object_object_add(item, "length",
				       json_object_new_int(width));
		if (!out)
			out = json_object_new_array();
		json_object_array_add(out, item);
		units += width;
		offset = m->end;
	}
	return out;
}

static gboolean same_draft(const char *chat, const char *text, Json *sent)
{
	if (!ui_emoji_same(chat, text, sent))
		return FALSE;
	Json *current = mention_ranges(chat, text);
	Json *expected = ui_field(sent, "mentions");
	gboolean equal = json_object_equal(current, expected);

	json_object_put(current);
	return equal;
}

static int max_list_scroll(void)
{
	return MAX(0,
		   (int)ui.visible->len * ui_chat_row_height() - ui.list_box.h);
}

void ui_language_changed(void)
{
	/* Cached message rows hold translated dates and labels. */
	ui_forget_history();
	ui_layout();
	ui.dirty = TRUE;
}

void ui_theme_changed(int old_row_height)
{
	/* Keep the same chat at the top when row heights change. */
	ui_forget_history();
	ui_forget_rail();
	ui_layout();
	ui.list_offset = MIN(max_list_scroll(), (gint64)ui.list_offset *
							ui_chat_row_height() /
							old_row_height);
	ui_scroll_reset(&list_scroll, ui.list_offset);
	ui_scroll_reset(&history_scroll, ui.history_offset);
	ui.dirty = TRUE;
}

static void scroll_moved(UiScroll *scroll, int *offset, int sampled)
{
	if (scroll->end_us) {
		/*
		 * Keep hit testing on the actually displayed offset until the next
		 * frame. The fractional trajectory and its destination stay current.
		 */
		scroll->displayed = *offset;
	} else if (sampled != *offset) {
		*offset = sampled;
		ui.dirty = TRUE;
	}
}

static void scroll_by(UiScroll *scroll, int *offset, double distance,
		      int maximum)
{
	int sampled = *offset;

	ui_scroll_by(scroll, &sampled, distance, maximum,
		     g_get_monotonic_time(), ui_reduce_motion());
	scroll_moved(scroll, offset, sampled);
}

static void scroll_to(UiScroll *scroll, int *offset, int target, int maximum)
{
	int sampled = *offset;

	ui_scroll_to(scroll, &sampled, target, maximum, g_get_monotonic_time(),
		     ui_reduce_motion());
	scroll_moved(scroll, offset, sampled);
}

static void scroll_history(double distance)
{
	if (ui.stick_bottom)
		ui_scroll_reset(&history_scroll, ui.history_offset);
	scroll_by(&history_scroll, &ui.history_offset, distance,
		  ui.max_history_offset);
	ui.stick_bottom = !history_scroll.end_us &&
			  ui.history_offset == ui.max_history_offset;
}

static void sample_scroll(gint64 now)
{
	ui_scroll_sample(&list_scroll, &ui.list_offset, max_list_scroll(), now,
			 ui_reduce_motion());
	if (ui.stick_bottom) {
		ui_scroll_reset(&history_scroll, ui.history_offset);
		return;
	}
	gboolean moving = history_scroll.end_us != 0;
	gboolean history = ui_scroll_sample(&history_scroll, &ui.history_offset,
					    ui.max_history_offset, now,
					    ui_reduce_motion());
	if (moving && !history && ui.history_offset == ui.max_history_offset)
		ui.stick_bottom = TRUE;
}

static gboolean window_visible(void)
{
	return !(SDL_GetWindowFlags(ui.window) &
		 (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}

static struct {
	gint64 not_before; /* monotonic µs */
	guint rejected;
	char *notice;
	int shown;
} sign_in;

static int sign_in_wait(void);
static void show_sign_in_wait(void);

static int frame_wait(gint64 now, gboolean visual_animation)
{
	/* Keep receiving worker and input events while the tray or window
	 * manager hides the window. A show/restore event dirties the canvas.
	 */
	if (!window_visible())
		return -1;
	gint64 emoji = ui_reduce_motion() ? 0 : ui_emoji_deadline();

	if (emoji && emoji <= now)
		ui.dirty = TRUE;
	gboolean requested = ui.dirty || ui.video_dirty || visual_animation ||
			     history_scroll.end_us || list_scroll.end_us;
	int wait = ui_frame_wait_ms(&frame_clock, now, requested);

	if (emoji > now) {
		int next_image = (int)MIN((emoji - now + 999) / 1000, G_MAXINT);

		if (wait < 0 || next_image < wait)
			wait = next_image;
	}
	int left = !ui.connected && !ui.authenticating ? sign_in_wait() : 0;

	if (left != sign_in.shown) {
		show_sign_in_wait();
		ui.dirty = TRUE;
	}
	if (left)
		wait = wait < 0 ? 1000 : MIN(wait, 1000);
	return wait;
}

static void display_rate(void)
{
	int display = SDL_GetWindowDisplayIndex(ui.window);
	SDL_DisplayMode mode;
	int hz = display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) ==
					 0 ?
			 mode.refresh_rate :
			 0;
	const char *limit = g_getenv("AVERNELLA_MAX_FPS");
	guint64 fps;

	if (limit && g_ascii_string_to_unsigned(limit, 10, 1, 2000, &fps, NULL))
		hz = MIN(hz > 0 && hz <= 2000 ? hz : 60, (int)fps);
	ui_frame_rate(&frame_clock, hz);
}

static struct {
	pid_t pid;
	int fd, wake, preview;
	GThread *thread;
	GAsyncQueue *writes;
	GHashTable *requests;
	guint generation, seq, newest_history, rendered_history;
	guint history_pending;
	gboolean history_again, chats_again, refresh_notice;
	gint stopping;
	GHashTable *read_marks;
	Json *selected_summary; /* borrowed from ui.chats; rebound when the list changes */
	gint64 notification_baseline;
	guint32 packet_event, refresh_event, preview_event;
	SDL_TimerID login_timer;
	guint login_failures;
} wire;

/* A pending event owns the latest frame for its stream. */
static GMutex preview_lock;
struct pending_preview {
	int fd;
	struct line_preview_frame frame;
};
static struct pending_preview previews[LINE_PREVIEW_COUNT] = {
	[LINE_PREVIEW_SCREEN] = { .fd = -1 },
	[LINE_PREVIEW_REMOTE] = { .fd = -1 },
	[LINE_PREVIEW_SELF] = { .fd = -1 },
};

static void set_string(char **dest, const char *s);

/*
 * Sign-in pacing that outlives each attempt's worker: the wait LINE asked
 * for, and the run of rejected email passwords: after the first two, it
 * waits 10 s, 30 s and then 2 minutes before the next try. A success
 * clears the run.
 */
static int sign_in_wait(void)
{
	gint64 left = sign_in.not_before - g_get_monotonic_time();

	return left > 0 ? (int)((left + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC) :
			  0;
}

static void note_sign_in_failure(Json *packet)
{
	gint64 wait = line_login_wait_us(packet, &sign_in.rejected);

	if (wait > 0)
		sign_in.not_before =
			MAX(sign_in.not_before, g_get_monotonic_time() + wait);
	g_clear_pointer(&sign_in.notice, g_free);
}

/* Adds how long to wait to the sign-in notice, while a wait is due. */
static void show_sign_in_wait(void)
{
	int wait = sign_in_wait();

	if (!wait) {
		if (sign_in.notice)
			set_string(&ui.notice, sign_in.notice);
		g_clear_pointer(&sign_in.notice, g_free);
		sign_in.shown = 0;
		return;
	}
	if (!sign_in.notice)
		sign_in.notice = g_strdup(ui.notice);
	g_autofree char *pause = g_strdup_printf(
		_("Wait %d seconds before signing in again."), wait);
	g_autofree char *notice =
		sign_in.notice && *sign_in.notice ?
			g_strconcat(sign_in.notice, " ", pause, NULL) :
			g_strdup(pause);

	set_string(&ui.notice, notice);
	sign_in.shown = wait;
}
static void reset_login(const char *message);
static void handle_packet(Json *packet);

Json *ui_field(Json *obj, const char *key)
{
	Json *value = NULL;

	if (obj)
		json_object_object_get_ex(obj, key, &value);
	return value;
}

const char *ui_string(Json *obj, const char *key, const char *fallback)
{
	Json *value = ui_field(obj, key);

	return json_object_is_type(value, json_type_string) ?
		       json_object_get_string(value) :
		       fallback;
}

static int reader_count(Json *value)
{
	return json_object_is_type(value, json_type_int) ?
		       MAX(0, json_object_get_int(value)) :
		       0;
}

gboolean ui_message_is_read(Json *message)
{
	return reader_count(ui_field(message, "readCount")) > 0;
}

static gboolean merge_message_reads(Json *rows, Json *counts)
{
	gboolean changed = FALSE;

	if (!json_object_is_type(rows, json_type_array) ||
	    !json_object_is_type(counts, json_type_object))
		return FALSE;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		Json *row = json_object_array_get_idx(rows, i);
		const char *id = ui_string(row, "id", NULL);

		if (!id || !json_object_get_boolean(ui_field(row, "own")))
			continue;
		Json *count = ui_field(counts, id);
		int old = reader_count(ui_field(row, "readCount"));

		if (reader_count(count) <= old)
			continue;
		json_object_object_add(row, "readCount",
				       json_object_get(count));
		changed |= !old;
	}
	return changed;
}

void ui_message_reads(Json *packet)
{
	if (ui.selected && g_strcmp0(ui.selected, ui.self_id) &&
	    !g_strcmp0(ui.selected, ui_string(packet, "chat", NULL)) &&
	    merge_message_reads(ui.history, ui_field(packet, "counts")))
		ui.dirty = TRUE;
}

/* A delayed history response may predate a receipt for the same message. */
static void preserve_message_reads(Json *rows)
{
	Json *counts = NULL;

	if (!ui.history || !json_object_is_type(rows, json_type_array))
		return;
	for (size_t i = 0; i < json_object_array_length(ui.history); i++) {
		Json *row = json_object_array_get_idx(ui.history, i);
		const char *id = ui_string(row, "id", NULL);

		if (!id || !json_object_get_boolean(ui_field(row, "own")) ||
		    !ui_message_is_read(row))
			continue;
		if (!counts)
			counts = json_object_new_object();
		json_object_object_add(
			counts, id,
			json_object_get(ui_field(row, "readCount")));
	}
	if (counts) {
		merge_message_reads(rows, counts);
		json_object_put(counts);
	}
}

static void free_request(gpointer data)
{
	struct ui_request *request = data;

	g_free(request->chat);
	g_free(request->text);
	g_free(request->action);
	if (request->params)
		json_object_put(request->params);
	g_free(request);
}

static void post_packet(guint generation, char *line)
{
	SDL_Event e = { 0 };

	if (g_atomic_int_get(&wire.stopping)) {
		g_free(line);
		return;
	}
	e.type = wire.packet_event;
	e.user.code = (Sint32)generation;
	e.user.data1 = line;
	if (SDL_PushEvent(&e) != 1)
		g_free(line);
}

struct wire_write {
	size_t size;
	char data[];
};

enum {
	WORKER_QUIT_GRACE_MS = 2250
};

static void pump_preview(guint generation)
{
	struct line_preview_frame frame;
	int fd = line_media_recv_frame(wire.preview, &frame);

	if (fd < 0)
		return;
	struct pending_preview *preview = &previews[frame.source];

	g_mutex_lock(&preview_lock);
	gboolean notify = preview->fd < 0;

	if (!notify)
		close(preview->fd);
	preview->fd = fd;
	preview->frame = frame;
	if (notify) {
		SDL_Event e = { 0 };

		e.type = wire.preview_event;
		e.user.code = (Sint32)generation;
		e.user.data1 = GUINT_TO_POINTER((guint)frame.source);
		if (SDL_PushEvent(&e) != 1) {
			close(preview->fd);
			preview->fd = -1;
		}
	}
	g_mutex_unlock(&preview_lock);
}

static gpointer pump(gpointer data)
{
	guint generation = GPOINTER_TO_UINT(data);
	GString *input = g_string_new(NULL);
	struct wire_write *writing = NULL;
	size_t offset = 0;

	for (;;) {
		gboolean stopping = g_atomic_int_get(&wire.stopping);

		if (stopping) {
			g_clear_pointer(&writing, g_free);
		} else if (!writing) {
			writing = g_async_queue_try_pop(wire.writes);
			offset = 0;
		}
		struct pollfd fds[] = {
			{ wire.fd, POLLIN | (writing ? POLLOUT : 0), 0 },
			{ wire.wake, POLLIN, 0 },
			{ stopping ? -1 : wire.preview, POLLIN, 0 }
		};
		int rc = poll(fds, G_N_ELEMENTS(fds), -1);

		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 || (fds[0].revents & POLLNVAL))
			break;
		if (fds[1].revents & POLLIN) {
			uint64_t count;
			ssize_t n = read(wire.wake, &count, sizeof(count));
			(void)n;
		}
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			char buf[8192];
			ssize_t got = recv(wire.fd, buf, sizeof(buf), 0);

			if (got < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (got <= 0)
				break;
			if (g_atomic_int_get(&wire.stopping))
				continue;
			size_t start = 0;

			for (size_t i = 0; i < (size_t)got; i++) {
				if (buf[i] != '\n')
					continue;
				g_string_append_len(input, buf + start,
						    (gssize)(i - start));
				post_packet(generation,
					    g_string_free(input, FALSE));
				input = g_string_new(NULL);
				start = i + 1;
			}
			g_string_append_len(input, buf + start,
					    got - (ssize_t)start);
		}
		if (writing && (fds[0].revents & POLLOUT)) {
			size_t left = writing->size - offset;
			ssize_t n = send(wire.fd, writing->data + offset, left,
					 MSG_NOSIGNAL);
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (n < 0 && errno == EPIPE &&
			    g_atomic_int_get(&wire.stopping))
				continue;
			if (n <= 0)
				break;
			offset += n;
			if ((size_t)n == left)
				g_clear_pointer(&writing, g_free);
		}
		if (fds[2].revents & POLLIN)
			pump_preview(generation);
		if ((fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) &&
		    !g_atomic_int_get(&wire.stopping))
			break;
	}
	g_free(writing);
	g_string_free(input, TRUE);
	post_packet(generation, NULL);
	return NULL;
}

static gboolean reap_worker(pid_t pid, int options)
{
	pid_t result;

	do {
		result = waitpid(pid, NULL, options);
	} while (result < 0 && errno == EINTR);
	/* A failed wait cannot authorize a signal to a reused PID. */
	return result != 0;
}

static gpointer finish_reaping(gpointer child)
{
	reap_worker(GPOINTER_TO_INT(child), 0);
	return NULL;
}

static gboolean wait_worker(gint64 deadline)
{
#ifdef SYS_pidfd_open
	int fd = (int)syscall(SYS_pidfd_open, wire.pid, 0);

	if (fd >= 0) {
		struct pollfd exit = { .fd = fd, .events = POLLIN };
		gint64 remaining;

		while ((remaining = deadline - g_get_monotonic_time()) > 0) {
			int rc = poll(&exit, 1, (remaining + 999) / 1000);

			if (rc >= 0 || errno != EINTR)
				break;
		}
		close(fd);
		return reap_worker(wire.pid, WNOHANG);
	}
#endif
	while (!reap_worker(wire.pid, WNOHANG)) {
		gint64 remaining = deadline - g_get_monotonic_time();

		if (remaining <= 0)
			return FALSE;
		g_usleep(MIN(remaining, 10000));
	}
	return TRUE;
}

static void stop_worker(gboolean orderly)
{
	wire.generation++;
	if (wire.login_timer) {
		SDL_RemoveTimer(wire.login_timer);
		wire.login_timer = 0;
	}
	if (wire.pid > 0) {
		gboolean exited = FALSE;

		g_atomic_int_set(&wire.stopping, TRUE);
		/* EOF lets the worker leave a call while the pump drains replies. */
		if (orderly && !shutdown(wire.fd, SHUT_WR))
			exited = wait_worker(g_get_monotonic_time() +
					     WORKER_QUIT_GRACE_MS * 1000);
		shutdown(wire.fd, SHUT_RDWR);
		if (!exited && !reap_worker(wire.pid, WNOHANG)) {
			/* Even SIGKILL may wait for a stuck device to release. */
			kill(wire.pid, SIGKILL);
			if (!reap_worker(wire.pid, WNOHANG))
				g_thread_unref(g_thread_new(
					"line-reap", finish_reaping,
					GINT_TO_POINTER(wire.pid)));
		}
		g_thread_join(wire.thread);
		close(wire.fd);
		close(wire.wake);
		close(wire.preview);
		for (size_t i = 0; i < G_N_ELEMENTS(previews); i++) {
			if (previews[i].fd >= 0)
				close(previews[i].fd);
			previews[i].fd = -1;
		}
		g_async_queue_unref(wire.writes);
		wire.pid = 0;
	}
	g_hash_table_remove_all(wire.requests);
	g_clear_pointer(&wire.read_marks, g_hash_table_unref);
	wire.selected_summary = NULL;
	wire.rendered_history = 0;
	wire.history_pending = 0;
	wire.history_again = wire.chats_again = FALSE;
}

/*
 * The decoder lives in the worker. Pass the GPU actually opened by the UI,
 * including a fallback after device creation failed on the preferred GPU.
 */
static char **worker_environment(void)
{
	char **env = g_get_environ();
	int minor = ui_gpu_render_minor();
	guint vendor = ui_gpu_device_vendor();

	/* The audio worker has no SDL event loop to consume SDL_QUIT. */
	env = g_environ_setenv(env, SDL_HINT_NO_SIGNAL_HANDLERS, "1", TRUE);
	env = g_environ_unsetenv(env, "AVERNELLA_MEDIA_RENDER_NODE");
	env = g_environ_unsetenv(env, "AVERNELLA_MEDIA_GPU_VENDOR");
	if (minor >= 0) {
		g_autofree char *node =
			g_strdup_printf("/dev/dri/renderD%d", minor);

		env = g_environ_setenv(env, "AVERNELLA_MEDIA_RENDER_NODE", node,
				       TRUE);
	}
	if (vendor) {
		char id[16];

		g_snprintf(id, sizeof(id), "0x%x", vendor);
		env = g_environ_setenv(env, "AVERNELLA_MEDIA_GPU_VENDOR", id,
				       TRUE);
	}
	return env;
}

static gboolean start_worker(int credentials_fd)
{
	int pair[2], preview[2];

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
		return FALSE;
	wire.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (wire.wake < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
					0, preview) < 0) {
		int failure = errno;

		if (wire.wake >= 0)
			close(wire.wake);
		close(pair[0]);
		close(pair[1]);
		errno = failure;
		return FALSE;
	}
	int output = LINE_CREDENTIAL_FD + (credentials_fd >= 0);
	int buffer = 4096;

	setsockopt(preview[1], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
	posix_spawn_file_actions_t actions;
	int err = posix_spawn_file_actions_init(&actions);

	if (!err) {
		err = posix_spawn_file_actions_adddup2(&actions, pair[1],
						       STDIN_FILENO);
		if (!err)
			err = posix_spawn_file_actions_adddup2(
				&actions, pair[1], STDOUT_FILENO);
		if (!err && credentials_fd >= 0)
			err = posix_spawn_file_actions_adddup2(
				&actions, credentials_fd, LINE_CREDENTIAL_FD);
		if (!err)
			err = posix_spawn_file_actions_adddup2(
				&actions, preview[1], output);
#if defined(__GLIBC__) && \
	(__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
		/* Keep the control socket, preview socket and optional password pipe. */
		if (!err)
			err = posix_spawn_file_actions_addclosefrom_np(
				&actions, output + 1);
#endif
		char *const args[] = { (char *)"avernella",
				       credentials_fd >= 0 ?
					       (char *)"--worker-password" :
					       (char *)"--worker",
				       NULL };
		if (!err) {
			g_auto(GStrv) env = worker_environment();

			err = posix_spawn(&wire.pid, "/proc/self/exe", &actions,
					  NULL, args, env);
		}
		posix_spawn_file_actions_destroy(&actions);
	}
	close(pair[1]);
	close(preview[1]);
	if (err) {
		close(pair[0]);
		close(preview[0]);
		close(wire.wake);
		wire.pid = 0;
		errno = err;
		return FALSE;
	}
	wire.fd = pair[0];
	wire.preview = preview[0];
	fcntl(wire.fd, F_SETFL, O_NONBLOCK);
	wire.writes = g_async_queue_new_full(g_free);
	g_atomic_int_set(&wire.stopping, FALSE);
	wire.thread = g_thread_new("line-ipc", pump,
				   GUINT_TO_POINTER(wire.generation));
	return TRUE;
}

static guint submit(struct ui_request *saved, const char *name, Json *args)
{
	guint id = ++wire.seq;

	g_hash_table_insert(wire.requests, GUINT_TO_POINTER(id), saved);
	Json *packet = json_object_new_object();

	json_object_object_add(packet, "id", json_object_new_int64(id));
	json_object_object_add(packet, "method", json_object_new_string(name));
	json_object_object_add(packet, "params", args);
	size_t size;
	const char *encoded = json_object_to_json_string_length(
		packet, JSON_C_TO_STRING_PLAIN, &size);
	struct wire_write *line = g_malloc(sizeof(*line) + size + 1);

	line->size = size + 1;
	memcpy(line->data, encoded, size);
	line->data[size] = '\n';
	json_object_put(packet);
	g_async_queue_lock(wire.writes);
	gboolean wake = g_async_queue_length_unlocked(wire.writes) == 0;

	g_async_queue_push_unlocked(wire.writes, line);
	g_async_queue_unlock(wire.writes);
	uint64_t one = 1;

	if (wake)
		while (write(wire.wake, &one, sizeof(one)) < 0 &&
		       errno == EINTR)
			;
	return id;
}

static guint request(enum ui_request_method method, const char *chat,
		     const char *text)
{
	static const char *const names[] = { "login",	    "chats",  "history",
					     "send",	    "logout", NULL,
					     "olderHistory" };
	struct ui_request *saved = g_new0(struct ui_request, 1);

	saved->method = method;
	saved->chat = g_strdup(chat);
	saved->text = g_strdup(text);
	Json *args = json_object_new_object();

	if (chat)
		json_object_object_add(args, "chat",
				       json_object_new_string(chat));
	if (text) {
		Json *spans = NULL;

		g_autofree char *visible =
			ui_emoji_export(chat, text, 0, strlen(text), &spans);
		json_object_object_add(args, "text",
				       json_object_new_string(visible));
		if (spans)
			json_object_object_add(args, "emojis", spans);
		Json *decorations =
			method == SEND ? mention_ranges(chat, text) : NULL;

		if (decorations)
			json_object_object_add(args, "mentions", decorations);
	}
	if (method == SEND && ui.reply_id && !g_strcmp0(chat, ui.reply_chat))
		json_object_object_add(args, "replyTo",
				       json_object_new_string(ui.reply_id));
	if (method == SEND)
		saved->params = json_object_get(args);
	return submit(saved, names[method], args);
}

static Json *chat_summary(const char *id)
{
	for (size_t i = 0;
	     id && ui.chats && i < json_object_array_length(ui.chats); i++) {
		Json *row = json_object_array_get_idx(ui.chats, i);

		if (!g_strcmp0(id, ui_string(row, "id", NULL)))
			return row;
	}
	return NULL;
}

/* The window is unfocused, hidden in the tray or minimized. */
static gboolean window_away(void)
{
	Uint32 flags = SDL_GetWindowFlags(ui.window);

	return !(flags & SDL_WINDOW_INPUT_FOCUS) ||
	       (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}

static gboolean notify_in_background(void)
{
	return ui_notifications_enabled() && window_away();
}

static void notify_unread(Json *rows)
{
	if (!wire.notification_baseline) {
		wire.notification_baseline = g_get_real_time() / 1000;
		return;
	}
	Json *newest = NULL;
	gint64 newest_time = 0;

	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		Json *row = json_object_array_get_idx(rows, i);
		gint64 time = json_object_get_int64(ui_field(row, "time"));
		const char *id = ui_string(row, "id", NULL);
		guint64 last = ui_read_id(ui_string(row, "lastId", NULL));

		if (!id || !last || time <= newest_time ||
		    !g_strcmp0(id, ui.self_id) || ui_chat_muted(id) ||
		    json_object_get_int64(ui_field(row, "unread")) <= 0)
			continue;
		UiReadCursor *cursor =
			wire.read_marks ?
				g_hash_table_lookup(wire.read_marks, id) :
				NULL;
		Json *previous = chat_summary(id);
		guint64 prior = ui_read_id(ui_string(previous, "lastId", NULL));
		guint64 seen = ui_read_id(ui_string(row, "lastSeenId", NULL));

		if (ui_read_covers(cursor, last, seen) ||
		    (prior ? last <= prior : time < wire.notification_baseline))
			continue;
		newest = row;
		newest_time = time;
	}
	if (!newest)
		return;
	if (notify_in_background())
		ui_desktop_notify(ui_string(newest, "name", NULL), FALSE);
	/* In the window, chime only for a chat other than the open one. */
	if (window_away() ||
	    g_strcmp0(ui_string(newest, "id", NULL), ui.selected))
		ui_sound_play(CHIP_MESSAGE);
}

void ui_conversation_presented(gboolean covered)
{
	Json *summary = wire.selected_summary;

	if (!wire.pid || !summary || !ui.history || !ui.selected ||
	    !g_strcmp0(ui.selected, ui.self_id))
		return;
	UiReadCursor *cursor =
		wire.read_marks ?
			g_hash_table_lookup(wire.read_marks, ui.selected) :
			NULL;
	gboolean unread = json_object_get_int64(ui_field(summary, "unread")) >
			  0;

	if (!unread &&
	    (!cursor || cursor->pending ||
	     cursor->viewed <= MAX(cursor->attempted, cursor->confirmed)))
		return;
	gboolean latest = wire.rendered_history &&
			  wire.rendered_history == wire.newest_history;
	if (!ui_read_visible(&ui, SDL_GetWindowFlags(ui.window), covered,
			     latest, ui_chat_visible()))
		return;
	size_t count = json_object_array_length(ui.history);
	Json *last = count ? json_object_array_get_idx(ui.history, count - 1) :
			     NULL;
	const char *id = ui_string(last, "id", NULL);

	if (!id || g_strcmp0(id, ui_string(summary, "lastId", NULL)))
		return;
	guint64 target = ui_read_id(id),
		seen = ui_read_id(ui_string(summary, "lastSeenId", NULL));
	if (!target)
		return;
	if (!wire.read_marks)
		wire.read_marks = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);
	if (!cursor) {
		cursor = g_new0(UiReadCursor, 1);
		g_hash_table_insert(wire.read_marks, g_strdup(ui.selected),
				    cursor);
	}
	gboolean send_receipt = ui_read_present(cursor, target, seen);

	if (unread) {
		json_object_object_add(summary, "unread",
				       json_object_new_int(0));
		ui_forget_rail();
		ui_refilter_chats();
	}
	if (!send_receipt)
		return;
	struct ui_request *saved = g_new0(struct ui_request, 1);

	saved->method = MARK_READ;
	saved->chat = g_strdup(ui.selected);
	saved->text = g_strdup(id);
	Json *params = json_object_new_object();

	json_object_object_add(params, "chat",
			       json_object_new_string(ui.selected));
	json_object_object_add(params, "id", json_object_new_string(id));
	submit(saved, "markRead", params);
}

void ui_rpc(const char *name, Json *args, guint tag)
{
	if (!wire.pid || !ui.connected) {
		json_object_put(args);
		return;
	}
	struct ui_request *saved = g_new0(struct ui_request, 1);

	saved->method = MEDIA;
	saved->action = g_strdup(name);
	saved->params = json_object_get(args);
	saved->tag = tag;
	submit(saved, name, args);
}

static void set_string(char **dest, const char *s)
{
	if (dest == &ui.notice)
		wire.refresh_notice = FALSE;
	if (!g_strcmp0(*dest, s))
		return;
	char *copy = g_strdup(s);

	g_free(*dest);
	*dest = copy;
	ui.dirty = TRUE;
}

static void set_refresh_notice(const char *s)
{
	set_string(&ui.notice, s);
	wire.refresh_notice = TRUE;
}

static gboolean set_json(Json **dest, Json *value)
{
	if (dest == &ui.history && value && value != *dest)
		preserve_message_reads(value);
	if (dest == &ui.history && !value) {
		ui.older_page = ui.older_pending = ui.has_older = FALSE;
		wire.rendered_history = 0;
	}
	if (*dest == value ||
	    (*dest && value && json_object_equal(*dest, value)))
		return FALSE;
	if (dest == &ui.history)
		ui_forget_history();
	if (dest == &ui.chats)
		ui_forget_rail();
	if (value)
		json_object_get(value);
	if (*dest)
		json_object_put(*dest);
	*dest = value;
	ui.dirty = TRUE;
	return TRUE;
}

static void set_text(Editor *edit, const char *text)
{
	g_string_assign(edit->value, text ? text : "");
	edit->cursor = edit->anchor = (int)edit->value->len;
	edit->scroll = 0;
	ui.dirty = TRUE;
}

static void focus_on(Focus focus)
{
	ui.focus = focus;
	ui.dirty = TRUE;
	set_string(&ui.composition, "");
	Rect rect;

	switch (focus) {
	case F_SEARCH:
		rect = ui.search_box;
		break;
	case F_LOGIN_EMAIL:
		rect = ui.email_box;
		break;
	case F_LOGIN_PASSWORD:
		rect = ui.password_box;
		break;
	case F_MESSAGE:
		if (ui.selected) {
			rect = ui.entry;
			break;
		}
		/* fall through */
	default:
		SDL_StopTextInput();
		return;
	}
	if (rect.w > 0) {
		SDL_Rect caret = { rect.x + 14, rect.y + 6, MAX(1, rect.w - 28),
				   rect.h - 12 };
		SDL_SetTextInputRect(&caret);
	}
	SDL_StartTextInput();
}

static void clear_password(void)
{
	GString *password = ui.login_password.value;

	explicit_bzero(password->str, password->allocated_len);
	set_text(&ui.login_password, "");
}

static void reset_login(const char *message)
{
	stop_worker(FALSE);
	ui_sound_stop_loop();
	ui_media_reset();
	ui_lists_reset();
	ui_forget_labels();
	ui.authenticating = ui.connected = ui.logging_out = ui.getting_chats =
		FALSE;
	g_clear_pointer(&ui.qr, QRcode_free);
	g_clear_pointer(&ui.pin, g_free);
	g_clear_pointer(&ui.selected, g_free);
	g_clear_pointer(&ui.name, g_free);
	set_json(&ui.opened, NULL);
	set_json(&ui.chats, NULL);
	wire.notification_baseline = 0;
	set_json(&ui.history, NULL);
	g_hash_table_remove_all(ui.drafts);
	g_clear_pointer(&mentions, g_hash_table_unref);
	g_hash_table_remove_all(ui.sending);
	g_array_set_size(ui.visible, 0);
	set_text(&ui.search, "");
	set_text(&ui.message, "");
	ui.list_offset = ui.history_offset = 0;
	ui_scroll_reset(&list_scroll, 0);
	ui_scroll_reset(&history_scroll, 0);
	ui.list_cursor = -1;
	clear_password();
	ui.saved_session = line_store_has_session();
	/* Email or QR sign-in pairs a new session; resuming one needs neither. */
	if (ui.saved_session)
		ui.email_login = FALSE;
	focus_on(ui.email_login ? F_LOGIN_PASSWORD : F_LOGIN);
	set_string(&ui.notice, message);
	wire.login_failures = 0;
}

static gboolean start_password_worker(void)
{
	struct line_credentials creds = { 0 };
	int pipefd[2];

	if (pipe2(pipefd, O_CLOEXEC) < 0)
		return FALSE;
	g_strlcpy(creds.email, ui.login_email.value->str, sizeof(creds.email));
	g_strstrip(creds.email);
	g_strlcpy(creds.password, ui.login_password.value->str,
		  sizeof(creds.password));
	/* One record fits in PIPE_BUF and is available before the child starts. */
	ssize_t n;

	do {
		n = write(pipefd[1], &creds, sizeof(creds));
	} while (n < 0 && errno == EINTR);
	explicit_bzero(&creds, sizeof(creds));
	int failure = n < 0 ? errno : EIO;

	close(pipefd[1]);
	gboolean started = FALSE;

	if (n == (ssize_t)sizeof(creds)) {
		started = start_worker(pipefd[0]);
		failure = errno;
	}
	close(pipefd[0]);
	errno = failure;
	return started;
}

static void login(gboolean resume)
{
	if (wire.pid) {
		reset_login(_("Sign-in cancelled."));
		return;
	}
	gboolean password = ui.email_login && !resume;

	if (sign_in_wait()) {
		set_string(&ui.notice, "");
		show_sign_in_wait();
		return;
	}
	if (password &&
	    (!ui.login_email.value->len || !ui.login_password.value->len)) {
		set_string(&ui.notice,
			   _("Enter your email address and password."));
		focus_on(ui.login_email.value->len ? F_LOGIN_PASSWORD :
						     F_LOGIN_EMAIL);
		return;
	}
	gboolean started = password ? start_password_worker() :
				      start_worker(-1);

	if (!started) {
		set_string(&ui.notice, g_strerror(errno));
		return;
	}
	clear_password();
	ui.authenticating = TRUE;
	focus_on(F_LOGIN);
	set_string(&ui.notice, _("Contacting LINE…"));
	request(LOGIN, NULL, NULL);
}

static void toggle_sign_in(void)
{
	ui.email_login = !ui.email_login;
	clear_password();
	set_string(&ui.notice, "");
	ui_layout();
	focus_on(ui.email_login ? F_LOGIN_EMAIL : F_LOGIN);
}

static Uint32 login_again(Uint32 interval, void *generation)
{
	(void)interval;
	SDL_Event e = { 0 };

	e.type = wire.refresh_event;
	e.user.code = (Sint32)GPOINTER_TO_UINT(generation);
	e.user.data1 = &wire.login_timer;
	SDL_PushEvent(&e);
	return 0;
}

static void arm_login_timer(guint seconds)
{
	/* SDL takes a 32-bit millisecond interval. Recheck long waits daily. */
	wire.login_timer = SDL_AddTimer(MIN(seconds, 86400u) * 1000u,
					login_again,
					GUINT_TO_POINTER(wire.generation));
}

/*
 * LINE could not be reached with a saved session. Stay signed in and retry
 * after 5, 10, 20 ... seconds, at most every five minutes.
 */
static void retry_login(const char *error)
{
	guint delay = 5u << MIN(wire.login_failures, 6u);

	delay = MAX(MIN(delay, 300u), (guint)sign_in_wait());
	wire.login_failures++;
	g_autofree char *notice = g_strdup_printf(
		_("Still signed in. %s Retrying in %u s…"), error, delay);
	set_string(&ui.notice, notice);
	if (!wire.login_timer)
		arm_login_timer(delay);
}

static void logout(void)
{
	if (ui.logging_out)
		return;
	/* A saved session that is not signed in has no worker running yet. */
	if (!wire.pid && !start_worker(-1)) {
		set_string(&ui.notice, g_strerror(errno));
		return;
	}
	ui.logging_out = TRUE;
	request(LOGOUT, NULL, NULL);
	set_string(&ui.notice, _("Signing out…"));
}

static void request_history(void)
{
	wire.history_again = FALSE;
	wire.newest_history = wire.history_pending =
		request(HISTORY, ui.selected, NULL);
}

static void refresh_visible_history(void)
{
	if (!ui.selected || ui.older_page || ui.older_pending)
		return;
	if (wire.history_pending)
		wire.history_again = TRUE;
	else
		request_history();
}

static void refresh(void)
{
	if (!ui.connected || ui.logging_out)
		return;
	if (ui.getting_chats)
		wire.chats_again = TRUE;
	else {
		ui.getting_chats = TRUE;
		request(CHATS, NULL, NULL);
	}
}

static void show_chats(Json *list);

/*
 * A new message moves its chat to the top and counts it unread without
 * reloading the list; an unknown chat falls back to a full refresh.
 */
static void chat_message(Json *packet)
{
	const char *chat = ui_string(packet, "chat", NULL),
		   *id = ui_string(packet, "id", NULL);
	gboolean mine = json_object_get_boolean(ui_field(packet, "mine"));
	gint64 time = json_object_get_int64(ui_field(packet, "time"));
	Json *moved = NULL;

	for (size_t i = 0;
	     chat && id && ui.chats && i < json_object_array_length(ui.chats);
	     i++) {
		Json *row = json_object_array_get_idx(ui.chats, i);

		if (g_strcmp0(ui_string(row, "id", NULL), chat))
			continue;
		guint64 incoming = ui_read_id(id);

		/* Sync can replay a delivery already included in the list. */
		if (incoming &&
		    incoming <= ui_read_id(ui_string(row, "lastId", NULL)))
			return;
		if (json_object_deep_copy(row, &moved, NULL))
			moved = NULL;
		break;
	}
	if (!moved) {
		refresh();
		return;
	}
	if (time > json_object_get_int64(ui_field(moved, "time")))
		json_object_object_add(moved, "time",
				       json_object_new_int64(time));
	json_object_object_add(moved, "lastId", json_object_new_string(id));
	if (!mine)
		json_object_object_add(
			moved, "unread",
			json_object_new_int64(json_object_get_int64(ui_field(
						      moved, "unread")) +
					      1));
	Json *rows = json_object_new_array();

	json_object_array_add(rows, moved);
	for (size_t i = 0; i < json_object_array_length(ui.chats); i++) {
		Json *row = json_object_array_get_idx(ui.chats, i);

		if (g_strcmp0(ui_string(row, "id", NULL), chat))
			json_object_array_add(rows, json_object_get(row));
	}
	notify_unread(rows);
	show_chats(rows);
	json_object_put(rows);
}

/* The worker revalidated the open chat and pushed what changed. */
static void pushed_history(Json *packet)
{
	Json *rows = ui_field(packet, "messages");

	if (g_strcmp0(ui_string(packet, "chat", NULL), ui.selected) ||
	    !json_object_is_type(rows, json_type_array) || ui.older_page ||
	    ui.older_pending)
		return;
	ui.has_older = json_object_array_length(rows) >= 50;
	set_json(&ui.history, rows);
	set_string(&ui.notice, "");
}

static void retry_read_receipt(void)
{
	UiReadCursor *cursor =
		wire.read_marks && ui.selected ?
			g_hash_table_lookup(wire.read_marks, ui.selected) :
			NULL;
	if (cursor && !cursor->pending)
		cursor->attempted = cursor->confirmed;
}

static void refresh_content(void)
{
	if (!ui.connected || ui.logging_out)
		return;
	ui_media_refresh();
	retry_read_receipt();
	refresh();
}

static void history_page(gboolean latest)
{
	if (!ui.selected || ui.older_pending ||
	    (latest ? !ui.older_page : !ui.has_older))
		return;
	ui.older_pending = TRUE;
	if (latest)
		request_history();
	else {
		wire.history_pending = 0;
		wire.history_again = FALSE;
		wire.newest_history = request(OLDER, ui.selected, NULL);
	}
	ui.dirty = TRUE;
}

void ui_refilter_chats(void)
{
	ui_lists_rebuild();
	ui.list_offset = CLAMP(ui.list_offset, 0, max_list_scroll());
	ui.dirty = TRUE;
}

static void remember_draft(void)
{
	if (!ui.selected)
		return;
	if (ui.message.value->len)
		g_hash_table_replace(ui.drafts, g_strdup(ui.selected),
				     g_strdup(ui.message.value->str));
	else
		g_hash_table_remove(ui.drafts, ui.selected);
}

static void highlight(int pos)
{
	int row_height = ui_chat_row_height();
	int top = pos * row_height;
	/* Held arrow keys retarget from where the list is heading. */
	int view = list_scroll.end_us ? (int)(list_scroll.to + .5) :
					ui.list_offset;

	ui.list_cursor = pos;
	if (top < view)
		scroll_to(&list_scroll, &ui.list_offset, top,
			  max_list_scroll());
	else if (top + row_height > view + ui.list_box.h)
		scroll_to(&list_scroll, &ui.list_offset,
			  top + row_height - ui.list_box.h, max_list_scroll());
	ui.dirty = TRUE;
}

static void select_id(const char *id)
{
	if (g_strcmp0(ui.selected, id)) {
		remember_draft();
		ui_media_select_chat();
		set_string(&ui.selected, id);
		retry_read_receipt();
		ui_media_chat_shown();
		wire.selected_summary = chat_summary(id);
		set_text(&ui.message, g_hash_table_lookup(ui.drafts, id));
		set_json(&ui.history, NULL);
		set_string(&ui.notice, _("Loading messages…"));
		request_history();
		ui.stick_bottom = TRUE;
	}
	focus_on(F_MESSAGE);
}

static void select_chat(int pos)
{
	if (pos < 0 || (guint)pos >= ui.visible->len)
		return;
	guint index = g_array_index(ui.visible, guint, pos);

	select_id(ui_string(json_object_array_get_idx(ui.chats, index), "id",
			    ""));
	highlight(pos);
}

static gboolean includes_chat(Json *list, const char *id)
{
	for (size_t i = 0; list && i < json_object_array_length(list); i++)
		if (!g_strcmp0(id, ui_string(json_object_array_get_idx(list, i),
					     "id", "")))
			return TRUE;
	return FALSE;
}

static void show_chats(Json *list)
{
	Json *merged = json_object_new_array();

	for (size_t i = 0;
	     ui.opened && i < json_object_array_length(ui.opened);) {
		Json *r = json_object_array_get_idx(ui.opened, i);

		if (includes_chat(list, ui_string(r, "id", "")))
			json_object_array_del_idx(ui.opened, i, 1);
		else {
			json_object_array_add(merged, json_object_get(r));
			i++;
		}
	}
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		Json *row = json_object_array_get_idx(list, i);
		const char *id = ui_string(row, "id", NULL);
		UiReadCursor *cursor =
			wire.read_marks && id ?
				g_hash_table_lookup(wire.read_marks, id) :
				NULL;
		guint64 last = ui_read_id(ui_string(row, "lastId", NULL));
		guint64 seen = ui_read_id(ui_string(row, "lastSeenId", NULL));

		if (cursor)
			cursor->confirmed = MAX(cursor->confirmed, seen);
		if (json_object_get_int64(ui_field(row, "unread")) > 0 &&
		    ui_read_covers(cursor, last, seen))
			json_object_object_add(row, "unread",
					       json_object_new_int(0));
		json_object_array_add(merged, json_object_get(row));
	}
	gboolean changed = set_json(&ui.chats, merged);

	json_object_put(merged);
	if (!changed)
		return;
	wire.selected_summary = chat_summary(ui.selected);
	ui_refilter_chats();
	if (!wire.selected_summary) {
		if (ui.selected)
			ui_find_reset();
		remember_draft();
		g_clear_pointer(&ui.selected, g_free);
		set_json(&ui.history, NULL);
		set_text(&ui.message, "");
		set_string(&ui.notice, "");
		if (ui.focus == F_MESSAGE || ui.focus == F_SEND ||
		    ui.focus == F_BACK)
			focus_on(F_SEARCH);
	}
}

void ui_group_left(const char *id)
{
	for (size_t i = 0; ui.opened && i < json_object_array_length(ui.opened);
	     i++) {
		Json *row = json_object_array_get_idx(ui.opened, i);

		if (!g_strcmp0(id, ui_string(row, "id", NULL))) {
			json_object_array_del_idx(ui.opened, i, 1);
			break;
		}
	}
	Json *rows = json_object_new_array();

	for (size_t i = 0; ui.chats && i < json_object_array_length(ui.chats);
	     i++) {
		Json *row = json_object_array_get_idx(ui.chats, i);
		const char *chat = ui_string(row, "id", "");

		if (g_strcmp0(id, chat) && !includes_chat(ui.opened, chat))
			json_object_array_add(rows, json_object_get(row));
	}
	if (!g_strcmp0(ui.selected, id))
		ui_media_select_chat();
	show_chats(rows);
	json_object_put(rows);
	refresh();
}

void ui_update_friend_chat(Json *row)
{
	Json *chat = chat_summary(ui_string(row, "id", NULL));
	const char *name = ui_string(row, "name", NULL);

	const char *avatar = ui_string(row, "avatarKey", NULL);

	if (chat && avatar && *avatar &&
	    g_strcmp0(avatar, ui_string(chat, "avatarKey", NULL))) {
		json_object_object_add(chat, "avatarKey",
				       json_object_new_string(avatar));
		ui.dirty = TRUE;
	}
	if (chat && name && g_strcmp0(name, ui_string(chat, "name", NULL))) {
		json_object_object_add(chat, "name",
				       json_object_new_string(name));
		ui_refilter_chats();
	}
}

void ui_open_conversation(Json *row)
{
	const char *id = ui_string(row, "id", "");

	if (!*id || !ui.connected)
		return;
	if (!includes_chat(ui.chats, id)) {
		if (!ui.opened)
			ui.opened = json_object_new_array();
		json_object_array_add(ui.opened, json_object_get(row));
		Json *merged = json_object_new_array();

		json_object_array_add(merged, json_object_get(row));
		for (size_t i = 0;
		     ui.chats && i < json_object_array_length(ui.chats); i++)
			json_object_array_add(
				merged,
				json_object_get(json_object_array_get_idx(
					ui.chats, i)));
		set_json(&ui.chats, merged);
		json_object_put(merged);
	}
	ui_update_friend_chat(row);
	set_text(&ui.search, "");
	ui_lists_reveal(id);
	ui_refilter_chats();
	select_id(id);
	for (guint i = 0; i < ui.visible->len; i++) {
		Json *r = json_object_array_get_idx(
			ui.chats, g_array_index(ui.visible, guint, i));
		if (!g_strcmp0(id, ui_string(r, "id", ""))) {
			highlight(i);
			break;
		}
	}
}

static gboolean can_send(void)
{
	if (!ui.selected || ui.logging_out ||
	    g_hash_table_contains(ui.sending, ui.selected))
		return FALSE;
	for (const char *ch = ui.message.value->str; *ch;
	     ch = g_utf8_next_char(ch))
		if (!g_unichar_isspace(g_utf8_get_char(ch)))
			return TRUE;
	return FALSE;
}

static void send_message(void)
{
	if (!can_send())
		return;
	g_hash_table_add(ui.sending, g_strdup(ui.selected));
	request(SEND, ui.selected, ui.message.value->str);
	set_string(&ui.notice, _("Sending…"));
}

/*
 * A tapped rich-message area: open an https link, send its text as you,
 * copy, or switch the message to another scene.
 */
static void rich_action(Json *area, Json *message)
{
	const char *kind = ui_string(area, "kind", ""),
		   *value = ui_string(area, "value", "");

	if (!strcmp(kind, "link")) {
		ui_liff_open_link(value);
	} else if (!strcmp(kind, "message")) {
		if (!ui.selected || ui.logging_out ||
		    g_hash_table_contains(ui.sending, ui.selected))
			return;
		g_hash_table_add(ui.sending, g_strdup(ui.selected));
		request(SEND, ui.selected, value);
		set_string(&ui.notice, _("Sending…"));
	} else if (!strcmp(kind, "copy")) {
		ui_status(SDL_SetClipboardText(value) ?
				  _("Could not copy to the clipboard.") :
				  _("Copied to the clipboard."));
	} else if (!strcmp(kind, "scene")) {
		ui_rich_set_scene(message, value);
	}
}

static void report_foreground(void)
{
	if (!ui.connected)
		return;
	Json *p = json_object_new_object();

	json_object_object_add(p, "active", json_object_new_boolean(ui.active));
	ui_rpc("foreground", p, 0);
	/* Hand pages freed while the window was busy back to the kernel. */
	if (!ui.active)
		line_trim_memory();
}

static void show_sent(const struct ui_request *saved, Json *receipt)
{
	const char *id = ui_string(receipt, "id", NULL);
	const char *text = ui_string(saved->params, "text", NULL);

	if (!ui.history || ui.older_page || ui.older_pending ||
	    !ui_read_id(id) || !text)
		return;
	size_t n = json_object_array_length(ui.history);

	for (size_t i = 0; i < n; i++)
		if (!g_strcmp0(id, ui_string(json_object_array_get_idx(
						     ui.history, i),
					     "id", NULL)))
			return;
	Json *row = json_object_new_object();
	gint64 sent = json_object_get_int64(ui_field(receipt, "sentAt"));
	Json *emojis = ui_field(saved->params, "emojis");
	Json *reply = ui_field(saved->params, "replyTo");

	json_object_object_add(row, "id", json_object_new_string(id));
	json_object_object_add(row, "text", json_object_new_string(text));
	json_object_object_add(row, "type", json_object_new_string("text"));
	json_object_object_add(row, "from", json_object_new_string("You"));
	json_object_object_add(row, "own", json_object_new_boolean(TRUE));
	json_object_object_add(
		row, "time",
		json_object_new_int64(sent > 0 ? sent :
						 g_get_real_time() / 1000));
	if (ui.self_id)
		json_object_object_add(row, "senderId",
				       json_object_new_string(ui.self_id));
	if (emojis)
		json_object_object_add(row, "emojis", json_object_get(emojis));
	if (reply)
		json_object_object_add(row, "replyTo", json_object_get(reply));
	json_object_array_add(ui.history, row);
	ui_forget_history();
	set_string(&ui.notice, "");
	ui.dirty = TRUE;
}

static void handle_reply(guint id, struct ui_request *saved, Json *result,
			 const char *error)
{
	switch (saved->method) {
	case LOGIN:
		if (error || !json_object_is_type(result, json_type_object)) {
			reset_login(
				error ? error :
					_("Sign-in returned an unexpected response."));
			show_sign_in_wait();
			return;
		}
		sign_in.rejected = 0;
		sign_in.not_before = 0;
		g_clear_pointer(&sign_in.notice, g_free);
		sign_in.shown = 0;
		ui.authenticating = FALSE;
		ui.connected = TRUE;
		ui.email_login = FALSE;
		set_text(&ui.login_email, "");
		wire.login_failures = 0;
		ui_media_login(result);
		g_clear_pointer(&ui.qr, QRcode_free);
		g_clear_pointer(&ui.pin, g_free);
		set_string(&ui.name, ui_string(result, "name", "LINE"));
		set_refresh_notice(_("Loading chats…"));
		ui_lists_login(ui.self_id);
		ui_profile_login();
		ui_layout();
		focus_on(F_SEARCH);
		report_foreground();
		refresh();
		break;
	case CHATS:
		ui.getting_chats = FALSE;
		if (error || !json_object_is_type(result, json_type_array))
			set_refresh_notice(
				error ? error :
					_("Could not read the chat list."));
		else {
			if (wire.refresh_notice)
				set_string(&ui.notice, "");
			notify_unread(result);
			show_chats(result);
		}
		if (wire.chats_again && ui.connected && !ui.logging_out) {
			wire.chats_again = FALSE;
			refresh();
		} else if (ui.connected && !ui.logging_out) {
			/* Empty pages need the directory's latest delivery ID. */
			refresh_visible_history();
		}
		break;
	case HISTORY:
		if (id == wire.history_pending)
			wire.history_pending = 0;
		if (id != wire.newest_history ||
		    g_strcmp0(saved->chat, ui.selected))
			break;
		ui.older_pending = FALSE;
		if (error || !json_object_is_type(result, json_type_array))
			set_string(
				&ui.notice,
				error ? error :
					_("Could not read the conversation."));
		else {
			wire.rendered_history = id;
			if (ui.older_page)
				ui.stick_bottom = TRUE;
			ui.older_page = FALSE;
			ui.has_older = json_object_array_length(result) >= 50;
			set_json(&ui.history, result);
			set_string(&ui.notice, "");
		}
		if (wire.history_again && !ui.older_page && !ui.logging_out)
			request_history();
		break;
	case MARK_READ: {
		UiReadCursor *cursor =
			wire.read_marks ? g_hash_table_lookup(wire.read_marks,
							      saved->chat) :
					  NULL;
		if (!cursor)
			break;
		gboolean confirmed =
			!error &&
			!g_strcmp0(ui_string(result, "chat", NULL),
				   saved->chat) &&
			!g_strcmp0(ui_string(result, "id", NULL), saved->text);
		Json *row = chat_summary(saved->chat);
		guint64 target = ui_read_id(saved->text);

		if (ui_read_finish(cursor, target, confirmed,
				   ui_read_id(ui_string(row, "lastId", NULL)))) {
			json_object_object_add(row, "unread",
					       json_object_new_int(0));
			ui_forget_rail();
			json_object_object_add(
				row, "lastSeenId",
				json_object_new_string(saved->text));
		}
		if (!confirmed && cursor->confirmed < target &&
		    !g_strcmp0(ui.selected, saved->chat) &&
		    !g_strcmp0(ui_string(row, "lastId", NULL), saved->text))
			set_string(
				&ui.notice,
				_("Read receipt unconfirmed. Refresh to retry."));
		if (confirmed)
			ui_refilter_chats();
		ui.dirty = TRUE;
		break;
	}
	case OLDER: {
		if (id != wire.newest_history ||
		    g_strcmp0(saved->chat, ui.selected))
			break;
		ui.older_pending = FALSE;
		Json *rows = ui_field(result, "messages");

		if (error || !json_object_is_type(rows, json_type_array)) {
			set_string(
				&ui.notice,
				error ? error :
					_("Could not read earlier messages."));
			break;
		}
		ui.has_older =
			json_object_get_boolean(ui_field(result, "more"));
		if (!json_object_array_length(rows)) {
			set_string(&ui.notice, _("Beginning of conversation."));
			break;
		}
		ui.older_page = TRUE;
		ui.stick_bottom = TRUE;
		ui_scroll_reset(&history_scroll, 0);
		set_json(&ui.history, rows);
		set_string(&ui.notice, "");
		break;
	}
	case MEDIA:
		if (!strcmp(saved->action, "foreground"))
			break;
		if (!ui_liff_reply(saved->action, saved->tag, result, error) &&
		    !ui_profile_reply(saved->action, saved->tag, result,
				      error) &&
		    !ui_find_reply(saved->action, saved->tag, result, error) &&
		    !ui_groups_reply(saved->action, saved->tag, result,
				     error) &&
		    !ui_friends_reply(saved->action, saved->tag, result, error))
			ui_media_reply(saved->action, saved->params, saved->tag,
				       result, error);
		break;
	case SEND:
		if (!error && json_object_is_type(result, json_type_object) &&
		    !g_strcmp0(ui.reply_id,
			       ui_string(saved->params, "replyTo", NULL)))
			ui_media_reply_sent(saved->chat);
		g_hash_table_remove(ui.sending, saved->chat);
		if (error || !json_object_is_type(result, json_type_object))
			set_string(&ui.notice,
				   error ? error :
					   _("The message was not confirmed."));
		else if (!g_strcmp0(ui.selected, saved->chat)) {
			show_sent(saved, result);
			if (same_draft(saved->chat, ui.message.value->str,
				       saved->params)) {
				ui_emoji_forget(saved->chat);
				forget_mentions(saved->chat);
				set_text(&ui.message, "");
			}
			ui_show_latest();
		} else if (same_draft(saved->chat,
				      g_hash_table_lookup(ui.drafts,
							  saved->chat),
				      saved->params)) {
			ui_emoji_forget(saved->chat);
			forget_mentions(saved->chat);
			g_hash_table_remove(ui.drafts, saved->chat);
		}
		break;
	case LOGOUT:
		if (error) {
			ui.logging_out = FALSE;
			set_string(&ui.notice, error);
		} else {
			gboolean confirmed = json_object_get_boolean(
				ui_field(result, "confirmed"));

			ui_liff_close();
			reset_login(
				confirmed ?
					_("Signed out.") :
					_("Signed out on this computer. LINE did not confirm it, so check linked devices on your phone."));
		}
		break;
	}
}

static void handle_packet(Json *packet)
{
	Json *packet_id = ui_field(packet, "id");

	if (json_object_is_type(packet_id, json_type_int)) {
		guint id = (guint)json_object_get_int64(packet_id);
		struct ui_request *saved = g_hash_table_lookup(
			wire.requests, GUINT_TO_POINTER(id));
		if (saved) {
			const char *error = ui_string(packet, "error", NULL);

			g_hash_table_steal(wire.requests, GUINT_TO_POINTER(id));
			if (saved->method == LOGIN && error)
				note_sign_in_failure(packet);
			if (saved->method == LOGIN && error &&
			    json_object_get_boolean(ui_field(packet, "retry")))
				retry_login(error);
			else
				handle_reply(id, saved,
					     ui_field(packet, "result"), error);
			free_request(saved);
		}
		return;
	}
	const char *event = ui_string(packet, "event", "");

	if (!strcmp(event, "call_incoming") && ui.connected &&
	    notify_in_background()) {
		Json *chat = chat_summary(ui_string(packet, "chat", NULL));

		ui_desktop_notify(ui_string(chat, "name", NULL), TRUE);
	}
	if ((!strcmp(event, "call") || !strcmp(event, "call_incoming") ||
	     !strcmp(event, "call_reception") ||
	     !strcmp(event, "call_participants") ||
	     !strcmp(event, "call_video_peer") ||
	     !strcmp(event, "call_error") || !strcmp(event, "call_frame") ||
	     !strcmp(event, "call_self_frame") ||
	     !strcmp(event, "call_share") ||
	     !strcmp(event, "call_share_frame") ||
	     !strcmp(event, "call_camera_error")) &&
	    ui.connected)
		ui_media_call_event(packet);
	else if (!strcmp(event, "signed_out"))
		reset_login(ui_string(packet, "message",
				      _("Your LINE session ended.")));
	else if (!strcmp(event, "qr") && ui.authenticating) {
		g_clear_pointer(&ui.qr, QRcode_free);
		g_clear_pointer(&ui.pin, g_free);
		ui.qr = QRcode_encodeString8bit(ui_string(packet, "url", ""), 0,
						QR_ECLEVEL_M);
		if (ui.qr)
			set_string(
				&ui.notice,
				_("Scan this code with LINE on your phone."));
		else
			reset_login(
				_("Could not render the LINE sign-in code."));
	} else if (!strcmp(event, "pin") && ui.authenticating) {
		g_clear_pointer(&ui.qr, QRcode_free);
		set_string(&ui.pin, ui_string(packet, "pin", ""));
		set_string(&ui.notice,
			   _("Enter this code in LINE on your phone."));
	} else if (!strcmp(event, "status"))
		set_string(&ui.notice, ui_string(packet, "message", ""));
	else if (!strcmp(event, "sync_unavailable") && ui.connected)
		set_refresh_notice(ui_string(packet, "message", ""));
	else if (!strcmp(event, "changed") && ui.connected && !ui.logging_out)
		refresh();
	else if (!strcmp(event, "reconnected"))
		refresh_content();
	else if (!strcmp(event, "chat_message") && ui.connected &&
		 !ui.logging_out)
		chat_message(packet);
	else if (!strcmp(event, "message_reads") && ui.connected &&
		 !ui.logging_out)
		ui_message_reads(packet);
	else if (!strcmp(event, "history") && ui.connected)
		pushed_history(packet);
	else if ((!strcmp(event, "self") || !strcmp(event, "contact") ||
		  !strcmp(event, "friend_added") ||
		  !strcmp(event, "friend_request")) &&
		 ui.connected && !ui.logging_out)
		ui_profile_packet(packet);
}

static gboolean inside(Rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static Editor *active_editor(void)
{
	if (ui.focus == F_LOGIN_EMAIL)
		return &ui.login_email;
	if (ui.focus == F_LOGIN_PASSWORD)
		return &ui.login_password;
	if (ui.focus == F_SEARCH)
		return &ui.search;
	return ui.focus == F_MESSAGE && ui.selected ? &ui.message : NULL;
}

static Rect editor_rect(const Editor *edit)
{
	if (edit == &ui.login_email)
		return ui.email_box;
	if (edit == &ui.login_password)
		return ui.password_box;
	return edit == &ui.search ? ui.search_box : ui.entry;
}

static int editor_hit(Editor *edit, int x)
{
	return edit == &ui.login_password ?
		       ui_password_index(x) :
		       ui_edit_hit(edit, editor_rect(edit), x);
}

/* Use Unicode grapheme boundaries (including Thai combining marks and ZWJ emoji). */
static int step(Editor *edit, int dir, gboolean word)
{
	const char *s = edit->value->str;
	int chars = (int)g_utf8_strlen(s, -1),
	    pos = (int)g_utf8_pointer_to_offset(s, s + edit->cursor);
	g_autofree PangoLogAttr *attrs = g_new0(PangoLogAttr, chars + 1);

	pango_get_log_attrs(s, (int)edit->value->len, -1,
			    pango_language_get_default(), attrs, chars + 1);
	do {
		pos += dir;
		if (pos <= 0)
			return 0;
		if (pos >= chars)
			return (int)edit->value->len;
	} while (word ? !(dir < 0 ? attrs[pos].is_word_start :
				    attrs[pos].is_word_end) :
			!attrs[pos].is_cursor_position);
	return (int)(g_utf8_offset_to_pointer(s, pos) - s);
}

void ui_insert(Editor *edit, const char *s)
{
	if (!g_utf8_validate(s, -1, NULL))
		return;
	int left = MIN(edit->cursor, edit->anchor),
	    right = MAX(edit->cursor, edit->anchor);

	if (edit == &ui.login_email || edit == &ui.login_password) {
		size_t n = strlen(s);

		if (strpbrk(s, "\r\n\t") ||
		    n > LINE_CREDENTIAL_LIMIT -
				    (edit->value->len - right + left))
			return;
		g_string_erase(edit->value, left, right - left);
		g_string_insert_len(edit->value, left, s, n);
		if (edit == &ui.login_password)
			explicit_bzero(edit->value->str + edit->value->len,
				       edit->value->allocated_len -
					       edit->value->len);
		edit->cursor = edit->anchor = left + (int)n;
		set_string(&ui.composition, "");
		ui.dirty = TRUE;
		return;
	}
	g_string_erase(edit->value, left, right - left);
	g_autofree char *single = g_strdup(s);

	for (char *p = single; *p; p++)
		if (*p == '\r' || *p == '\n' || *p == '\t')
			*p = ' ';
	if (edit == &ui.message) {
		ui_emoji_edit(left, right, strlen(single));
		edit_mentions(left, right, strlen(single));
	}
	g_string_insert(edit->value, left, single);
	edit->cursor = edit->anchor = left + (int)strlen(single);
	set_string(&ui.composition, "");
	if (edit == &ui.search) {
		ui.list_offset = 0;
		ui_scroll_reset(&list_scroll, 0);
		ui_refilter_chats();
	}
}

void ui_mention_member(const char *id, const char *name)
{
	if (!ui.selected || ui.selected[0] != 'c' || !id || id[0] != 'u' ||
	    strlen(id) != 33 ||
	    strspn(id + 1, "0123456789abcdefABCDEF") != 32 || !name || !*name ||
	    !g_utf8_validate(name, -1, NULL))
		return;
	GArray *list = mention_list(ui.selected);

	if (list && list->len >= 20) {
		ui_status(_("A message can mention up to 20 people."));
		return;
	}
	if (!mentions)
		mentions = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free,
						 (GDestroyNotify)g_array_unref);
	if (!list) {
		list = g_array_new(FALSE, FALSE, sizeof(struct draft_mention));
		g_hash_table_insert(mentions, g_strdup(ui.selected), list);
	}
	Editor *editor = &ui.message;
	int start = MIN(editor->cursor, editor->anchor);
	g_autofree char *label = g_strdup_printf("@%s ", name);

	ui_insert(editor, label);
	struct draft_mention member = { .start = start,
					.end = editor->cursor - 1 };
	g_strlcpy(member.id, id, sizeof(member.id));
	guint index = 0;

	while (index < list->len &&
	       g_array_index(list, struct draft_mention, index).start < start)
		index++;
	g_array_insert_val(list, index, member);
	focus_on(F_MESSAGE);
	ui.dirty = TRUE;
}

static void tab_focus(gboolean backwards)
{
	if (!ui.connected) {
		if (ui.authenticating)
			return;
		const Focus controls[] = { F_LOGIN, F_LOGIN_MODE, F_LOGIN_EMAIL,
					   F_LOGIN_PASSWORD };
		int n = ui.email_login ? G_N_ELEMENTS(controls) : 2;
		int i = 0;

		while (i < n && controls[i] != ui.focus)
			i++;
		focus_on(controls[(i + (backwards ? -1 : 1) + n) % n]);
		return;
	}
	const Focus all[] = { F_FRIENDS, F_GROUP_NEW, F_MEMBERS, F_FIND_TEXT,
			      F_SEARCH,	 F_LIST,      F_MESSAGE, F_SEND,
			      F_RELOAD,	 F_LOGOUT };
	const Focus list[] = { F_FRIENDS, F_GROUP_NEW, F_SEARCH,
			       F_LIST,	  F_RELOAD,    F_LOGOUT };
	const Focus thread[] = { F_BACK, F_MEMBERS, F_FIND_TEXT, F_MESSAGE,
				 F_SEND };
	const Focus *cycle = all;
	int n = G_N_ELEMENTS(all), i = 0;

	if (ui_compact()) {
		cycle = ui_chat_visible() ? thread : list;
		n = ui_chat_visible() ? G_N_ELEMENTS(thread) :
					G_N_ELEMENTS(list);
	}
	while (i < n && cycle[i] != ui.focus)
		i++;
	do
		i = (i + (backwards ? -1 : 1) + n) % n;
	while ((cycle[i] == F_FIND_TEXT &&
		(!ui.selected || !ui.find_messages)) ||
	       (cycle[i] == F_FRIENDS && !ui.friends) ||
	       (cycle[i] == F_GROUP_NEW && !ui.groups) ||
	       (cycle[i] == F_MEMBERS &&
		(!ui.groups || !ui.selected || *ui.selected != 'c')) ||
	       (!ui.selected &&
		(cycle[i] == F_MESSAGE || cycle[i] == F_SEND)) ||
	       (!ui.visible->len && cycle[i] == F_LIST));
	focus_on(cycle[i]);
}

static void key(SDL_KeyboardEvent k)
{
	SDL_Keycode code = k.keysym.sym;
	gboolean ctrl = (k.keysym.mod & KMOD_CTRL) != 0,
		 shift = (k.keysym.mod & KMOD_SHIFT) != 0;
	if (ui.composition && *ui.composition && active_editor())
		return; /* IME owns composing keys. */
	if (code == SDLK_TAB) {
		tab_focus(shift);
		return;
	}
	if (!ui.connected) {
		Editor *edit = active_editor();
		gboolean activate = !k.repeat &&
				    (code == SDLK_RETURN ||
				     code == SDLK_KP_ENTER ||
				     (code == SDLK_SPACE && !edit));

		if (activate && ui.focus == F_LOGIN_MODE && ui.saved_session)
			logout();
		else if (activate && ui.focus == F_LOGIN_MODE)
			toggle_sign_in();
		else if (activate || (code == SDLK_ESCAPE && ui.authenticating))
			login(FALSE);
		else if (edit)
			ui_edit_key(edit, k);
		return;
	}
	if (code == SDLK_ESCAPE && ui_compact() && ui_chat_visible()) {
		focus_on(F_LIST);
		return;
	}
	int shortcut = ui_list_shortcut(k);

	if (shortcut == LIST_REFRESH) {
		refresh_content();
		return;
	}
	if (ctrl && (code == SDLK_PAGEUP || code == SDLK_END) &&
	    ui_chat_visible()) {
		history_page(code == SDLK_END);
		return;
	}
	if (shortcut == LIST_FIND && ui.selected && ui.find_messages) {
		ui_find_open();
		return;
	}
	if (shortcut == LIST_SEARCH) {
		focus_on(F_SEARCH);
		return;
	}
	if ((code == SDLK_PAGEUP || code == SDLK_PAGEDOWN) &&
	    ui_chat_visible()) {
		scroll_history((code == SDLK_PAGEUP ? -1 : 1) *
			       ui.conversation.h);
		return;
	}
	if ((ui.focus == F_LIST || ui.focus == F_SEARCH) &&
	    (code == SDLK_UP || code == SDLK_DOWN)) {
		if (ui_compact()) {
			if (ui.visible->len)
				highlight(CLAMP(ui.list_cursor +
							(code == SDLK_UP ? -1 :
									   1),
						0, (int)ui.visible->len - 1));
			focus_on(F_LIST);
			return;
		}
		int pos = -1;

		for (guint i = 0; i < ui.visible->len; i++) {
			Json *c = json_object_array_get_idx(
				ui.chats, g_array_index(ui.visible, guint, i));
			if (!g_strcmp0(ui.selected, ui_string(c, "id", ""))) {
				pos = (int)i;
				break;
			}
		}
		select_chat(CLAMP(pos + (code == SDLK_UP ? -1 : 1), 0,
				  MAX(0, (int)ui.visible->len - 1)));
		focus_on(F_LIST);
		return;
	}
	if (code == SDLK_RETURN || code == SDLK_KP_ENTER) {
		if (k.repeat)
			return;
		if (ui.focus == F_SEARCH) {
			select_chat(0);
			return;
		}
		if (ui.focus == F_FRIENDS)
			ui_friends_show();
		else if (ui.focus == F_GROUP_NEW)
			ui_groups_new();
		else if (ui.focus == F_MEMBERS)
			ui_groups_members();
		else if (ui.focus == F_FIND_TEXT)
			ui_find_open();
		else if (ui.focus == F_LIST)
			select_chat(MAX(0, ui.list_cursor));
		else if (ui.focus == F_BACK)
			focus_on(F_LIST);
		else if (ui.focus == F_SEND || ui.focus == F_MESSAGE)
			send_message();
		else if (ui.focus == F_RELOAD)
			refresh_content();
		else if (ui.focus == F_LOGOUT)
			logout();
		return;
	}
	Editor *edit = active_editor();

	if (!edit)
		return;
	ui_edit_key(edit, k);
}

void ui_edit_key(Editor *edit, SDL_KeyboardEvent k)
{
	SDL_Keycode code = k.keysym.sym;
	gboolean ctrl = (k.keysym.mod & KMOD_CTRL) != 0,
		 shift = (k.keysym.mod & KMOD_SHIFT) != 0;
	if (ctrl && code == SDLK_a) {
		edit->anchor = 0;
		edit->cursor = (int)edit->value->len;
	} else if (ctrl && (code == SDLK_c || code == SDLK_x)) {
		if (edit == &ui.login_password)
			return;
		int l = MIN(edit->cursor, edit->anchor),
		    r = MAX(edit->cursor, edit->anchor);
		if (l != r) {
			g_autofree char *copy = ui_emoji_export(
				edit == &ui.message ? ui.selected : NULL,
				edit->value->str, l, r, NULL);
			if (SDL_SetClipboardText(copy) == 0 && code == SDLK_x)
				ui_insert(edit, "");
		}
	} else if (ctrl && code == SDLK_v && edit == &ui.message &&
		   ui_media_paste()) {
		/* A picture or file went to the send preview. */
	} else if (ctrl && code == SDLK_v) {
		char *clip = SDL_GetClipboardText();

		if (clip && *clip)
			ui_insert(edit, clip);
		if (clip && edit == &ui.login_password)
			explicit_bzero(clip, strlen(clip));
		SDL_free(clip);
	} else if (code == SDLK_BACKSPACE || code == SDLK_DELETE) {
		if (edit->anchor == edit->cursor)
			edit->anchor = step(
				edit, code == SDLK_BACKSPACE ? -1 : 1, ctrl);
		ui_insert(edit, "");
	} else if (code == SDLK_HOME || code == SDLK_END || code == SDLK_LEFT ||
		   code == SDLK_RIGHT) {
		if (code == SDLK_HOME)
			edit->cursor = 0;
		else if (code == SDLK_END)
			edit->cursor = (int)edit->value->len;
		else if (!shift && edit->cursor != edit->anchor)
			edit->cursor = code == SDLK_LEFT ?
					       MIN(edit->anchor, edit->cursor) :
					       MAX(edit->anchor, edit->cursor);
		else
			edit->cursor =
				step(edit, code == SDLK_LEFT ? -1 : 1, ctrl);
		if (!shift)
			edit->anchor = edit->cursor;
	}
	ui.dirty = TRUE;
}

static void click(int x, int y)
{
	if (ui.connected && ui.selected && ui_chat_visible()) {
		if (ui.has_older && inside(ui.older_button, x, y)) {
			history_page(FALSE);
			return;
		}
		if (ui.older_page && inside(ui.latest_button, x, y)) {
			history_page(TRUE);
			return;
		}
		const char *uri = ui_flex_link_at(x, y);

		if (uri && (line_flex_link_allowed(uri) ||
			    line_liff_url(uri, NULL, NULL))) {
			ui_liff_open_link(uri);
			return;
		}
		Json *tapped = NULL;
		Json *rich = ui_rich_area_at(x, y, &tapped, NULL);

		if (rich) {
			rich_action(rich, tapped);
			return;
		}
		/* Discord opens a profile from an avatar or a name. */
		Rect anchor;
		Json *message = ui_sender_at(x, y, &anchor);

		if (message) {
			gboolean mine = json_object_get_boolean(
				ui_field(message, "own"));
			const char *id = ui_string(message, "senderId",
						   mine ? ui.self_id : NULL);
			Json *who = json_object_new_object();

			json_object_object_add(who, "id",
					       json_object_new_string(id ? id :
									   ""));
			json_object_object_add(
				who, "name",
				json_object_new_string(ui_string(
					message, "from",
					mine && ui.name ? ui.name : "")));
			json_object_object_add(
				who, "avatarKey",
				json_object_new_string(
					ui_string(message, "avatarKey", "")));
			ui_profile_open(who, ui.selected, anchor);
			json_object_put(who);
			return;
		}
		if (ui_header_peer_at(x, y, &anchor)) {
			Json *chat = ui_selected_chat();
			Json *who = json_object_new_object();

			json_object_object_add(
				who, "id", json_object_new_string(ui.selected));
			json_object_object_add(
				who, "name",
				json_object_new_string(
					ui_string(chat, "name", ui.selected)));
			json_object_object_add(who, "avatarKey",
					       json_object_new_string(ui_string(
						       chat, "avatarKey", "")));
			ui_profile_open(who, ui.selected, anchor);
			json_object_put(who);
			return;
		}
	}
	if (ui.connected && ui_profile_panel_click(x, y))
		return;
	if (!ui.connected) {
		if (inside(ui.login, x, y))
			login(FALSE);
		else if (!ui.authenticating && ui.saved_session &&
			 inside(ui.login_mode, x, y))
			logout();
		else if (!ui.authenticating && inside(ui.login_mode, x, y))
			toggle_sign_in();
		else if (!ui.authenticating && ui.email_login &&
			 (inside(ui.email_box, x, y) ||
			  inside(ui.password_box, x, y))) {
			focus_on(inside(ui.email_box, x, y) ? F_LOGIN_EMAIL :
							      F_LOGIN_PASSWORD);
			Editor *edit = active_editor();

			edit->cursor = edit->anchor = editor_hit(edit, x);
		}
	} else if (ui_list_visible() && inside(ui.rail_chats, x, y))
		focus_on(F_LIST);
	else if (ui_list_visible() && ui.friends &&
		 inside(ui.friends_button, x, y))
		ui_friends_show();
	else if (ui_list_visible() && ui.groups &&
		 (inside(ui.new_group, x, y) || inside(ui.rail_add, x, y)))
		ui_groups_new();
	else if (ui_list_visible() && ui_rail_chat_at(x, y))
		select_id(ui_rail_chat_at(x, y));
	else if (ui_chat_visible() && ui.selected && ui.find_messages &&
		 inside(ui.find_button, x, y))
		ui_find_open();
	else if (ui_chat_visible() && ui.groups && ui.selected &&
		 *ui.selected == 'c' && inside(ui.members, x, y))
		ui_groups_members();
	else if (ui_compact() && ui_chat_visible() && inside(ui.back, x, y))
		focus_on(F_LIST);
	else if (ui_list_visible() && inside(ui.logout, x, y))
		logout();
	else if (ui_list_visible() && inside(ui.reload, x, y))
		refresh_content();
	else if ((ui_list_visible() && inside(ui.search_box, x, y)) ||
		 (ui_chat_visible() && ui.selected && inside(ui.entry, x, y))) {
		gboolean search = ui_list_visible() &&
				  inside(ui.search_box, x, y);
		focus_on(search ? F_SEARCH : F_MESSAGE);
		Editor *edit = active_editor();

		edit->cursor = edit->anchor = ui_text_index(edit, x);
	} else if (ui_chat_visible() && inside(ui.send, x, y))
		send_message();
	else if (ui_list_visible() && inside(ui.list_box, x, y))
		select_chat((y - ui.list_box.y + ui.list_offset) /
			    ui_chat_row_height());
	ui.dirty = TRUE;
}

/* Inputs with a visual hover state. Movement within one target never repaints. */
static int hover_target(int x, int y)
{
	int dock = ui_media_dock_hover(x, y);

	if (dock)
		return dock;
	if (!ui.connected)
		return inside(ui.login, x, y)				 ? 1 :
		       !ui.authenticating && inside(ui.login_mode, x, y) ? 2 :
									   0;
	int panel = ui_profile_panel_hover(x, y);

	if (panel)
		return panel;
	if (ui_chat_visible() && ui.selected && ui.find_messages &&
	    inside(ui.find_button, x, y))
		return -9;
	if (ui_list_visible() && ui.friends && inside(ui.friends_button, x, y))
		return -6;
	if (ui_list_visible() && ui.groups && inside(ui.new_group, x, y))
		return -7;
	if (ui_list_visible() && inside(ui.rail_chats, x, y))
		return -2001;
	if (ui_list_visible() && ui.groups && inside(ui.rail_add, x, y))
		return -11;
	if (ui_list_visible() && ui_rail_chat_at(x, y))
		return -12 - (int)(g_str_hash(ui_rail_chat_at(x, y)) % 1000);
	if (ui_chat_visible() && ui.groups && ui.selected &&
	    *ui.selected == 'c' && inside(ui.members, x, y))
		return -8;
	if (ui_chat_visible() && ui.selected) {
		if ((ui.emojis || ui.stickers) && inside(ui.emoji, x, y))
			return -5;
		if ((ui.photos || ui.files) && inside(ui.attach, x, y))
			return -2;
		if (ui_media_can_call(FALSE) && inside(ui.call, x, y))
			return -3;
		if (ui_media_can_call(TRUE) && inside(ui.video, x, y))
			return -4;
	}
	if (ui_compact() && ui_chat_visible() && inside(ui.back, x, y))
		return -1;
	if (ui_list_visible() && ui_lists_ready() &&
	    inside(ui.folders_button, x, y))
		return -10;
	if (ui_list_visible() && inside(ui.reload, x, y))
		return 1;
	if (ui_list_visible() && inside(ui.logout, x, y))
		return 2;
	if (ui_chat_visible() && ui.selected && inside(ui.send, x, y))
		return 3;
	if (ui_list_visible() && inside(ui.list_box, x, y)) {
		int row = (y - ui.list_box.y + ui.list_offset) /
			  ui_chat_row_height();

		if ((guint)row < ui.visible->len)
			return 4 + row;
	}
	if (ui_chat_visible() && ui.selected && inside(ui.conversation, x, y)) {
		int row = ui_message_at(x, y), area;

		if (row >= 0 && ui_rich_area_at(x, y, NULL, &area))
			return 200000 + row * 64 + area;
		if (row >= 0)
			return 100000 + row;
	}
	return 0;
}

static Uint32 activate_event;

static void event(SDL_Event *e)
{
	if (activate_event && e->type == activate_event) {
		ui_present_window(ui.window);
		ui.dirty = TRUE;
		return;
	}
	if (ui_tray_event(e, ui.window, &ui.quitting))
		return;
	if (e->type == wire.preview_event) {
		guint source = GPOINTER_TO_UINT(e->user.data1);

		if ((guint)e->user.code != wire.generation || ui.quitting ||
		    source >= LINE_PREVIEW_COUNT)
			return;
		struct pending_preview *preview = &previews[source];

		g_mutex_lock(&preview_lock);
		int fd = preview->fd;
		struct line_preview_frame frame = preview->frame;

		preview->fd = -1;
		g_mutex_unlock(&preview_lock);
		if (fd >= 0) {
			ui_media_preview_frame(&frame, fd);
			close(fd);
		}
		return;
	}
	if (e->type == wire.packet_event) {
		g_autofree char *line = e->user.data1;

		if ((guint)e->user.code != wire.generation || ui.quitting)
			return;
		Json *packet = line ? json_tokener_parse(line) : NULL;

		if (json_object_is_type(packet, json_type_object))
			handle_packet(packet);
		else
			reset_login(_(
				"The LINE connection closed. Sign in again to reconnect."));
		json_object_put(packet);
		return;
	}
	if (e->type == wire.refresh_event) {
		if ((guint)e->user.code != wire.generation)
			return;
		if (e->user.data1 == &wire.login_timer) {
			wire.login_timer = 0;
			if (ui.authenticating && !ui.connected) {
				int wait = sign_in_wait();

				if (wait)
					arm_login_timer(wait);
				else
					request(LOGIN, NULL, NULL);
			}
		}
		return;
	}
	if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP ||
	    e->type == SDL_WINDOWEVENT)
		ui_draw_pointer(e);
	if (ui_media_event(e) || ui_liff_event(e))
		return;
	switch (e->type) {
	case SDL_QUIT:
		ui.quitting = TRUE;
		break;
	case SDL_RENDER_TARGETS_RESET:
	case SDL_RENDER_DEVICE_RESET:
		ui_draw_reset_renderer();
		break;
	case SDL_WINDOWEVENT:
		if (e->window.windowID != SDL_GetWindowID(ui.window))
			break;
		switch (e->window.event) {
		case SDL_WINDOWEVENT_SHOWN:
		case SDL_WINDOWEVENT_HIDDEN:
		case SDL_WINDOWEVENT_MINIMIZED:
		case SDL_WINDOWEVENT_MAXIMIZED:
		case SDL_WINDOWEVENT_RESTORED:
			ui_media_window_visibility(window_visible());
			break;
		}
		if (e->window.event == SDL_WINDOWEVENT_CLOSE) {
			if (ui_tray_hide(ui.window))
				SDL_FlushEvent(SDL_QUIT);
			else
				ui.quitting = TRUE;
		}
		if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
			ui.active = TRUE;
			report_foreground();
			ui_motion_refresh_preferences();
			display_rate();
		}
#if SDL_VERSION_ATLEAST(2, 0, 18)
		if (e->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED ||
		    e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
			display_rate();
#else
		if (e->window.event == SDL_WINDOWEVENT_MOVED)
			display_rate();
#endif
		if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
			ui.active = FALSE;
			report_foreground();
		}
		ui_layout();
		ui.dirty = TRUE;
		break;
	case SDL_KEYDOWN:
		key(e->key);
		break;
	case SDL_TEXTINPUT:
		if (active_editor())
			ui_insert(active_editor(), e->text.text);
		if (ui.focus == F_LOGIN_PASSWORD)
			explicit_bzero(e->text.text, sizeof(e->text.text));
		break;
#if SDL_VERSION_ATLEAST(2, 0, 9)
	case SDL_DISPLAYEVENT:
		display_rate();
		ui.dirty = TRUE;
		break;
#endif
	case SDL_TEXTEDITING:
		if (ui.focus == F_LOGIN_PASSWORD)
			explicit_bzero(e->edit.text, sizeof(e->edit.text));
		else if (active_editor())
			set_string(&ui.composition, e->edit.text);
		break;
	case SDL_MOUSEMOTION: {
		int old = hover_target(ui.hover_x, ui.hover_y);

		ui.hover_x = e->motion.x;
		ui.hover_y = e->motion.y;
		if ((e->motion.state & SDL_BUTTON_LMASK) && active_editor()) {
			Editor *edit = active_editor();
			Rect box = editor_rect(edit);

			if (inside(box, ui.hover_x, ui.hover_y)) {
				int cursor = editor_hit(edit, ui.hover_x);

				if (edit->cursor != cursor)
					ui.dirty = TRUE;
				edit->cursor = cursor;
			}
		}
		if (old != hover_target(ui.hover_x, ui.hover_y))
			ui.dirty = TRUE;
		break;
	}
	case SDL_MOUSEBUTTONDOWN:
		if (e->button.button == SDL_BUTTON_LEFT)
			click(e->button.x, e->button.y);
		break;
	case SDL_MOUSEWHEEL: {
		int x, y;

		SDL_GetMouseState(&x, &y);
		/* Natural or traditional, as the desktop has set it. */
		double distance = -48. * ui_wheel_y(&e->wheel);

		if (ui_list_visible() && inside(ui.list_box, x, y)) {
			scroll_by(&list_scroll, &ui.list_offset, distance,
				  max_list_scroll());
		} else if (ui_chat_visible() && inside(ui.conversation, x, y)) {
			scroll_history(distance);
		}
		break;
	}
	}
}

void ui_status(const char *s)
{
	set_string(&ui.notice, s ? s : "");
}

void ui_refresh(void)
{
	refresh();
}

void ui_show_latest(void)
{
	if (!ui.selected || !ui.connected)
		return;
	ui.stick_bottom = TRUE;
	if (wire.history_pending && !ui.older_pending)
		wire.history_again = TRUE;
	else
		request_history();
}

void ui_restore_input(void)
{
	if (ui_lists_active())
		ui_lists_resume_input();
	else if (ui_chat_tools_active())
		ui_chat_tools_resume_input();
	else if (ui_shared_active())
		ui_shared_resume_input();
	else if (ui_groups_active())
		ui_groups_resume_input();
	else if (ui_find_active())
		ui_find_resume_input();
	else if (ui_friends_active())
		ui_friends_resume_input();
	else
		focus_on(ui.focus);
}

int line_ui(int argc, char **argv)
{
	g_autoptr(GError) error = NULL;
	int primary = ui_instance_open(&error);
	int status = 1;

	(void)argc;
	(void)argv;
	if (primary < 0) {
		g_printerr("Avernella: %s\n", error->message);
		return 1;
	}
	if (!primary)
		return 0;
	if ((g_getenv("WAYLAND_DISPLAY") || g_getenv("WAYLAND_SOCKET") ||
	     !g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland")) &&
	    !g_getenv("SDL_VIDEODRIVER") && !g_getenv("SDL_VIDEO_DRIVER"))
		SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
	/* sdl2-compat defaults to pixel-sized Wayland windows. Keep the
	 * logical size by setting this before the video backend starts.
	 */
	if (!g_getenv("SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY"))
		SDL_SetHintWithPriority("SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY",
					"0", SDL_HINT_OVERRIDE);
	SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
	SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
	SDL_SetHint("SDL_QUIT_ON_LAST_WINDOW_CLOSE", "0");
	/* Desktops match the window to avernella.desktop by this ID. */
	SDL_SetHint("SDL_APP_ID", "avernella");
	SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "avernella");
	SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "avernella");
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		g_printerr("SDL: %s\n", SDL_GetError());
		ui_instance_close();
		return 1;
	}
	ui_sound_init();
	wire.packet_event = SDL_RegisterEvents(4);
	if (wire.packet_event == (Uint32)-1) {
		g_printerr("No SDL event IDs are available.\n");
		goto quit_sdl;
	}
	wire.refresh_event = wire.packet_event + 1;
	wire.preview_event = wire.packet_event + 2;
	activate_event = wire.packet_event + 3;
	if (!ui_instance_watch(activate_event, &error)) {
		g_printerr("Avernella: %s\n", error->message);
		goto quit_sdl;
	}
	/* The Wayland window-surface fallback still uses SDL presentation. */
	gboolean wayland = g_strcmp0(SDL_GetCurrentVideoDriver(), "wayland") ==
			   0;
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, wayland ? "1" : "0");
	if (wayland)
		SDL_SetHintWithPriority(SDL_HINT_RENDER_VSYNC, "1",
					SDL_HINT_DEFAULT);
	ui_theme_init();
	ui_lang_init();
	ui.window = ui_gpu_window(
		"Avernella (" LINE_BUILD_COMMIT ")", SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, 920, 630,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!ui.window || !ui_draw_init()) {
		g_printerr("Window: %s\n", SDL_GetError());
		goto quit_sdl;
	}
	SDL_SetWindowMinimumSize(ui.window, 360, 480);
	wire.requests = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					      NULL, free_request);
	ui.sending =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ui.drafts =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	ui.visible = g_array_new(FALSE, FALSE, sizeof(guint));
	ui.search.value = g_string_new("");
	ui.message.value = g_string_new("");
	ui.login_email.value = g_string_new("");
	ui.login_password.value = g_string_sized_new(LINE_CREDENTIAL_LIMIT + 1);
	ui_tray_start();
	display_rate();
	ui_motion_refresh_preferences();
	ui.active = TRUE;
	ui.hover_x = ui.hover_y = -1;
	ui_layout();
	reset_login("");
	/* Resume a saved session at launch; only Sign out or LINE ends it. */
	if (line_store_has_session())
		login(TRUE);
	gboolean visual_animation = FALSE;

	while (!ui.quitting) {
		gint64 now = g_get_monotonic_time();
		int wait_ms = frame_wait(now, visual_animation);

		if (wait_ms == 0) {
			ui_frame_started(&frame_clock, now);
			int old_list = ui.list_offset,
			    old_history = ui.history_offset;
			sample_scroll(now);
			gboolean list_changed = old_list != ui.list_offset,
				 history_changed = old_history !=
						   ui.history_offset;
			gboolean scrolled = list_changed || history_changed;
			gboolean full = ui.dirty ||
					(visual_animation &&
					 (scrolled || ui.video_dirty)) ||
					(scrolled && (ui_emoji_deadline() ||
						      ui_media_layer())) ||
					(list_changed && history_changed);
			gboolean video_dirty = ui.video_dirty;

			ui.dirty = ui.video_dirty = FALSE;
			if (full)
				ui_draw();
			else if (visual_animation)
				ui_draw_animation();
			else if (scrolled)
				ui_draw_region(list_changed ? ui.list_box :
							      ui.conversation);
			else if (video_dirty)
				ui_draw_video();
			visual_animation = ui_animation_deadline(now) > now;
			/*
			 * The driver may have spent a refresh interval presenting. Count
			 * that time before deciding whether another timed wait is due.
			 */
			wait_ms = frame_wait(g_get_monotonic_time(),
					     visual_animation);
		}
		SDL_Event e;
		gboolean received = wait_ms < 0 ?
					    SDL_WaitEvent(&e) :
					    SDL_WaitEventTimeout(&e, wait_ms);
		if (received) {
			/*
			 * Input and worker packets wake any frame wait immediately. Bound
			 * event draining so continuous pointer input cannot starve paint.
			 */
			for (int budget = 64;;) {
				event(&e);
				if (ui.quitting || --budget == 0 ||
				    !SDL_PollEvent(&e))
					break;
			}
		}
	}
	ui_tray_stop();
	stop_worker(TRUE);
	g_clear_pointer(&ui.qr, QRcode_free);
	ui_media_shutdown();
	ui_lists_reset();
	g_free(ui.selected);
	g_clear_pointer(&mentions, g_hash_table_unref);
	json_object_put(ui.opened);
	g_free(ui.name);
	g_free(ui.notice);
	g_free(sign_in.notice);
	g_free(ui.pin);
	g_free(ui.composition);
	set_json(&ui.history, NULL);
	set_json(&ui.chats, NULL);
	g_hash_table_unref(wire.requests);
	g_hash_table_unref(ui.sending);
	g_hash_table_unref(ui.drafts);
	g_array_unref(ui.visible);
	g_string_free(ui.search.value, TRUE);
	g_string_free(ui.message.value, TRUE);
	clear_password();
	g_string_free(ui.login_email.value, TRUE);
	g_string_free(ui.login_password.value, TRUE);
	ui_liff_close();
	ui_draw_shutdown();
	ui_motion_shutdown();
	status = 0;
quit_sdl:
	ui_sound_shutdown();
	ui_instance_close();
	if (ui.window)
		SDL_DestroyWindow(ui.window);
	SDL_Quit();
	return status;
}

void ui_sign_out(void)
{
	logout();
}

const char *ui_build_commit(void)
{
	return LINE_BUILD_COMMIT;
}
