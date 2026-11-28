/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

void ui_chat_tools_open(void);
void ui_chat_tools_forward(Json *row);
void ui_chat_tools_announce(Json *row);
void ui_chat_tools_reset(void);
void ui_chat_tools_resume_input(void);
gboolean ui_chat_tools_active(void);
gboolean ui_chat_tools_event(SDL_Event *event);
gboolean ui_chat_tools_reply(const char *method, guint tag, Json *result,
			     const char *error);
void ui_chat_tools_draw(void);

void ui_chat_announcements_select(const char *chat);
void ui_chat_announcements_refresh(const char *chat);
int ui_chat_announcements_height(void);
void ui_chat_announcements_place(Rect region);
void ui_chat_announcements_draw(void);
gboolean ui_chat_announcements_click(int x, int y);
