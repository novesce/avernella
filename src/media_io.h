/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
struct line_camera_frame;

/* Dedicated datagram path between the call worker and the window. */
struct line_share_frame {
	gint64 decoded_at;
	guint32 width, height, rotation;
	char chat[34];
};

gboolean line_media_send_share(int socket, const struct line_share_frame *frame,
			       const struct line_camera_frame *image);
int line_media_recv_share(int socket, struct line_share_frame *frame);

GHashTable *line_leases_new(void);
J *line_media_lease(GHashTable *leases, GBytes *bytes, GError **error);
gboolean line_save_media(const char *path, GBytes *bytes, gboolean replace,
			 GError **error);
