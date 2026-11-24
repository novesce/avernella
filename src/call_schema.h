/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "call_wire.h"

typedef struct {
	char *local, *target, *token, *i_zone, *r_zone, *stid, *cid;
	guint64 local_channel, local_media, remote_channel, remote_media, nonce,
		remote_nonce;
	guint32 transaction, media_id;
	guint8 session[16], pub[33], media_nonce[16], media_secret[30],
		device[32];
	guint8 audio_payload, video_payload, data_payload;
	gboolean have_session, video, video_capable;
} CallInfo;

void call_offer(Pb *out, const CallInfo *s);
void call_answer_offer(Pb *out, const CallInfo *s);
void call_verify(Pb *out, const CallInfo *s);
void call_setup(Pb *out, const CallInfo *s);
void call_user_agent(Pb *out);
gboolean call_envelope(Pb *out, CallInfo *s, guint plane, guint oneof,
		       guint msg_id, const Pb *body, Slice reply_header);
void call_stream_spec(Pb *out, guint32 audio_rx, guint32 audio_tx,
		      guint32 video_rx, guint32 video_tx, guint32 data_rx,
		      guint32 data_tx, gboolean camera_enabled);

/* The supported Planet video profile is EVS3 VP8 with the base descriptor. */
gboolean call_video_offer_profile(Slice offer, guint8 *payload,
				  guint32 *local_ssrc, guint32 *remote_ssrc);
gboolean call_video_profile(Slice answer, guint8 *payload, guint32 *local_ssrc,
			    guint32 *remote_ssrc);

/* RTP payload type and SSRCs of a bidirectional session such as 'D'. */
gboolean call_session_rtp(Slice description, char label, guint8 *payload,
			  guint32 *local_ssrc, guint32 *remote_ssrc);

/* The RTP payload type a description gives session 'label', or 0. */
guint8 call_session_payload(Slice description, char label);

/* Receive limits a peer advertises for its incoming video; 0 if absent. */
struct call_video_limits {
	guint kbps, fps, resolution;
};
void call_video_peer_limits(Slice description,
			    struct call_video_limits *limits);
void call_video_describe(Slice description, const char *role);

/* What this client asks to receive: HD/FHD (resolution class 3), 30 fps. */
enum {
	CALL_VIDEO_RECEIVE_KBPS = 4000,
	CALL_VIDEO_RECEIVE_FPS = 30,
	CALL_VIDEO_RECEIVE_RESOLUTION = 3
};
