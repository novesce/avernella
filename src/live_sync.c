// SPDX-License-Identifier: GPL-3.0-or-later
#include "live_sync.h"
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

enum {
	/* x-lst: how long LINE may hold an idle sync request. */
	LONG_POLL_SECONDS = 50,
	/* A sync answered faster than this without news is not long-polled. */
	QUICK_REPLY_US = 800000,
};

struct line_live_sync {
	GThread *thread;
	GMutex lock;
	GCond wake_thread;
	int wake;
	gboolean stop;
	char *access, *rotated, *rotated_from;
	guint64 access_generation;
	J *ops;
	gboolean reload;
	gboolean denied; /* LINE refused the token: no more requests */
	gint64 revision;
	guint failures;
	gint64 failed_since, recovered_since;
	struct line_talk_sync sync; /* owned by the thread */
};

static void notify(struct line_live_sync *l)
{
	eventfd_t one = 1;

	(void)eventfd_write(l->wake, one);
}

static void collect(J *op, gpointer data)
{
	json_object_array_add(data, json_object_get(op));
}

/* Sleeps up to 'us' unless stopped; returns FALSE once stopping. */
static gboolean pause_for(struct line_live_sync *l, gint64 us)
{
	gint64 until = g_get_monotonic_time() + us;
	gboolean running;

	g_mutex_lock(&l->lock);
	while (!l->stop && g_cond_wait_until(&l->wake_thread, &l->lock, until))
		;
	running = !l->stop;
	g_mutex_unlock(&l->lock);
	return running;
}

static void destroy(struct line_live_sync *l)
{
	line_transport_release();
	line_secret_free(l->access);
	line_secret_free(l->rotated);
	line_secret_free(l->rotated_from);
	json_object_put(l->ops);
	line_talk_sync_clear(&l->sync);
	close(l->wake);
	g_mutex_clear(&l->lock);
	g_cond_clear(&l->wake_thread);
	g_free(l);
}

static gpointer run(gpointer data)
{
	struct line_live_sync *l = data;

	for (;;) {
		Line session = { 0 };

		g_mutex_lock(&l->lock);
		gboolean stopping = l->stop;
		guint64 generation = l->access_generation;

		session.access = stopping ? NULL : g_strdup(l->access);
		g_mutex_unlock(&l->lock);
		if (stopping)
			break;
		g_autoptr(J) args = line_talk_sync_request(&l->sync);
		g_autoptr(GError) error = NULL;
		gint64 started = g_get_monotonic_time();
		g_autoptr(J) reply = line_call(&session, "/SYNC4", "sync", args,
					       session.access,
					       LONG_POLL_SECONDS, &error);
		gint64 took = g_get_monotonic_time() - started;
		gboolean idle_timeout =
			took >= LONG_POLL_SECONDS * (gint64)G_USEC_PER_SEC &&
			g_error_matches(error, LINE_ERROR, LINE_TIMEOUT);
		if (session.denied ||
		    g_error_matches(error, LINE_ERROR, LINE_ACCESS_DENIED)) {
			/*
			 * LINE refused this token. As in LINE's own client, no
			 * further request goes out with it: report it once and
			 * wait for the worker to confirm and stop this thread.
			 */
			g_mutex_lock(&l->lock);
			l->denied = TRUE;
			g_mutex_unlock(&l->lock);
			line_secret_free(session.access);
			notify(l);
			while (pause_for(l, (gint64)3600 * G_USEC_PER_SEC))
				;
			break;
		}
		g_autoptr(J) ops = json_object_new_array();
		gboolean reload = FALSE, more = FALSE;

		if (reply)
			more = line_talk_sync_apply(&l->sync, reply, collect,
						    ops, &reload);
		if (!g_strcmp0(g_getenv("LINE_DEBUG_SYNC"), "1"))
			g_printerr(
				"line-sync %s after %lld ms, %zu operations%s\n",
				reply ? "reply" :
				error ? error->message :
					"?",
				(long long)(took / 1000),
				json_object_array_length(ops),
				more ? ", more" : "");
		g_mutex_lock(&l->lock);
		stopping = l->stop;
		if (generation == l->access_generation &&
		    session.token_changed && session.access &&
		    g_strcmp0(session.access, l->access)) {
			if (!l->rotated_from)
				l->rotated_from = l->access;
			else
				line_secret_free(l->access);
			l->access = g_strdup(session.access);
			line_secret_set(&l->rotated, g_strdup(session.access));
		}
		gboolean news = json_object_array_length(ops) || reload ||
				l->rotated;

		if (reply) {
			if (!l->ops)
				l->ops = json_object_new_array();
			for (size_t i = 0; i < json_object_array_length(ops);
			     i++)
				json_object_array_add(
					l->ops,
					json_object_get(
						json_object_array_get_idx(ops,
									  i)));
			l->reload |= reload;
			l->revision = l->sync.revision;
			if (l->failures) {
				l->recovered_since = l->failed_since;
				news = TRUE;
			}
			l->failures = 0;
		} else if (!idle_timeout) {
			if (!l->failures)
				l->failed_since = started + took;
			l->failures = MIN(l->failures + 1, 6u);
			news = TRUE;
		}
		guint failures = l->failures;
		g_mutex_unlock(&l->lock);
		line_secret_free(session.access);
		if (stopping)
			break;
		if (news)
			notify(l);
		if (failures) {
			gint64 wait = line_transport_retry_after();

			wait = MAX(wait, line_retry_delay(failures, 1, 32));
			if (!pause_for(l, wait * G_USEC_PER_SEC))
				break;
		} else if (reply && !more && !json_object_array_length(ops) &&
			   took < QUICK_REPLY_US) {
			/* Not held open by the server: pace like a poll. */
			if (!pause_for(l, G_USEC_PER_SEC - took))
				break;
		}
	}
	destroy(l);
	return NULL;
}

struct line_live_sync *line_live_sync_start(const struct line_talk_sync *from,
					    const char *access, GError **error)
{
	struct line_live_sync *l = g_new0(struct line_live_sync, 1);

	g_mutex_init(&l->lock);
	g_cond_init(&l->wake_thread);
	l->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	l->access = g_strdup(access);
	l->sync = (struct line_talk_sync){
		.revision = from->revision,
		.global_revision = from->global_revision,
		.individual_revision = from->individual_revision,
	};
	l->revision = from->revision;
	if (l->wake < 0 || !access ||
	    !(l->thread = g_thread_try_new("line-sync", run, l, error))) {
		if (l->wake >= 0)
			close(l->wake);
		line_secret_free(l->access);
		g_mutex_clear(&l->lock);
		g_cond_clear(&l->wake_thread);
		g_free(l);
		return NULL;
	}
	return l;
}

void line_live_sync_stop(struct line_live_sync *l)
{
	if (!l)
		return;
	g_mutex_lock(&l->lock);
	GThread *thread = l->thread;

	l->stop = TRUE;
	g_cond_signal(&l->wake_thread);
	g_mutex_unlock(&l->lock);
	/* The thread frees itself once its pending request returns. */
	g_thread_unref(thread);
}

int line_live_sync_fd(const struct line_live_sync *l)
{
	return l ? l->wake : -1;
}

void line_live_sync_set_access(struct line_live_sync *l, const char *access)
{
	if (!l || !access)
		return;
	g_mutex_lock(&l->lock);
	/* A worker may still be using the token awaiting delivery in take(). */
	if (g_strcmp0(l->access, access) &&
	    g_strcmp0(l->rotated_from, access)) {
		l->access_generation++;
		line_secret_set(&l->access, g_strdup(access));
		line_secret_set(&l->rotated, NULL);
		line_secret_set(&l->rotated_from, NULL);
	}
	g_mutex_unlock(&l->lock);
}

void line_live_sync_take(struct line_live_sync *l,
			 struct line_live_batch *batch)
{
	eventfd_t n;

	*batch = (struct line_live_batch){ 0 };
	(void)eventfd_read(l->wake, &n);
	g_mutex_lock(&l->lock);
	batch->ops = g_steal_pointer(&l->ops);
	batch->reload = l->reload;
	batch->access = g_steal_pointer(&l->rotated);
	batch->access_from = g_steal_pointer(&l->rotated_from);
	batch->revision = l->revision;
	batch->failing = l->failures >= 2;
	if (!l->failures) {
		batch->recovered_since = l->recovered_since;
		l->recovered_since = 0;
	}
	batch->denied = l->denied;
	l->reload = FALSE;
	g_mutex_unlock(&l->lock);
}
