/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include "liff_url.h"

/*
 * Ask LINE for a LIFF view of url, from chat (a chat MID, or NULL).
 * Returns {"url": page to load, "size": "full"|"tall"|"compact",
 * "title": app name} where url carries the tokens liff.init() reads, or,
 * when LINE first needs the user's consent, the consent page.
 */
J *line_liff_view(Line *s, const char *url, const char *chat, GError **error);

/*
 * The page LINE's client loads for a view: the endpoint with the LIFF
 * URL's extra path and query appended, and the view's tokens in the
 * fragment. NULL unless endpoint is https.
 */
char *line_liff_launch_url(const char *endpoint, const char *rest, J *response);
