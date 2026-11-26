/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

void ui_groups_new(void);
void ui_groups_members(void);
void ui_groups_reset(void);
gboolean ui_groups_active(void);
gboolean ui_groups_event(SDL_Event *event);
gboolean ui_groups_reply(const char *method, guint tag, Json *result,
			 const char *error);
void ui_groups_overlay(void);
void ui_groups_resume_input(void);
void ui_groups_avatar_picked(const char *path);

/* Remove the departed group from the active UI session. */
void ui_group_left(const char *id);
