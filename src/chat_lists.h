/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <json-c/json.h>

struct chat_lists;

enum list_command {
	LIST_FOLDERS,
	LIST_PIN,
	LIST_SEARCH,
	LIST_FIND,
	LIST_REFRESH,
	LIST_COMMANDS
};

struct chat_lists *chat_lists_open(const char *account, GError **e);
void chat_lists_free(struct chat_lists *lists);
struct json_object *chat_folders(struct chat_lists *lists);
const char *chat_current_view(struct chat_lists *lists);
gboolean chat_view_set(struct chat_lists *lists, const char *view, GError **e);
gboolean chat_folder_set(struct chat_lists *lists, const char *old,
			 const char *name, const char *initial_chat,
			 GError **e);
gboolean chat_folder_toggle(struct chat_lists *lists, const char *name,
			    const char *chat, GError **e);
gboolean chat_folder_contains(struct chat_lists *lists, const char *name,
			      const char *chat);
gboolean chat_has_pins(struct chat_lists *lists);
gboolean chat_pinned(struct chat_lists *lists, const char *chat);
gboolean chat_pin_toggle(struct chat_lists *lists, const char *chat,
			 GError **e);
gboolean chat_notifications(struct chat_lists *lists);
gboolean chat_notifications_set(struct chat_lists *lists, gboolean on,
				GError **e);
/* Call and message sounds; on unless turned off. */
gboolean chat_sounds(struct chat_lists *lists);
gboolean chat_sounds_set(struct chat_lists *lists, gboolean on, GError **e);
/* Muted conversations never raise a desktop notification. */
gboolean chat_muted(struct chat_lists *lists, const char *chat);
gboolean chat_mute_toggle(struct chat_lists *lists, const char *chat,
			  GError **e);
gboolean chat_strict_muted(struct chat_lists *lists, const char *chat);
gboolean chat_strict_mute_toggle(struct chat_lists *lists, const char *chat,
				 GError **e);
/* Reduced motion: 1 always, 0 never, -1 follow the desktop. */
int chat_motion(struct chat_lists *lists);
gboolean chat_motion_set(struct chat_lists *lists, int reduce, GError **e);
const char *chat_binding(struct chat_lists *lists, enum list_command cmd);
gboolean chat_binding_set(struct chat_lists *lists, enum list_command cmd,
			  const char *chord, GError **e);
