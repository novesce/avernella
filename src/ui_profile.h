/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ui_private.h"

/*
 * Discord-style people and account surfaces: the profile popout opened
 * from an avatar or name, the full-window User Settings, the Inbox of
 * friend requests and adds, and the toast for live friend activity.
 */
void ui_profile_open(Json *person, const char *chat, Rect anchor);
void ui_settings_open(void);
void ui_inbox_open(void);
gboolean ui_profile_active(void);
int ui_profile_layer(void);
/* Consumes input while a surface is open; FALSE otherwise. */
gboolean ui_profile_event(SDL_Event *event);
void ui_profile_draw(void);
gboolean ui_profile_reply(const char *method, guint tag, Json *result,
			  const char *error);
/* Worker events: self, contact, friend_added, friend_request. */
void ui_profile_packet(Json *packet);
void ui_profile_login(void);
void ui_profile_reset(void);
void ui_profile_picture_chosen(const char *path);

/* User panel: the name block opens your popout; inbox and settings. */
Rect ui_profile_self_area(Rect panel);
const char *ui_profile_self_avatar(void);
const char *ui_profile_self_status(void);
void ui_profile_panel_draw(void);
gboolean ui_profile_panel_click(int x, int y);
int ui_profile_panel_hover(int x, int y);
/* Unseen friend requests and adds, for the Friends badge. */
int ui_profile_badge(void);
gboolean ui_profile_toast_visible(void);
void ui_profile_toast_draw(void);
