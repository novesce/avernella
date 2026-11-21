// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_srtp.h"

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
