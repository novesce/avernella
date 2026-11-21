/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

#define LINE_STATE_DIR "avernella"

gboolean line_store_has_session(void);
int line_store_open(Line *s, GError **error);
gboolean line_store_save(int dir, Line *s, GError **error);
gint32 line_store_sequence(int dir, Line *s, GError **error);
gboolean line_store_forget(int dir, GError **error);
void line_session_clear(Line *s);
