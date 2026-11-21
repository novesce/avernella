/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

typedef struct LineAudio LineAudio;
LineAudio *line_audio_open(const guint8 tx[30], const guint8 rx[30],
			   guint8 payload_type, guint32 tx_ssrc,
			   guint32 rx_ssrc, GError **error);
int line_audio_capture(LineAudio *audio, guint8 *packet, gsize capacity,
		       GError **error);
gboolean line_audio_receive(LineAudio *audio, const guint8 *packet, gsize size);
gint64 line_audio_playout(LineAudio *audio, gint64 now);
void line_audio_mute(LineAudio *audio, gboolean muted);
void line_audio_close(LineAudio *audio);
