/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

struct line_talk_sync {
	gint64 revision, global_revision, individual_revision;
	J *partial;
	gboolean more;
};

typedef void (*LineTalkOperation)(J *op, gpointer data);

void line_talk_sync_clear(struct line_talk_sync *sync);
J *line_talk_sync_request(const struct line_talk_sync *sync);

/* Returns whether to check again promptly for another page. */
gboolean line_talk_sync_apply(struct line_talk_sync *sync, J *response,
			      LineTalkOperation operation, gpointer data,
			      gboolean *reload);
