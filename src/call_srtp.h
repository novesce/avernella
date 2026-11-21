/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <srtp2/srtp.h>

gboolean call_srtp_open(srtp_t *ctx, const guint8 key[30],
			srtp_ssrc_type_t type, guint32 ssrc);
