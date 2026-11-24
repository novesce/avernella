/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
struct line_camera_frame;

/* Group receive progress sent once per transition over the worker IPC. */
enum line_media_reception {
	LINE_MEDIA_WAITING,
	LINE_MEDIA_RECEIVING,
	LINE_MEDIA_PLAYABLE,
};

enum line_preview_source {
	LINE_PREVIEW_SCREEN,
	LINE_PREVIEW_REMOTE,
	LINE_PREVIEW_SELF,
	LINE_PREVIEW_COUNT
};

/* Dedicated datagram path between the call worker and the window. */
struct line_preview_frame {
	gint64 decoded_at;
	guint32 width, height, rotation;
	guint8 source, camera_enabled;
	char chat[34];
};

gboolean line_media_send_frame(int socket,
			       const struct line_preview_frame *frame,
			       const struct line_camera_frame *image);
int line_media_recv_frame(int socket, struct line_preview_frame *frame);

GHashTable *line_leases_new(void);
J *line_media_lease(GHashTable *leases, GBytes *bytes, GError **error);
gboolean line_save_media(const char *path, GBytes *bytes, gboolean replace,
			 GError **error);
