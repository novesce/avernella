/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/* Carry the worker's cooldown across sign-in attempts in the window. */
static inline void line_reply_retry(J *reply, const GError *error,
				    gboolean login)
{
	if (!error)
		return;
	if (g_error_matches(error, LINE_ERROR, LINE_RETRY))
		json_object_object_add(reply, "retry",
				       json_object_new_boolean(TRUE));
	if (error->domain == LINE_ERROR)
		json_object_object_add(reply, "code",
				       json_object_new_int(error->code));
	if (login || g_error_matches(error, LINE_ERROR, LINE_RATE_LIMITED)) {
		int wait = line_transport_retry_after();

		if (wait)
			json_object_object_add(reply, "retryAfter",
					       json_object_new_int(wait));
	}
}

static inline gint64 line_login_wait_us(J *reply, guint *rejected)
{
	static const int pause[] = { 0, 0, 0, 10, 30, 120 };
	gint64 seconds = json_object_get_int(field(reply, "retryAfter"));

	if (json_object_get_int(field(reply, "code")) ==
	    LINE_CREDENTIALS_REJECTED) {
		*rejected = MIN(*rejected + 1, G_N_ELEMENTS(pause) - 1);
		seconds = MAX(seconds, pause[*rejected]);
	}
	return MAX(seconds, 0) * G_USEC_PER_SEC;
}
