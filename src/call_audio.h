/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "media_io.h"

typedef struct LineAudio LineAudio;
LineAudio *line_audio_open(const guint8 tx[30], const guint8 rx[30],
			   guint8 payload_type, guint32 tx_ssrc,
			   guint32 rx_ssrc, GError **error);
/* The conference answer assigns independent transmit and receive payloads. */
LineAudio *line_audio_open_group(const guint8 tx[30], const guint8 rx[30],
				 guint8 tx_pt, guint8 rx_pt, guint32 tx_ssrc,
				 guint32 rx_ssrc, GError **error);
int line_audio_capture_fd(const LineAudio *audio);
gboolean line_audio_capture_ready(const LineAudio *audio);
int line_audio_capture(LineAudio *audio, guint8 *packet, gsize capacity,
		       GError **error);
gboolean line_audio_receive(LineAudio *audio, const guint8 *packet, gsize size);
/*
 * The same with the time the kernel received the packet, so the jitter
 * buffer's transit estimator sees network jitter, not the call thread's own
 * delays. Zero means now.
 */
gboolean line_audio_receive_at(LineAudio *audio, const guint8 *packet,
			       gsize size, gint64 arrival);
gsize line_audio_receiver_report(LineAudio *audio, gint64 now, guint8 *packet,
				 gsize capacity);
/* Apply authenticated SR/RR feedback for this microphone stream. */
gboolean line_audio_receive_report(LineAudio *audio, const guint8 *packet,
				   gsize size);
gint64 line_audio_playout(LineAudio *audio, gint64 now);
enum line_media_reception line_audio_reception(const LineAudio *audio);
void line_audio_enable_diagnostics(LineAudio *audio);

/* Receive totals since the stream opened, in 48 kHz samples or packets. */
struct line_audio_stats {
	guint64 played; /* decoded from received packets */
	guint64 concealed; /* synthesized for lost or late packets */
	guint64 fec; /* gap decodes with confirmed Opus LBRR */
	guint64 fec_attempts; /* next-packet decodes, includes PLC */
	gboolean can_count_lbrr; /* libopus provides opus_packet_has_lbrr */
	guint64 silence; /* silence inserted while the queue was empty */
	guint64 dropped; /* decoded audio discarded to bound the delay */
	guint64 late; /* packets that arrived after their playout time */
	guint64 lost; /* packets never received before their playout time */
	/* Playout delay at the speaker and in the jitter buffer, summed once
	 * per decoded packet; delay_ms_sum / delay_count is the mean.
	 */
	guint64 delay_ms_sum, delay_count;
	guint target_ms; /* current adaptive receive delay budget */
	guint expected_loss_pct; /* outgoing Opus in-band loss protection */
};
void line_audio_stats(const LineAudio *audio, struct line_audio_stats *out);
void line_audio_report(LineAudio *audio);
void line_audio_mute(LineAudio *audio, gboolean muted);
void line_audio_close(LineAudio *audio);
