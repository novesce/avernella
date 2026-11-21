/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * Worker validates every selected MID against its session friend roster. Each
 * mutation consumes one request sequence immediately before its sole write.
 */
J *line_group_create(Line *s, const char *name, J *users, GError **error);
J *line_group_invite(Line *s, const char *chat, J *users, GError **error);
J *line_group_members(Line *s, const char *chat, GError **error);

/* Worker permits responses only for group IDs in the session's invitation list. */
J *line_group_invitations(Line *s, GError **error);
J *line_group_respond(Line *s, const char *chat, gboolean accept,
		      GError **error);

/* The worker persists the sequence before each explicit mutation. */
J *line_group_rename(Line *s, const char *chat, const char *name, gint32 seq,
		     GError **error);
J *line_group_remove(Line *s, const char *chat, const char *user,
		     gboolean pending, gint32 seq, GError **error);
J *line_group_leave(Line *s, const char *chat, gint32 seq, GError **error);
J *line_group_link(Line *s, const char *chat, GError **error);
J *line_group_set_link(Line *s, const char *chat, gboolean enabled, gint32 seq,
		       GError **error);
J *line_group_renew_link(Line *s, const char *chat, gint32 seq, GError **error);
