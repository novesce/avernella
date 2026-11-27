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
	guint8 screen; /* The selected remote group video is a virtual display. */
	char chat[34];
};

gboolean line_media_send_frame(int socket,
			       const struct line_preview_frame *frame,
			       const struct line_camera_frame *image);
int line_media_recv_frame(int socket, struct line_preview_frame *frame);

GHashTable *line_leases_new(void);
J *line_media_lease(GHashTable *leases, GBytes *bytes, GError **error);
/*
 * A read-only descriptor for a lease, sent to the UI with the reply
 * (SCM_RIGHTS). The worker is not dumpable, so the UI cannot open the
 * worker's /proc/<pid>/fd itself. -1 for an unknown lease.
 */
int line_media_lease_open(GHashTable *leases, const char *lease);

/*
 * The worker's JSON lines to the UI. line_wire_send() writes one line and,
 * with fd >= 0, passes the descriptor with its first byte (SCM_RIGHTS). A
 * stream read ends just after a segment that carries descriptors, so
 * line_wire_read() gives each descriptor to the line it came with.
 */
gboolean line_wire_send(int socket, const char *text, gsize length, int fd);

struct line_wire_reader {
	GString *input;
	int pending_fd; /* for a lease line that continues in the next read */
};

void line_wire_reader_init(struct line_wire_reader *r);
void line_wire_reader_clear(struct line_wire_reader *r);
/*
 * One read into buf. Calls line() for each complete line with the line
 * (owned by the callee) and its descriptor or -1 (also owned by the
 * callee). Returns what recvmsg() returned.
 */
ssize_t line_wire_read(struct line_wire_reader *r, int socket, char *buf,
		       gsize size,
		       void (*line)(char *text, int fd, gpointer data),
		       gpointer data);
gboolean line_save_media(const char *path, GBytes *bytes, gboolean replace,
			 GError **error);
/*
 * Single-slot read-only FD staged from UI -> worker over SCM_RIGHTS for the
 * currently executing upload RPC so the sandboxed worker never open()s $HOME.
 */
void line_media_stage_fd(int fd);
int line_media_take_staged_fd(void);
void line_media_clear_staged_fd(void);
