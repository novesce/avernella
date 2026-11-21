/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_VK_FRAME_H
#define LINE_VK_FRAME_H

#include "vk_ctx.h"

VkResult vk_ui_pipeline(const struct vk_ctx *ctx, VkFormat format,
			VkPipeline *pipeline);
/* The caller owns layout transitions and synchronization for the target. */
void vk_ui_draw(const struct vk_ctx *ctx, VkCommandBuffer cmd,
		VkPipeline pipeline, VkImageView target, VkExtent2D extent,
		const struct ui_push *push, uint32_t count);

#endif
