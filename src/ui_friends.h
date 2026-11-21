/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"
void ui_friends_reset(void);
void ui_friends_show(void);
/* Opens a roster: 0 everyone, 1 hidden, 2 blocked, 3 suggestions. */
void ui_friends_show_view(int directory);
gboolean ui_friends_active(void);
void ui_friends_resume_input(void);
gboolean ui_friends_event(SDL_Event *event);
gboolean ui_friends_reply(const char *method, guint tag, Json *result,
			  const char *error);
void ui_friends_draw(void);
void ui_friends_scan_result(const char *value, const char *error);
void ui_friend_pick_qr(void);
void ui_friend_scan_file(const char *path);
void ui_open_conversation(Json *row);
void ui_update_friend_chat(Json *row);
void ui_draw_profile(Json *row, Rect area);
void ui_draw_code(QRcode *qr, int cx, int top, int bound);
void ui_draw_edit(Editor *edit, Rect area, gboolean focus,
		  const char *placeholder);
int ui_edit_hit(Editor *edit, Rect area, int x);
