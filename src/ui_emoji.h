/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

void ui_emoji_reset(void);
void ui_emoji_forget(const char *chat);
void ui_emoji_edit(int left, int right, int inserted);
char *ui_emoji_export(const char *chat, const char *text, int left, int right,
		      Json **spans);
gboolean ui_emoji_same(const char *chat, const char *text, Json *params);
gboolean ui_emoji_has_tokens(void);
void ui_emoji_shape_history(PangoLayout *layout, const char *body,
			    const char *prefix, Json *spans);
void ui_emoji_shape_editor(PangoLayout *layout, int left, int right,
			   int inserted);
void ui_emoji_open(void);
void ui_emoji_catalog(guint tag, Json *result, const char *error);
void ui_palette_closed(void);
void ui_sticker_sent(guint tag, const char *error);
void ui_emoji_draw(void);
gboolean ui_emoji_input(SDL_Event *event);
void ui_emoji_pick_close(void);
cairo_surface_t *ui_emoji_image(Json *meta, gboolean load, gboolean *failed);

void ui_emoji_frame_start(gint64 now, gboolean full);
gint64 ui_emoji_deadline(void);
