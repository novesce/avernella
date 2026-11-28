/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include <sys/stat.h>

#define LINE_STATE_DIR "avernella"
#define LINE_STATE_SESSION_FILE "session.json"

gboolean line_store_start(const guint8 *key);
gboolean line_store_offline_key(const char *mid, guint8 *key);
void line_store_stop(void);
gboolean line_store_temporary(void);
int line_store_session_fd(int dir);
gboolean line_store_is_memory_file(int fd, const struct stat *st);

gboolean line_store_has_session(void);
int line_store_open(Line *s, GError **error);
/* Reads the record into an empty session under the caller's directory lock. */
gboolean line_store_load(int dir, Line *s, GError **error);
gboolean line_store_save(int dir, Line *s, GError **error);
gint32 line_store_sequence(int dir, Line *s, GError **error);
gboolean line_store_forget(int dir, GError **error);
/*
 * Removes the saved session without reading it, under the same lock, for
 * signing out when the session file cannot be read. TRUE if none exists.
 * If requested, return the still-locked directory for further cleanup.
 */
gboolean line_store_discard(int *dir_out, GError **error);
J *line_store_read_unsent(int dir, const char *mid);
void line_store_write_unsent(const char *mid, J *rows);
void line_session_clear(Line *s);
