/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "call_video.h"

struct call_vp8_decoder;

struct call_vp8_decoder *call_vp8_open(guint threads, gboolean presentation);
void call_vp8_close(struct call_vp8_decoder *decoder);
const char *call_vp8_backend(const struct call_vp8_decoder *decoder);
/* The caller validates keyframe dimensions and handles keyframe requests. */
int call_vp8_decode(struct call_vp8_decoder *decoder, const guint8 *packet,
		    gsize size, gboolean key, guint rotation,
		    call_video_display_fn display, gpointer data);
