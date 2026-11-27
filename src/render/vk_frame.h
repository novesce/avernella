/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_VK_FRAME_H
#define LINE_VK_FRAME_H

#include "vk_ctx.h"

struct vk_nv12_batch;
VkResult vk_ui_nv12_pipeline(const struct vk_ctx *ctx, VkFormat format,
			     VkPipelineLayout layout, VkPipeline *pipeline);
VkResult vk_ui_pipeline_mode(const struct vk_ctx *ctx, VkFormat format,
			     bool opaque, VkPipeline *pipeline);
static inline VkResult vk_ui_pipeline(const struct vk_ctx *ctx, VkFormat format,
				      VkPipeline *pipeline)
{
	return vk_ui_pipeline_mode(ctx, format, false, pipeline);
}

/* Skip layers hidden by a fully covering opaque canvas or video tile. */
bool vk_visible_layers(const struct ui_prim **prims, uint32_t *count,
		       VkRect2D area);

/* The caller owns layout transitions and synchronization for the target.
 * Discard the load only if opaque primitives cover the render area.
 */
void vk_ui_draw_mode(const struct vk_ctx *ctx, VkCommandBuffer cmd,
		     VkPipeline pipeline, VkImageView target, VkExtent2D extent,
		     VkRect2D area, const struct ui_push *push, uint32_t count,
		     VkClearColorValue clear, bool discard,
		     const struct vk_nv12_batch *external,
		     const struct ui_prim *items);
static inline void vk_ui_draw(const struct vk_ctx *ctx, VkCommandBuffer cmd,
			      VkPipeline pipeline, VkImageView target,
			      VkExtent2D extent, const struct ui_push *push,
			      uint32_t count, VkClearColorValue clear)
{
	vk_ui_draw_mode(ctx, cmd, pipeline, target, extent,
			(VkRect2D){ .extent = extent }, push, count, clear,
			false, NULL, NULL);
}

#endif
