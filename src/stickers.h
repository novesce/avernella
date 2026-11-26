/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/* Owned by one signed-in worker. Catalog/entitlement operations are serialized. */
typedef struct line_stickers LineStickers;
LineStickers *line_stickers_new(void);
void line_stickers_free(LineStickers *stickers);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(LineStickers, line_stickers_free)

J *line_sticker_catalog(Line *s, LineStickers *stickers, guint offset,
			guint limit, const char *query, GError **error);
char *line_sticker_asset(LineStickers *stickers, const char *package_id,
			 const char *sticker_id, GError **error);
char *line_sticker_message_asset(J *message, GError **error);
char *line_sticker_message_sound(J *message, GError **error);
/* May run in a download worker with an owned URL returned by either asset helper. */
GBytes *line_sticker_fetch(const char *trusted_url, GError **error);
GBytes *line_sticker_sound_fetch(const char *trusted_url, GError **error);
J *line_sticker_metadata(Line *s, LineStickers *stickers,
			 const char *package_id, const char *sticker_id,
			 GError **error);
J *line_send_sticker(Line *s, LineStickers *stickers, const char *chat,
		     const char *package_id, const char *sticker_id,
		     const char *reply_to, gint32 reserved_seq, GError **error);
