/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/*
 * Invoke from the UI only after the user chooses camera recording. The callback
 * runs on a capture thread and owns both strings. A successful path remains
 * private until the UI sends it. Dispose with line_record_discard.
 */
typedef void (*LineRecordDone)(char *path, char *error, gpointer data);
gboolean line_record_start(LineRecordDone finished, gpointer data,
			   GError **error);
void line_record_stop(void);
void line_record_cancel(void);
void line_record_discard(char *path);
void line_record_shutdown(void);

#ifdef LINE_RECORD_TEST
gboolean line_record_start_synthetic(LineRecordDone finished, gpointer data,
				     GError **error);
#endif
