/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"
void ui_find_open(void);
void ui_find_open_all(void);
void ui_find_resume_input(void);
void ui_find_reset(void);
gboolean ui_find_active(void);
gboolean ui_find_event(SDL_Event *e);
gboolean ui_find_reply(const char *method, guint tag, Json *result,
		       const char *error);
void ui_find_draw(void);
PangoLayout *ui_result_layout(const char *text, int width);
void ui_result_paint(PangoLayout *layout, int x, int y, Rect clip);
