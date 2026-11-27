// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_srtp.h"
#include <string.h>

gboolean call_srtp_open(srtp_t *ctx, const guint8 key[30],
			srtp_ssrc_type_t type, guint32 ssrc)
{
	static gsize initialized;
	srtp_policy_t policy = { 0 };

	if (g_once_init_enter(&initialized))
		g_once_init_leave(&initialized,
				  srtp_init() == srtp_err_status_ok ? 1 : 2);
	if (initialized != 1)
		return FALSE;
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	policy.key = (guint8 *)key;
	policy.ssrc.type = type;
	policy.ssrc.value = ssrc;
	policy.window_size = 128;
	return srtp_create(ctx, &policy) == srtp_err_status_ok;
}

/* RFC 3550 Appendix A.8. The SRTP replay window has already removed
 * duplicates. Count an authenticated late repair even after playout.
 */
void call_rtcp_arrival(struct call_reception_report *rr, guint16 seq,
		       guint32 stamp, guint rate, gint64 now)
{
	guint32 arrival = (guint32)(now / G_USEC_PER_SEC) * rate +
			  (now % G_USEC_PER_SEC) * rate / G_USEC_PER_SEC;
	guint32 transit = arrival - stamp;

	if (!rr->started) {
		rr->first = rr->high = seq;
		rr->started = TRUE;
	} else {
		gint16 advance = seq - (guint16)rr->high;
		gint64 delta = (gint32)(transit - rr->transit);

		if (advance > 0)
			rr->high += advance;
		rr->jitter16 += ABS(delta) - ((rr->jitter16 + 8) >> 4);
	}
	rr->transit = transit;
	rr->received++;
}

/* RFC 3550 section 6.4.1. Sequence cycles count from the first packet;
 * cumulative losses may decrease when a previous interval is repaired.
 */
gsize call_rtcp_report(struct call_reception_report *rr, srtp_t tx,
		       guint32 local, guint32 peer, guint8 *packet,
		       gsize capacity)
{
	guint32 expected = rr->high - rr->first + 1;
	guint32 interval = expected - rr->expected_prior;
	guint32 received = rr->received - rr->received_prior;
	guint32 lost = interval - MIN(received, interval);
	guint fraction = interval ? MIN(255u, (guint64)lost * 256 / interval) :
				    0;
	gint32 total =
		CLAMP((gint64)expected - rr->received, -0x800000, 0x7fffff);
	guint32 fields[8] = {
		/* V=2, one report block, RR=201, seven additional words. */
		GUINT32_TO_BE(0x81c90007),
		GUINT32_TO_BE(local),
		GUINT32_TO_BE(peer),
		0,
		GUINT32_TO_BE(rr->high),
		GUINT32_TO_BE((guint32)(rr->jitter16 >> 4)),
		0,
		0
	};
	int bytes = sizeof(fields);

	if (!rr->started || !packet ||
	    capacity < sizeof(fields) + SRTP_MAX_TRAILER_LEN + 4)
		return 0;
	fields[3] =
		GUINT32_TO_BE((fraction << 24) | ((guint32)total & 0xffffff));
	memcpy(packet, fields, sizeof(fields));
	if (srtp_protect_rtcp(tx, packet, &bytes) != srtp_err_status_ok)
		return 0;
	rr->expected_prior = expected;
	rr->received_prior = rr->received;
	return bytes;
}
