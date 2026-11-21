/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL.h>
#include <glib.h>
#include <json-c/json.h>

/*
 * Open a link from an official-account message as LINE does: LIFF apps
 * get a view from LINE and open in the in-app browser, other https pages
 * open there too. Without the in-app browser (avernella-liff) installed,
 * links go to the system browser.
 */
void ui_liff_open_link(const char *url);
/* Replies to "liffView"; FALSE for other actions. */
gboolean ui_liff_reply(const char *action, guint tag,
		       struct json_object *result, const char *error);
/* Requests from the in-app browser; FALSE for other events. */
gboolean ui_liff_event(const SDL_Event *e);
/* Closes the in-app browser, for sign-out and exit. */
void ui_liff_close(void);
