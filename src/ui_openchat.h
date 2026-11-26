/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

void ui_openchat_open_hub(void);
void ui_openchat_open_info(void);
void ui_openchat_open_link(const char *url);
void ui_openchat_open_group_link(const char *url);
void ui_openchat_avatar_picked(const char *path);

gboolean ui_openchat_active(void);
void ui_openchat_reset(void);
void ui_openchat_resume_input(void);
gboolean ui_openchat_event(SDL_Event *event);
gboolean ui_openchat_reply(const char *method, guint tag, Json *result,
			   const char *error);
void ui_openchat_draw(void);
