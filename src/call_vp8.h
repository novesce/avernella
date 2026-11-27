/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "call_video.h"
#include "media_io.h"

struct call_vp8_decoder;

struct call_vp8_decoder *call_vp8_open(guint threads, gboolean presentation);
void call_vp8_close(struct call_vp8_decoder *decoder);
const char *call_vp8_backend(const struct call_vp8_decoder *decoder);
/* The caller validates keyframe dimensions and handles keyframe requests. */
int call_vp8_decode(struct call_vp8_decoder *decoder, const guint8 *packet,
		    gsize size, gboolean key, guint rotation,
		    call_video_display_fn display, gpointer data);

/* The worker opts in only with a renderer capable of importing NV12.
 * An accepted DMA frame still invokes display with dimensions and no I420
 * planes so the call can update reception state while the UI owns it.
 */
typedef gboolean (*call_dma_send_fn)(gboolean presentation,
				     const struct line_camera_frame *,
				     const struct line_dmabuf_layout *, int,
				     int, gpointer);
void call_vp8_set_sink(struct call_vp8_decoder *d,
		       gboolean (*visible)(gpointer), call_dma_send_fn send,
		       gpointer data);
int call_vp8_release_fd(struct call_vp8_decoder *d);
void call_vp8_handle_rejection(struct call_vp8_decoder *d);

/* Once the first keyframe selects software permanently, the worker owns the
 * libvpx context. Only its single consumer may call these functions.
 */
void call_vp8_enable_worker(struct call_vp8_decoder *d);
int call_vp8_worker_fd(struct call_vp8_decoder *d);
void call_vp8_discontinuity(struct call_vp8_decoder *d);
enum call_vp8_submit {
	CALL_VP8_INLINE,
	CALL_VP8_QUEUED,
	CALL_VP8_FULL
};
enum call_vp8_submit call_vp8_submit(struct call_vp8_decoder *d,
				     const guint8 *p, gsize n, gboolean key,
				     guint rotation, guint tag);
struct call_vp8_output {
	struct line_camera_frame image;
	guint tag;
	int shown;
};
/* A successful take borrows the decoder planes until release. */
gboolean call_vp8_take(struct call_vp8_decoder *d, struct call_vp8_output *out);
void call_vp8_release(struct call_vp8_decoder *d);
