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
