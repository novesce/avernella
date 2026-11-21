// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_tex.h"
#include <string.h>

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
	if (!ctx->host_image_copy && (usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT))
		usage = (usage & ~VK_IMAGE_USAGE_HOST_TRANSFER_BIT) |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

/* The one-time upload helper also runs on Vulkan 1.3 without host image copy.
 * Repeated production updates use per-frame staging in vk_canvas instead.
 */
static VkResult stage_image(const struct vk_ctx *ctx,
			    const struct vk_image *image, const void *pixels,
			    size_t bytes)
{
	struct vk_buffer source = { 0 };
	VkResult result = vk_buffer_init(ctx, &source, bytes,
					 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (result != VK_SUCCESS)
		return result;
	memcpy(source.mapped, pixels, bytes);
	VkCommandPool pool = VK_NULL_HANDLE;
	result = vk_buffer_flush(ctx, &source);
	if (result != VK_SUCCESS)
		goto done;
	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = ctx->family,
	};
	result = vkCreateCommandPool(ctx->device, &pool_info, NULL, &pool);
	if (result != VK_SUCCESS)
		goto done;
	VkCommandBufferAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	result = vkAllocateCommandBuffers(ctx->device, &alloc, &cmd);
	if (result != VK_SUCCESS)
		goto done;
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	result = vkBeginCommandBuffer(cmd, &begin);
	if (result != VK_SUCCESS)
		goto done;
	VkImageMemoryBarrier2 barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->handle,
		.subresourceRange = image->view.subresourceRange,
	};
	VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				 .imageMemoryBarrierCount = 1,
				 .pImageMemoryBarriers = &barrier };
	vkCmdPipelineBarrier2(cmd, &dep);
	VkBufferImageCopy copy = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { image->extent.width, image->extent.height, 1 },
	};
	vkCmdCopyBufferToImage(cmd, source.handle, image->handle,
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = ctx->texture_layout;
	vkCmdPipelineBarrier2(cmd, &dep);
	result = vkEndCommandBuffer(cmd);
	if (result != VK_SUCCESS)
		goto done;
	VkCommandBufferSubmitInfo command = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = cmd,
	};
	VkSubmitInfo2 submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				 .commandBufferInfoCount = 1,
				 .pCommandBufferInfos = &command };
	result = vkQueueSubmit2(ctx->queue, 1, &submit, VK_NULL_HANDLE);
	if (result == VK_SUCCESS)
		result = vkQueueWaitIdle(ctx->queue);
done:
	vkDestroyCommandPool(ctx->device, pool, NULL);
	vk_buffer_destroy(ctx, &source);
	return result;
}

VkResult vk_image_upload(const struct vk_ctx *ctx, const struct vk_image *image,
			 const void *pixels, size_t bytes)
{
	if (!ctx->host_image_copy)
		return stage_image(ctx, image, pixels, bytes);
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
