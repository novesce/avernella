/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_VK_TEX_H
#define LINE_VK_TEX_H

#include "vk_ctx.h"

struct vk_image {
	VkImage handle;
	VkDeviceMemory memory;
	VkImageViewCreateInfo view;
	VkExtent2D extent;
};

VkResult vk_image_init(const struct vk_ctx *ctx, struct vk_image *image,
		       VkExtent2D extent, VkFormat format,
		       VkImageUsageFlags usage);
void vk_image_destroy(const struct vk_ctx *ctx, struct vk_image *image);
/* Upload a new image with HOST_TRANSFER usage before any queue accesses it.
 * On success its layout is ctx->texture_layout.
 */
VkResult vk_image_upload(const struct vk_ctx *ctx, const struct vk_image *image,
			 const void *pixels);

#endif
