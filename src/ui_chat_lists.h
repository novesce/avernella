/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"
#include "chat_lists.h"

void ui_lists_login(const char *account);
void ui_lists_reset(void);
void ui_lists_rebuild(void);
void ui_lists_reveal(const char *chat);
gboolean ui_list_pinned(const char *chat);
const char *ui_folder_title(void);
gboolean ui_lists_ready(void);
gboolean ui_lists_active(void);
gboolean ui_lists_event(SDL_Event *e);
void ui_lists_draw(void);
void ui_lists_resume_input(void);
void ui_lists_open(void);
int ui_list_shortcut(SDL_KeyboardEvent key);
gboolean ui_notifications_enabled(void);
gboolean ui_notifications_set(gboolean on);
gboolean ui_sounds_enabled(void);
gboolean ui_sounds_set(gboolean on);
gboolean ui_chat_muted(Json *row);
gboolean ui_chat_strict_muted(const char *chat);
gint64 ui_chat_unread(Json *row);
/* Saves the reduced-motion choice: 1, 0, or -1 for the desktop's. */
gboolean ui_motion_save(int reduce);
void ui_lists_open_preferences(void);
void ui_refilter_chats(void);
