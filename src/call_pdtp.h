/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * LINE's Planet data transport (PDTP) for the "D" session. A screen share
 * is DATA media with service type PRESENTATION carried on a PDTP stream.
 *
 * A PDTP packet is the RTP payload of an SRTP packet. It starts with the
 * high bytes of a QUIC-style varint packet number whose low 16 bits are the
 * RTP sequence number, a NUL-terminated service name and a big-endian
 * bitmap of element types (bit N-1 for type N). Element groups follow in
 * descending type order as a varint length, a varint count for the stream
 * and window types, then the elements themselves.
 */
enum call_pdtp_element {
	PDTP_STREAM_DATA = 1,
	PDTP_STREAM_CLOSE = 2,
	PDTP_STREAM_CTRL = 3,
	PDTP_STREAM_OPEN = 4,
	PDTP_STREAM_RESET = 5,
	PDTP_INCLUDE_RETRANS = 9,
	PDTP_SET_MAX_ACK_DELAY = 10,
	PDTP_SWITCH_ACK_ALL = 11,
	PDTP_REQUEST_WINDOW = 12,
	PDTP_WINDOW_UPDATE = 13,
	PDTP_ACK = 15,
	/* Events for outgoing streams, beside the element types above. */
	PDTP_EVENT_PAUSE = 100,
	PDTP_EVENT_RESUME = 101,
};

enum {
	PDTP_PRESENTATION_CONTROL = 3,
	PDTP_PRESENTATION_VIDEO = 4,
};

struct call_pdtp;

typedef gboolean (*call_pdtp_send_fn)(const guint8 *packet, gsize size,
				      gpointer data);
/*
 * In-order bytes of a peer stream, starting at stream offset 'offset'.
 * 'time' is the chunk's source time; all chunks of one frame share it.
 * 'flags' is the first STRM_DATA flag byte: PDTP_MESSAGE_START marks the
 * first chunk of a message (one EVS3 fragment) and PDTP_MESSAGE_END its
 * last.
 */
enum {
	PDTP_MESSAGE_END = 0x04,
	PDTP_MESSAGE_START = 0x08,
};
typedef void (*call_pdtp_stream_fn)(guint64 stream, const char *service,
				    guint64 offset, guint64 time, guint flags,
				    const guint8 *bytes, gsize size,
				    gpointer data);
typedef void (*call_pdtp_event_fn)(guint64 stream, const char *service,
				   int element, gpointer data);

struct call_pdtp *call_pdtp_open(const guint8 tx[30], const guint8 rx[30],
				 guint8 pt, guint32 tx_ssrc, guint32 rx_ssrc,
				 GError **error);
/* Conference DATA receives presentations and negotiates ACK timing. */
struct call_pdtp *call_pdtp_open_group(const guint8 tx[30], const guint8 rx[30],
				       guint8 pt, guint32 tx_ssrc,
				       guint32 rx_ssrc, GError **error);
/* The call worker validates presenter source IDs against conference presence. */
typedef gboolean (*call_pdtp_member_fn)(guint32 source, gpointer data);
void call_pdtp_set_group_members(struct call_pdtp *pdtp,
				 call_pdtp_member_fn check);
gboolean call_pdtp_group_presenter_left(struct call_pdtp *pdtp);
/* Select or stop the source of the current in-order presentation command. */
gboolean call_pdtp_watch_presentation(struct call_pdtp *pdtp, gboolean watch);
gboolean call_pdtp_group_setup_acknowledged(const struct call_pdtp *pdtp);
void call_pdtp_close(struct call_pdtp *pdtp);
void call_pdtp_set_trace(struct call_pdtp *pdtp, gboolean trace);
void call_pdtp_set_handlers(struct call_pdtp *pdtp, call_pdtp_stream_fn data,
			    call_pdtp_event_fn event, gpointer user);
gboolean call_pdtp_matches(const struct call_pdtp *pdtp, const guint8 *packet,
			   gsize size);
/* Authenticates one SRTP packet and processes its PDTP elements. */
gboolean call_pdtp_receive(struct call_pdtp *pdtp, const guint8 *packet,
			   gsize size, gint64 now_us);
/* Sends due ACKs, window updates and retransmissions; returns next due time. */
gint64 call_pdtp_poll(struct call_pdtp *pdtp, gint64 now_us,
		      call_pdtp_send_fn send, gpointer data);

/* Outgoing streams. Data is queued and sent reliably within the window. */
gboolean call_pdtp_stream_open(struct call_pdtp *pdtp, guint64 stream,
			       const char *service, guint flags);
gboolean call_pdtp_stream_write(struct call_pdtp *pdtp, guint64 stream,
				const guint8 *bytes, gsize size);
/* Queues one message; 'time' (0 for none) tags every chunk of it. */
gboolean call_pdtp_stream_send(struct call_pdtp *pdtp, guint64 stream,
			       const guint8 *bytes, gsize size, guint64 time);
void call_pdtp_stream_close(struct call_pdtp *pdtp, guint64 stream);
/* Out-of-order segments waiting for a gap in a peer stream to fill. */
guint call_pdtp_stream_waiting(const struct call_pdtp *pdtp, guint64 stream);
/* Whether the receiver paused an outgoing stream. */
gboolean call_pdtp_stream_paused(const struct call_pdtp *pdtp, guint64 stream);
/* Bytes queued or in flight on an outgoing stream. */
gsize call_pdtp_stream_backlog(const struct call_pdtp *pdtp, guint64 stream);

/* Prints send counters and stream positions for LINE_CALL_DEBUG. */
void call_pdtp_report(const struct call_pdtp *pdtp);

/* Explicit raw capture into an owner-private diagnostic directory. */
void call_pdtp_dump_stream(const char *dir, const char *service, guint64 stream,
			   const guint8 *bytes, gsize size);

/* Packet codec, exposed for tests. */
gsize call_pdtp_varint(guint8 *out, guint64 value);
gboolean call_pdtp_read_varint(const guint8 **p, const guint8 *end,
			       guint64 *value);
