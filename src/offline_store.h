/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include "state_crypto.h"

typedef struct line_offline LineOffline;

/* The caller retains the exclusive state directory lock while using this. */
LineOffline *line_offline_open(int state_dir, const char *mid,
			       const guint8 key[LINE_STATE_KEY_SIZE]);
void line_offline_close(LineOffline *store);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(LineOffline, line_offline_close)

J *line_offline_load_profile(LineOffline *store);
gboolean line_offline_save_profile(LineOffline *store, J *profile);
J *line_offline_load_chats(LineOffline *store);
gboolean line_offline_save_chats(LineOffline *store, J *rows);

/* Outputs and return values are owned. A missing snapshot returns NULL. */
J *line_offline_load_history(LineOffline *store, const char *chat,
			     const char *before, J **raw, gboolean *more);
gboolean line_offline_save_history(LineOffline *store, const char *chat,
				   J *rows, J *raw);
void line_offline_drop_history(LineOffline *store, const char *chat);
void line_offline_unsend(LineOffline *store, const char *chat, const char *id,
			 gint64 time);

/* A missing journal is empty. An unreadable journal is an error. */
J *line_offline_load_outbox(LineOffline *store, GError **error);
gboolean line_offline_save_outbox(LineOffline *store, J *rows, GError **error);
gboolean line_offline_clear_all(int state_dir, GError **error);
