/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

void ui_shared_open(void);
void ui_shared_reset(void);
void ui_shared_resume_input(void);
gboolean ui_shared_active(void);
gboolean ui_shared_event(SDL_Event *event);
gboolean ui_shared_reply(const char *method, guint tag, Json *result,
			 const char *error);
void ui_shared_draw(void);
void ui_media_album_photo(const char *album, const char *id);
