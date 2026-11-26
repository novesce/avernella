/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/*
 * LINE Front-end Framework (LIFF) apps: web pages that run inside LINE with
 * the user's LINE identity. LINE recognizes them by URL, asks LiffService
 * for a view and hands the page its tokens in the URL fragment.
 */

/*
 * Whether url names a LIFF app: https://liff.line.me/<liffId>...,
 * https://miniapp.line.me/<liffId>..., https://line.me/R/app/<liffId>...
 * or line://app/<liffId>.... When it does, *liff_id and *rest (the path,
 * query and fragment after the ID, possibly "") are newly allocated.
 * Either output may be NULL.
 */
gboolean line_liff_url(const char *url, char **liff_id, char **rest);

/* LINE's own URL schemes, which only LINE can open. */
gboolean line_liff_line_scheme(const char *url);

/* Parse OpenChat (/ti/g2/) or group (/ti/g/) invite links and tickets. */
char *line_square_parse_ticket(const char *input);
char *line_group_parse_ticket(const char *input);

/* The in-app browser's user agent marks; liff.isInClient() matches them. */
#define LINE_LIFF_UA_VERSION "9.7.0"
#define LINE_LIFF_USER_AGENT                                    \
	"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 " \
	"(KHTML, like Gecko) Line/" LINE_LIFF_UA_VERSION " LIFF"
#define LINE_WEB_USER_AGENT                                     \
	"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 " \
	"(KHTML, like Gecko) Line/" LINE_LIFF_UA_VERSION

/*
 * The in-app browser speaks to the UI over a SOCK_SEQPACKET socket, one
 * datagram per message, fields separated by tabs:
 *   UI -> browser: "L\t<size>\t<url>" LIFF page, "W\tweb\t<url>" web page,
 *                  "R\t<dialog>\t<1|0>" answer to a script dialog.
 *   browser -> UI: "O\t<url>" a LINE or LIFF link the page followed,
 *                  "D\t<dialog>\t<a|c>\t<text>" alert or confirm.
 */
/*
 * The in-app browser's storage directory under base (the user's data or
 * cache directory): base/avernella/liff. When it doesn't exist yet, an
 * earlier client's directory is moved there, newest first (kitatide, then
 * line-linux), so LINE apps keep their cookies and cache; otherwise it is
 * created. An existing Avernella directory is never replaced. Returns a
 * newly allocated path to a real directory that belongs to this user and
 * is private to them (0700, tightened if needed), inside base/avernella,
 * which no other user can write to. Returns NULL when that cannot be
 * guaranteed, such as for a symlink or another user's directory; the
 * browser then keeps nothing on disk.
 */
char *line_liff_storage_dir(const char *base);

#define LINE_LIFF_FD 3
#define LINE_LIFF_PACKET 16384

/*
 * LINE_DEBUG_LIFF test pages: http://127.0.0.1:<port> with a port from
 * 1024 to 65535 and nothing else in the authority (no user info).
 */
gboolean line_liff_debug_url(const char *url);

/*
 * Whether the in-app browser may load url itself: https, or the about:,
 * data: and blob: documents that pages create without the network. Plain
 * http is never loaded, except debug test pages when debug is set; the
 * browser offers other links to the desktop instead.
 */
gboolean line_liff_web_url(const char *url, gboolean debug);
