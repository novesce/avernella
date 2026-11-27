/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gpu_scene.h"
#include <stdbool.h>

/* NV12, one dma-buf object, explicit two-plane DRM modifier, BT.601.
 * The producer finishes decoding before dispatch and may reuse the storage
 * after its final lease reference. Dimensions include allocator padding.
 */
struct gpu_dmabuf {
	int fd;
	uint32_t width, height, offset[2], stride[2], crop_x, crop_y;
	uint64_t modifier;
	bool full_range;
	void *lease;
	void (*retain)(void *lease);
	void (*release)(void *lease);
};

/* Independent of the original renderer ABI. Rejected bits index scene.video. */
struct gpu_dmabuf_backend {
	bool (*supported)(void *state);
	int (*draw)(void *state, const struct gpu_scene *scene,
		    const struct gpu_dmabuf *const frames[3],
		    unsigned int *rejected);
};
