/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "call_schema.h"
#include "line.h"

enum {
	CALL_GROUP_RELEASE_MULTIDEV_IN_USE = 6,
	CALL_GROUP_PARTICIPATE_REQ = 51,
	CALL_GROUP_PARTICIPATE_RSP = 52,
	CALL_GROUP_DATA_REQ = 16,
	CALL_GROUP_DATA_RSP = 17,
	CALL_GROUP_PUSH_REQ = 55,
	CALL_GROUP_PUSH_RSP = 56,
	CALL_GROUP_SUBPUSH_REQ = 67,
	CALL_GROUP_SUBPUSH_RSP = 68,
	CALL_GROUP_STRM_REQ = 55,
	CALL_GROUP_STRM_RSP = 56,
	CALL_GROUP_NOTIFY_REQ = 59,
	CALL_GROUP_NOTIFY_RSP = 60,
	CALL_GROUP_PARTICIPATE_ID = 0x214e,
	CALL_GROUP_DATA_ID = 0x3189,
	CALL_GROUP_PUSH_RSP_ID = 0x2250,
	CALL_GROUP_SUBPUSH_RSP_ID = 0x2258,
	CALL_GROUP_STRM_REQ_ID = 0x318d,
	CALL_GROUP_STRM_RSP_ID = 0x328d,
	CALL_GROUP_NOTIFY_RSP_ID = 0x328f,
};

struct call_group_route {
	char *token, *relay, *relay6, *i_zone, *r_zone, *orion, *mixer;
	guint16 relay_port, mixer_port;
	guint8 relay_key[33];
	J *members;
};

enum call_group_result {
	CALL_GROUP_INVALID,
	CALL_GROUP_PENDING,
	CALL_GROUP_REJECTED,
	CALL_GROUP_JOINED,
};

enum call_group_video_map {
	CALL_GROUP_VIDEO_VP8 = 2,
	CALL_GROUP_VIDEO_SVC_SW = 4,
	CALL_GROUP_VIDEO_SVC = 7,
};

struct call_group_answer {
	guint64 media_channel;
	guint keepalive;
	guint32 audio_rx, audio_tx, video_rx, video_tx, data_rx, data_tx;
	guint32 feedback_tx, feedback_rx;
	guint feedback_version;
	guint8 audio_payload, video_payload, data_payload;
	guint8 video_map, video_packet_version;
	gboolean video_vp8a;
	gboolean video_supported, data_supported;
	/* Views of the validated, authenticated PARTICIPATE response. */
	Slice description, bridge, secret;
};

enum {
	CALL_GROUP_FEEDBACK_STREAMS = 8,
	CALL_GROUP_FEEDBACK_WINDOW = 128,
};

struct call_group_feedback_stream {
	gint64 arrival_us[CALL_GROUP_FEEDBACK_WINDOW];
	gint64 touched_us;
	guint32 ssrc;
	guint16 first, count;
	gboolean reported;
};

struct call_group_feedback {
	struct call_group_feedback_stream stream[CALL_GROUP_FEEDBACK_STREAMS];
	guint32 ssrc;
	gint64 epoch_us, previous_ms;
	guint16 sequence;
	guint next_stream, interval_ms, active_streams;
};

struct call_group_member {
	char mid[34];
	guint32 ssrc, channel;
	gboolean active;
};

struct call_group_presence {
	gboolean full;
	guint32 version, channel;
	guint wire_members, reported_members, unmapped_cameras;
	GArray *members;
};

enum call_group_stream_state {
	CALL_GROUP_STREAM_STOP,
	CALL_GROUP_STREAM_PAUSE,
	CALL_GROUP_STREAM_RESUME,
};

struct call_group_stream {
	char mid[34];
	guint32 ssrc, channel;
	enum call_group_stream_state state;
};

gboolean call_group_id(const char *chat);
gboolean call_group_acquire(Line *session, const char *chat, gboolean video,
			    struct call_group_route *route, GError **error);
J *call_group_members(Line *session, const char *chat, GError **error);
void call_group_route_clear(struct call_group_route *route);
void call_group_participate(Pb *out, const CallInfo *call,
			    const struct call_group_route *route);
enum call_group_result call_group_answer(Slice reply,
					 struct call_group_answer *answer);
void call_group_register(Pb *out, const struct call_group_answer *answer,
			 gboolean camera_active);
void call_group_media_key(const char *kind, const guint8 secret[30],
			  guint8 out[30]);
void call_group_feedback_init(struct call_group_feedback *feedback,
			      const struct call_group_answer *answer);
/* Pass packet arrival on the monotonic clock after authenticating media. */
void call_group_feedback_receive(struct call_group_feedback *feedback,
				 guint32 ssrc, guint16 sequence,
				 guint32 timestamp, gint64 received_us);
void call_group_feedback_forget(struct call_group_feedback *feedback,
				guint32 ssrc);
gsize call_group_feedback_report(struct call_group_feedback *feedback,
				 gint64 now_us, guint8 *packet, gsize capacity);
/* Return the number of requested RTP sequences and their media SSRC. */
guint call_group_retransmit_request(Slice packet, guint32 *target);
/* Pass one complete FMT12 packet from the negotiated relay. */
gsize call_group_feedback_control(struct call_group_feedback *feedback,
				  Slice packet, gint64 received_us,
				  gint64 now_us, guint8 response[24],
				  gboolean *interval_changed);
/* Pass only validated envelopes. The caller owns returned arrays. */
gboolean call_group_presence(Slice body, guint choice,
			     struct call_group_presence *out);
void call_group_stream_request(Pb *out, const char *mid, guint32 ssrc,
			       guint32 channel, gboolean start,
			       guint32 sequence);
enum {
	CALL_GROUP_LAYER_UNCHANGED = -2,
	CALL_GROUP_LAYER_CLEAR = -1,
};
int call_group_camera_resolution(Slice request, guint32 local_ssrc,
				 gboolean vp8a, guint *requested);
GArray *call_group_stream_updates(Slice body, guint choice);
