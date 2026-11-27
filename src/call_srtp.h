/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <srtp2/srtp.h>

gboolean call_srtp_open(srtp_t *ctx, const guint8 key[30],
			srtp_ssrc_type_t type, guint32 ssrc);

struct call_reception_report {
	guint32 high, received, expected_prior, received_prior, transit;
	guint64 jitter16;
	gint64 sent_at;
	guint16 first;
	gboolean started;
};

void call_rtcp_arrival(struct call_reception_report *rr, guint16 seq,
		       guint32 stamp, guint rate, gint64 now);
gsize call_rtcp_report(struct call_reception_report *rr, srtp_t tx,
		       guint32 local, guint32 peer, guint8 *packet,
		       gsize capacity);
