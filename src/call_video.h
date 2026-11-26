/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "camera.h"

struct call_video;

enum call_video_rtp_extension {
	CALL_VIDEO_RTP_EXT_NONE,
	CALL_VIDEO_RTP_EXT_VFD,
	CALL_VIDEO_RTP_EXT_VED,
};

/* Callbacks run synchronously on the call thread. Buffers are borrowed. */
typedef gboolean (*call_video_send_fn)(const guint8 *packet, gsize size,
				       gpointer data);
typedef void (*call_video_display_fn)(const struct line_camera_frame *frame,
				      gpointer data);

struct call_video *call_video_open(const guint8 tx[30], const guint8 rx[30],
				   guint8 pt, guint32 tx_ssrc, guint32 rx_ssrc,
				   guint kbps, guint fps, GError **error);
/* In a conference the camera and each subscription have their own stream. */
struct call_video *call_video_open_tx(const guint8 key[30], guint8 pt,
				      guint32 ssrc, guint kbps, guint fps,
				      GError **error);
struct call_video *call_video_open_group_tx(const guint8 key[30], guint8 pt,
					    guint32 ssrc, guint kbps, guint fps,
					    GError **error);
void call_video_set_group_format(struct call_video *video,
				 enum call_video_rtp_extension extension,
				 gboolean vp8a);
enum call_video_rtp_extension
call_video_group_rtp_extension(const struct call_video *video);
/* Limit the group uplink to the requested resolution class, or -1 to clear. */
void call_video_set_group_resolution(struct call_video *video, int resolution);
struct call_video *call_video_open_rx(const guint8 key[30], guint8 pt,
				      guint32 ssrc, GError **error);
struct call_video *call_video_open_group_rx(const guint8 key[30], guint8 pt,
					    guint32 ssrc, GError **error);
gboolean call_video_encode(struct call_video *video,
			   const struct line_camera_frame *frame, gint64 now_us,
			   call_video_send_fn send, gpointer data,
			   GError **error);
/*
 * Moves encoding of this sending stream to its own thread. Afterwards
 * call_video_encode() copies the frame and returns at once, skipping a
 * frame while the previous one is still encoding, and the picture is
 * packetized and sent by call_video_encoded() on the call thread once
 * call_video_encoder_fd() is readable. Returns FALSE (and keeps encoding
 * inline) when no thread could start.
 */
gboolean call_video_encode_async(struct call_video *video);
int call_video_encoder_fd(const struct call_video *video);
gboolean call_video_encoded(struct call_video *video, gint64 now_us,
			    call_video_send_fn send, gpointer data);
gboolean call_video_receive(struct call_video *video, const guint8 *packet,
			    gsize size, call_video_display_fn display,
			    gpointer data);
/*
 * The same with the time the kernel received the packet (see
 * line_udp_rx_arrival()), so reordering holds and the REMB delay estimate
 * measure the network rather than the call thread. Zero means now.
 */
gboolean call_video_receive_at(struct call_video *video, const guint8 *packet,
			       gsize size, gint64 arrival,
			       call_video_display_fn display, gpointer data);
/* Source of the decoded group picture, read during its display callback. */
gboolean call_video_frame_is_screen(const struct call_video *video);
gboolean call_video_needs_keyframe(const struct call_video *video);
guint call_video_authenticated_packets(const struct call_video *video);
void call_video_report(const struct call_video *video, gboolean group);
gboolean call_video_send_feedback(struct call_video *video,
				  call_video_send_fn send, gpointer data);
/* Reuses the sending stream's SRTCP state across conference subscriptions. */
gboolean call_video_send_peer_feedback(struct call_video *tx,
				       const struct call_video *rx,
				       call_video_send_fn send, gpointer data);
gboolean call_video_receive_feedback(struct call_video *video,
				     const guint8 *packet, gsize size);
/*
 * Handles one-to-one SRTCP from the peer: keyframe requests (spaced
 * 300 ms apart), retransmission requests (LINE's RTPFB format 5 and
 * RFC 4585 NACK), REMB and receiver-report loss for rate control.
 */
gboolean call_video_receive_rtcp(struct call_video *video, const guint8 *packet,
				 gsize size, gint64 now_us,
				 call_video_send_fn send, gpointer data);
/*
 * Receiver loss recovery. Releases packets the reorder buffer held,
 * requests missing ones (one-to-one only), asks for a keyframe when
 * keyframes is TRUE and decoding needs one, and sends REMB. feedback is
 * the stream whose SRTCP state signs the requests. Returns the next time
 * to call it.
 */
gint64 call_video_poll(struct call_video *rx, struct call_video *feedback,
		       gint64 now_us, gboolean keyframes,
		       call_video_send_fn send, gpointer data,
		       call_video_display_fn display, gpointer display_data);
/* Resends kept packets by RTP sequence number. Returns how many it sent. */
guint call_video_retransmit(struct call_video *video, const guint16 *seqs,
			    guint count, gint64 now_us, call_video_send_fn send,
			    gpointer data);
struct call_video_recovery {
	guint requests, requested, retransmitted, gave_up, late;
	guint keyframe_requests, rembs, rate_changes, rescales;
	guint target_kbps, encoder_kbps, rtt_ms, hold_ms;
	guint width, height;
	/* The kernel's send queue: last delay and rate cuts. */
	guint local_queue_ms, local_cuts;
};
void call_video_recovery(const struct call_video *video,
			 struct call_video_recovery *out);
/* Conference transport feedback: packets the relay saw for this stream. */
void call_video_uplink_loss(struct call_video *video, guint packets,
			    guint received);
/* The conference transport must validate the relay and sender first. */
gboolean call_video_receive_group_feedback(struct call_video *video,
					   const guint8 *packet, gsize size,
					   guint32 sender, gint64 now);
void call_video_keyframe(struct call_video *video);
/*
 * Bytes the kernel still holds for the call socket (SIOCOUTQ, see
 * line_udp_send_queue()), sampled before each picture. From successive
 * samples and the bytes sent in between it measures what the uplink
 * really carries. More than 60 ms of queue for 100 ms cuts the rate to
 * 85% of that (70% above 200 ms), again every 300 ms while the queue still
 * grows, and pauses growth until it drains.
 */
void call_video_local_queue(struct call_video *video, guint64 held_bytes,
			    gint64 now_us);
/* Sets a screen-specific send budget. Zero kbps restores webcam settings. */
void call_video_screen(struct call_video *video, guint kbps, guint fps);

/*
 * The VP8 speed (VP8E_SET_CPUUSED) to use after a second of frames that
 * averaged average_us to encode at fps: one step faster above 60% of the
 * frame interval, one step back toward base below 25%.
 */
int call_video_pace(int speed, int base, gint64 average_us, guint fps);
void call_video_close(struct call_video *video);

/*
 * Decoder for a peer's screen share. A PDTP presentation stream carries
 * one EVS3 fragment per message, without the RTP frame-length field; pass
 * each complete message in order.
 */
struct call_share_decoder;
struct call_share_decoder *call_share_decoder_new(void);
void call_share_decoder_free(struct call_share_decoder *decoder);
gboolean
call_share_decoder_keyframe_needed(const struct call_share_decoder *decoder);
gboolean call_share_decode(struct call_share_decoder *decoder,
			   const guint8 *frame, gsize size,
			   call_video_display_fn display, gpointer data);

/* Screen share encoder producing EVS3 fragments for a PDTP stream. */
struct call_share_encoder;
typedef gboolean (*call_share_fragment_fn)(const guint8 *fragment, gsize size,
					   gboolean key, gpointer data);
struct call_share_encoder *call_share_encoder_new(guint kbps, guint fps);
void call_share_encoder_free(struct call_share_encoder *encoder);
void call_share_encoder_keyframe(struct call_share_encoder *encoder);
gboolean call_share_encode(struct call_share_encoder *encoder,
			   const struct line_camera_frame *frame, gint64 now_us,
			   call_share_fragment_fn emit, gpointer data,
			   GError **error);
gsize call_share_descriptor(guint8 *p, guint16 picture, gboolean first,
			    gboolean last, gboolean key, guint resolution,
			    guint delay_ms, guint32 size);
