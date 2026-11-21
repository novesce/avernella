// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_tex.h"

void vk_image_destroy(const struct vk_ctx *ctx, struct vk_image *image)
{
	vkDestroyImage(ctx->device, image->handle, NULL);
	vkFreeMemory(ctx->device, image->memory, NULL);
	*image = (struct vk_image){ 0 };
}

VkResult vk_image_init(const struct vk_ctx *ctx, struct vk_image *image,
		       VkExtent2D extent, VkFormat format,
		       VkImageUsageFlags usage)
{
	VkImageFormatProperties format_limits;
	VkResult result = vkGetPhysicalDeviceImageFormatProperties(
		ctx->gpu, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		usage, 0, &format_limits);

	if (result != VK_SUCCESS)
		return result;
	if (!extent.width || !extent.height ||
	    extent.width > format_limits.maxExtent.width ||
	    extent.height > format_limits.maxExtent.height)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { extent.width, extent.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};

	result = vkCreateImage(ctx->device, &info, NULL, &image->handle);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements req;

	vkGetImageMemoryRequirements(ctx->device, image->handle, &req);
	uint32_t type = vk_memory_type(ctx, req.memoryTypeBits, 0,
				       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (type == UINT32_MAX) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
		goto fail;
	}
	VkMemoryAllocateInfo allocation = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};

	result = vkAllocateMemory(ctx->device, &allocation, NULL,
				  &image->memory);
	if (result != VK_SUCCESS)
		goto fail;
	result =
		vkBindImageMemory(ctx->device, image->handle, image->memory, 0);
	if (result != VK_SUCCESS)
		goto fail;
	image->extent = extent;
	image->view = (VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->handle,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	return VK_SUCCESS;
fail:
	vk_image_destroy(ctx, image);
	return result;
}

VkResult vk_image_upload(const struct vk_ctx *ctx, const struct vk_image *image,
			 const void *pixels)
{
	VkHostImageLayoutTransitionInfo transition = {
		.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
		.image = image->handle,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = ctx->texture_layout,
		.subresourceRange = image->view.subresourceRange,
	};
	VkResult result = vkTransitionImageLayout(ctx->device, 1, &transition);

	if (result != VK_SUCCESS)
		return result;
	VkMemoryToImageCopy region = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
		.pHostPointer = pixels,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { image->extent.width, image->extent.height, 1 },
	};
	VkCopyMemoryToImageInfo info = {
		.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
		.dstImage = image->handle,
		.dstImageLayout = ctx->texture_layout,
		.regionCount = 1,
		.pRegions = &region,
	};

	return vkCopyMemoryToImage(ctx->device, &info);
}
