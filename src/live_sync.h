/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "talk_sync.h"

/*
 * Talk operations by long poll. A dedicated thread with its own HTTPS
 * connection keeps one /SYNC4 sync request open; LINE answers it as soon as
 * an operation exists, so a message reaches the window one round trip after
 * it was sent instead of after the next timer tick.
 */
struct line_live_sync;

/* Starts after the worker established the cursor; 'access' is copied. */
struct line_live_sync *line_live_sync_start(const struct line_talk_sync *from,
					    const char *access,
					    GError **error);
/* Detaches the thread; it exits after its current request. */
void line_live_sync_stop(struct line_live_sync *live);
/* Readable when operations or a token are waiting. */
int line_live_sync_fd(const struct line_live_sync *live);
/* Shares a token the worker renewed or rotated. */
void line_live_sync_set_access(struct line_live_sync *live,
			       const char *access);

struct line_live_batch {
	J *ops;		/* array of operations in revision order, or NULL */
	gboolean reload; /* a full or partial resync is due */
	char *access;	/* a token LINE rotated on this connection, or NULL */
	char *access_from; /* worker token the pending rotation started from */
	gint64 revision; /* the cursor after these operations */
	gboolean failing; /* recent requests failed: poll as a fallback */
	gint64 recovered_since; /* start of a recovered outage, or 0 */
	/* LINE refused the session's credentials; the thread has stopped polling. */
	gboolean denied;
};

/* Takes everything received since the last call. */
void line_live_sync_take(struct line_live_sync *live,
			 struct line_live_batch *batch);
