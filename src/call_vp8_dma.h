/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "call_vp8.h"
#include <libavutil/frame.h>

struct call_dma_pool;
void call_dma_forget_latest(struct call_dma_pool *p);
AVFrame *call_dma_take_latest(struct call_dma_pool *p, guint *rotation);

/* Positive on delivery, zero for backpressure, negative if unsupported. */
int call_dma_submit(struct call_dma_pool **pool, AVFrame *surface,
		    const struct line_camera_frame *frame,
		    gboolean presentation, call_dma_send_fn send,
		    gpointer data);
int call_dma_reject_fd(const struct call_dma_pool *pool);
gboolean call_dma_rejected(const struct call_dma_pool *pool);
void call_dma_retire(struct call_dma_pool **pool);
