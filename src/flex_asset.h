/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/* Only a guard against a server that never stops sending. */
#define LINE_FLEX_ASSET_LIMIT (64u * 1024u * 1024u)

gboolean line_flex_link_allowed(const char *url);
/* Any public HTTPS host, as LINE's phone app loads a sender's art. */
gboolean line_flex_image_allowed(const char *url);
/*
 * JPEG, PNG or WebP over HTTPS. Requests are anonymous and connect only
 * to public addresses, never the local network. Redirects are followed
 * to other HTTPS URLs; an imagemap <base>/<width> that is rejected is
 * retried at <base>. LINE_DEBUG_MEDIA=1 reports failures on stderr.
 */
GBytes *line_flex_image_download(const char *url, GError **error);
/*
 * An official account's image message keeps its picture at its own HTTPS
 * URLs instead of LINE's object storage: DOWNLOAD_URL for the image and
 * PREVIEW_URL for a smaller copy, as LINE's phone app reads them. Returns
 * the first allowed one, or NULL for an ordinary photo.
 */
const char *line_hosted_image_url(J *message);
