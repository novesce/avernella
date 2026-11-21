/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_VK_FRAME_H
#define LINE_VK_FRAME_H

#include "vk_ctx.h"

VkResult vk_ui_pipeline_mode(const struct vk_ctx *ctx, VkFormat format,
			     bool opaque, VkPipeline *pipeline);
static inline VkResult vk_ui_pipeline(const struct vk_ctx *ctx, VkFormat format,
				      VkPipeline *pipeline)
{
	return vk_ui_pipeline_mode(ctx, format, false, pipeline);
}

/* The caller owns layout transitions and synchronization for the target.
 * Discard the load only if opaque primitives cover the complete target.
 */
void vk_ui_draw_mode(const struct vk_ctx *ctx, VkCommandBuffer cmd,
		     VkPipeline pipeline, VkImageView target, VkExtent2D extent,
		     const struct ui_push *push, uint32_t count,
		     VkClearColorValue clear, bool discard);
static inline void vk_ui_draw(const struct vk_ctx *ctx, VkCommandBuffer cmd,
			      VkPipeline pipeline, VkImageView target,
			      VkExtent2D extent, const struct ui_push *push,
			      uint32_t count, VkClearColorValue clear)
{
	vk_ui_draw_mode(ctx, cmd, pipeline, target, extent, push, count, clear,
			false);
}

#endif
