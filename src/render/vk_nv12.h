/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gpu_dmabuf.h"
#include "vk_ctx.h"

struct vk_nv12;
struct vk_nv12_image;
struct vk_nv12_batch {
	struct vk_nv12_image *images[3];
};

struct vk_nv12 *vk_nv12_open(const struct vk_ctx *ctx);
void vk_nv12_close(struct vk_nv12 *state);
VkResult vk_nv12_prepare(struct vk_nv12 *state, VkFormat target,
			 const struct gpu_dmabuf *const frames[3],
			 uint64_t completed, struct vk_nv12_batch *batch,
			 unsigned int *rejected);
void vk_nv12_submitted(const struct vk_nv12_batch *batch, uint64_t serial);
void vk_nv12_barriers(const struct vk_nv12_batch *batch, VkCommandBuffer cmd,
		      uint32_t family, bool acquire);
void vk_nv12_draw(const struct vk_nv12_image *image, VkCommandBuffer cmd,
		  const struct ui_push *push, uint32_t index);
