/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include "liff_url.h"

/*
 * Ask LINE for a view of params.url, optionally from a worker-known chat.
 * Returns {"url": page to load, "size": "full"|"tall"|"compact",
 * "title": app name} where url carries the tokens liff.init() reads, or,
 * when LINE requires a consent or fallback page, "web": true.
 */
J *line_liff_view(Line *s, GHashTable *chats, J *params, GError **error);

/*
 * The page LINE's client loads for a view: the endpoint with the LIFF
 * URL's extra path and query appended, and the view's tokens in the
 * fragment. NULL unless endpoint is https.
 */
char *line_liff_launch_url(const char *endpoint, const char *rest, J *response);
