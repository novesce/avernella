/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "camera.h"

struct call_vp8_encoder;
struct AVFrame;
struct AVBufferRef;

/* FFmpeg VA-API path only. dst retains the producer lease until its last
 * reference is released. No pixel memory is mapped into the CPU.
 */
gboolean call_capture_import_nv12(struct AVFrame *dst,
				  struct AVBufferRef *frames_ctx,
				  const struct line_camera_frame *frame);

/* Verify VP8 encode and direct import on the selected encoder before
 * offering this exact size and DRM modifier to the compositor.
 */
gboolean call_vp8_capture_probe(guint width, guint height, guint fps,
				guint kbps, guint64 *modifier);

/* Returns NULL when no GPU can encode the requested VP8 stream. */
struct call_vp8_encoder *call_vp8_encoder_open(guint width, guint height,
					       guint kbps, guint fps,
					       guint key_distance);
void call_vp8_encoder_close(struct call_vp8_encoder *encoder);
const char *call_vp8_encoder_name(const struct call_vp8_encoder *encoder);

/* Output borrows the encoder's packet until the next frame or close. On
 * failure discard this encoder and encode the same input as a software key.
 */
gboolean call_vp8_encoder_frame(struct call_vp8_encoder *encoder,
				const struct line_camera_frame *frame,
				gint64 pts90k, gboolean force_key,
				const guint8 **picture, gsize *size,
				gboolean *key);
