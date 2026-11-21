/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AVERNELLA_VK_DESC_BUFFER_H
#define AVERNELLA_VK_DESC_BUFFER_H

#include "vk_ctx.h"

struct vk_desc_buffer {
	struct vk_buffer memory;
	VkDeviceSize base, images, samplers, image_size, sampler_size;
	VkDescriptorSetLayout set;
	VkPipelineLayout layout;
	VkSampler sampler[UI_SAMPLER_SLOTS];
	VkImageView view[UI_TEXTURE_SLOTS];
};

VkResult vk_desc_buffer_open(struct vk_ctx *ctx);
void vk_desc_buffer_close(struct vk_ctx *ctx);
VkResult vk_desc_buffer_write(const struct vk_ctx *ctx, uint32_t slot,
			      const VkImageViewCreateInfo *view,
			      VkImageLayout layout);
void vk_desc_buffer_bind(const struct vk_ctx *ctx, VkCommandBuffer cmd,
			 const struct ui_push *push);

#endif
