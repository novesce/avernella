/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "camera.h"

struct call_vp8_jetson;

struct call_vp8_jetson *call_vp8_jetson_open(guint width, guint height,
					     guint kbps, guint fps,
					     guint key_distance);
void call_vp8_jetson_close(struct call_vp8_jetson *encoder);
gboolean call_vp8_jetson_frame(struct call_vp8_jetson *encoder,
			       const struct line_camera_frame *frame,
			       gint64 pts90k, gboolean force_key,
			       const guint8 **picture, gsize *size,
			       gboolean *key);
