// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_present.h"
#include "vk_frame.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_WAIT_NS UINT64_C(8000000)
#define PRESENT_FENCE_WAIT_NS UINT64_C(1000000000)

struct present_image {
	VkImage image;
	VkImageView view;
	VkSemaphore ready;
	VkFence presented;
	VkRect2D repair; /* Damage since this specific image was last rendered. */
	bool pending;
};

struct frame_slot {
	VkCommandBuffer cmd;
	VkSemaphore acquire;
	uint64_t done;
	struct vk_buffer prims;
	uint32_t capacity;
};

struct vk_present {
	struct vk_ctx ctx;
	VkSwapchainKHR swapchain;
	struct present_image *images;
	uint32_t image_count;
	VkExtent2D extent;
	VkFormat format;
	VkPipeline pipeline;
	VkCommandPool pool;
	VkSemaphore timeline;
	SDL_Window *window;
	VkExtent2D drawable;
	bool log, opaque, rebuild, previous_present, retain_images;
	VkRect2D damage;
	struct frame_slot slots[VK_PRESENT_SLOTS];
	uint64_t submitted;
};

static VkResult create_surface(VkInstance instance, void *window,
			       VkSurfaceKHR *surface)
{
	if (SDL_Vulkan_CreateSurface(window, instance, surface))
		return VK_SUCCESS;
	fprintf(stderr, "SDL Vulkan surface: %s\n", SDL_GetError());
	return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult init_context(struct vk_present *p, SDL_Window *window,
			     uint32_t vendor)
{
	unsigned count = 0;

	if (!SDL_Vulkan_GetInstanceExtensions(window, &count, NULL))
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	const char **extensions = calloc(count, sizeof(*extensions));

	if (!extensions)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;

	if (SDL_Vulkan_GetInstanceExtensions(window, &count, extensions)) {
		struct vk_surface_config config = {
			.extensions = extensions,
			.extension_count = count,
			.create = create_surface,
			.window = window,
		};

		result = vk_ctx_init(&p->ctx, vendor, &config);
	}
	free(extensions);
	return result;
}

static VkResult init_frames(struct vk_present *p)
{
	VkDevice device = p->ctx.device;
	VkPhysicalDeviceProperties properties;

	if (p->log) {
		vkGetPhysicalDeviceProperties(p->ctx.gpu, &properties);
		fprintf(stderr,
			"Vulkan device: %s (%04x:%04x%s), API %u.%u.%u, SDL %s, descriptor %s\n",
			properties.deviceName, properties.vendorID,
			properties.deviceID,
			properties.deviceType ==
					VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ?
				", discrete" :
				"",
			VK_API_VERSION_MAJOR(properties.apiVersion),
			VK_API_VERSION_MINOR(properties.apiVersion),
			VK_API_VERSION_PATCH(properties.apiVersion),
			SDL_GetCurrentVideoDriver(),
			p->ctx.descriptor_heap ? "heap" : "buffer");
	}
	VkResult result;
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = p->ctx.family,
	};
	result = vkCreateCommandPool(device, &pool, NULL, &p->pool);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBuffer commands[VK_PRESENT_SLOTS];
	VkCommandBufferAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = p->pool,
		.commandBufferCount = VK_PRESENT_SLOTS,
	};
	result = vkAllocateCommandBuffers(device, &alloc, commands);
	if (result != VK_SUCCESS)
		return result;
	VkSemaphoreCreateInfo semaphore = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	for (unsigned i = 0; i < VK_PRESENT_SLOTS; i++) {
		p->slots[i].cmd = commands[i];
		result = vkCreateSemaphore(device, &semaphore, NULL,
					   &p->slots[i].acquire);
		if (result != VK_SUCCESS)
			return result;
	}
	VkSemaphoreTypeCreateInfo timeline = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	semaphore.pNext = &timeline;
	return vkCreateSemaphore(device, &semaphore, NULL, &p->timeline);
}

static void destroy_swapchain(struct vk_present *p)
{
	VkDevice device = p->ctx.device;

	/* Presentation fences cover resources that a render timeline alone cannot retire. */
	for (uint32_t i = 0; i < p->image_count; i++) {
		struct present_image *image = &p->images[i];

		if (image->pending)
			vkWaitForFences(device, 1, &image->presented, VK_TRUE,
					UINT64_MAX);
		vkDestroyFence(device, image->presented, NULL);
		vkDestroySemaphore(device, image->ready, NULL);
		vkDestroyImageView(device, image->view, NULL);
	}
	free(p->images);
	p->images = NULL;
	p->image_count = 0;
	vkDestroySwapchainKHR(device, p->swapchain, NULL);
	p->swapchain = VK_NULL_HANDLE;
}

static VkResult init_images(struct vk_present *p, VkFormat format)
{
	VkDevice device = p->ctx.device;
	uint32_t count = 0;
	VkResult result =
		vkGetSwapchainImagesKHR(device, p->swapchain, &count, NULL);

	if (result != VK_SUCCESS || !count)
		return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED :
					      result;
	VkImage *images = calloc(count, sizeof(*images));
	struct present_image *slots = calloc(count, sizeof(*slots));

	if (!images || !slots) {
		free(images);
		free(slots);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	result = vkGetSwapchainImagesKHR(device, p->swapchain, &count, images);
	if (result != VK_SUCCESS) {
		free(images);
		free(slots);
		return result;
	}
	p->images = slots;
	p->image_count = count;
	const VkSemaphoreCreateInfo semaphore = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
	};
	const VkFenceCreateInfo fence = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
	};

	for (uint32_t i = 0; i < count && result == VK_SUCCESS; i++) {
		slots[i].image = images[i];
		VkImageViewCreateInfo view = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
					      0, 1 },
		};
		result = vkCreateImageView(device, &view, NULL, &slots[i].view);
		if (result == VK_SUCCESS)
			result = vkCreateSemaphore(device, &semaphore, NULL,
						   &slots[i].ready);
		if (result == VK_SUCCESS)
			result = vkCreateFence(device, &fence, NULL,
					       &slots[i].presented);
	}
	free(images);
	return result;
}

static uint32_t clamp_extent(uint32_t size, uint32_t min, uint32_t max)
{
	return size < min ? min : size > max ? max : size;
}

static VkResult rebuild_swapchain(struct vk_present *p, SDL_Window *window)
{
	const struct vk_ctx *ctx = &p->ctx;
	int w, h;

	SDL_Vulkan_GetDrawableSize(window, &w, &h);
	if (w <= 0 || h <= 0)
		return VK_NOT_READY;
	VkSurfaceCapabilitiesKHR caps;
	VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
		ctx->gpu, ctx->surface, &caps);

	if (result != VK_SUCCESS)
		return result;
	VkExtent2D extent = caps.currentExtent;

	if (extent.width == UINT32_MAX)
		extent = (VkExtent2D){
			clamp_extent(w, caps.minImageExtent.width,
				     caps.maxImageExtent.width),
			clamp_extent(h, caps.minImageExtent.height,
				     caps.maxImageExtent.height),
		};
	if (!extent.width || !extent.height)
		return VK_NOT_READY;
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkSurfaceFormatKHR format = { 0 };

	result = vk_surface_format(ctx->gpu, ctx->surface, &format);
	if (result != VK_SUCCESS)
		return result;
	/* FIFO paces presentation without spinning or tearing. */
	uint32_t count = caps.minImageCount + 1;

	if (caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;
	VkCompositeAlphaFlagsKHR alpha = caps.supportedCompositeAlpha;

	if (!alpha)
		return VK_ERROR_INITIALIZATION_FAILED;
	result = vkQueueWaitIdle(ctx->queue);
	if (result != VK_SUCCESS)
		return result;
	destroy_swapchain(p);
	if (!p->pipeline || p->format != format.format) {
		vkDestroyPipeline(ctx->device, p->pipeline, NULL);
		p->pipeline = VK_NULL_HANDLE;
		result = vk_ui_pipeline_mode(ctx, format.format, p->opaque,
					     &p->pipeline);
		if (result != VK_SUCCESS)
			return result;
		p->format = format.format;
	}
	bool retain = ctx->incremental_present &&
		      caps.currentTransform ==
			      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	VkSwapchainCreateInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = ctx->surface,
		.minImageCount = count,
		.imageFormat = format.format,
		.imageColorSpace = format.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = caps.currentTransform,
		.compositeAlpha = alpha & (0u - alpha),
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		/* Reacquired images must also retain previously obscured pixels. */
		.clipped = !retain,
	};
	result = vkCreateSwapchainKHR(ctx->device, &info, NULL, &p->swapchain);
	if (result != VK_SUCCESS)
		return result;
	result = init_images(p, format.format);
	if (result != VK_SUCCESS)
		return result;
	p->extent = extent;
	p->drawable = (VkExtent2D){ w, h };
	p->retain_images = retain;
	p->rebuild = false;
	if (p->log)
		fprintf(stderr, "Vulkan swapchain: %ux%u, %u images, FIFO\n",
			extent.width, extent.height, p->image_count);
	return VK_SUCCESS;
}

static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
		       VkImageLayout to)
{
	bool present = to == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkImageMemoryBarrier2 barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask =
			present ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0,
		.dstStageMask =
			present ?
				0 :
				VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask =
			present ? 0 : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &barrier,
	};

	vkCmdPipelineBarrier2(cmd, &info);
}

static VkResult stage_primitives(struct vk_present *p, struct frame_slot *slot,
				 const struct ui_prim *prims, uint32_t count,
				 VkDeviceAddress *address)
{
	if (!count) {
		*address = 0;
		return VK_SUCCESS;
	}
	if (!prims || (uint64_t)count * sizeof(*prims) > SIZE_MAX)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (slot->capacity < count) {
		uint32_t capacity = count < 16 ? 16 : count;
		struct vk_buffer buffer = { 0 };
		VkResult result = vk_buffer_init(
			&p->ctx, &buffer,
			(VkDeviceSize)capacity * sizeof(*prims) + 63,
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (result != VK_SUCCESS)
			return result;
		vk_buffer_destroy(&p->ctx, &slot->prims);
		slot->prims = buffer;
		slot->capacity = capacity;
	}
	VkDeviceSize offset = -slot->prims.address & 63;
	*address = slot->prims.address + offset;
	memcpy((char *)slot->prims.mapped + offset, prims,
	       (size_t)count * sizeof(*prims));
	return vk_buffer_flush(&p->ctx, &slot->prims);
}

static VkResult record_frame(struct vk_present *p, VkCommandBuffer cmd,
			     const struct present_image *image,
			     VkDeviceAddress address, uint32_t count,
			     VkClearColorValue clear, bool discard,
			     VkRect2D area, uint32_t index,
			     vk_present_upload_fn upload, void *user)
{
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkResult result = vkBeginCommandBuffer(cmd, &begin);

	if (result != VK_SUCCESS)
		return result;
	if (upload) {
		result = upload(user, cmd, index);
		if (result != VK_SUCCESS)
			return result < 0 ? result :
					    VK_ERROR_INITIALIZATION_FAILED;
	}
	const struct ui_push push = {
		.prims = address,
		.inv_viewport = { 2.0f / p->extent.width,
				  2.0f / p->extent.height },
	};
	bool partial = area.extent.width != p->extent.width ||
		       area.extent.height != p->extent.height;
	transition(cmd, image->image,
		   partial ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR :
			     VK_IMAGE_LAYOUT_UNDEFINED,
		   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vk_ui_draw_mode(&p->ctx, cmd, p->pipeline, image->view, p->extent, area,
			&push, count, clear, discard);
	transition(cmd, image->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	return vkEndCommandBuffer(cmd);
}

static VkResult wait_for(struct vk_present *p, uint64_t value, uint64_t timeout)
{
	if (!value)
		return VK_SUCCESS;
	VkSemaphoreWaitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &p->timeline,
		.pValues = &value,
	};
	return vkWaitSemaphores(p->ctx.device, &wait, timeout);
}

VkResult vk_present_wait(struct vk_present *p)
{
	return wait_for(p, p->submitted, UINT64_MAX);
}

struct vk_ctx *vk_present_context(struct vk_present *p)
{
	return &p->ctx;
}

VkExtent2D vk_present_extent(struct vk_present *p)
{
	int w, h;
	SDL_Vulkan_GetDrawableSize(p->window, &w, &h);
	if (w <= 0 || h <= 0)
		return (VkExtent2D){ 0 };
	if ((unsigned)w == p->drawable.width &&
	    (unsigned)h == p->drawable.height)
		return p->extent;
	return (VkExtent2D){ w, h };
}

void vk_present_set_damage(struct vk_present *p, const VkRect2D *changed)
{
	p->damage = changed ? *changed : (VkRect2D){ 0 };
}

/* Both rectangles are inside the swapchain, and delta is nonempty. */
static VkRect2D include_damage(VkRect2D old, VkRect2D delta)
{
	if (!old.extent.width)
		return delta;
	uint32_t right = old.offset.x + old.extent.width;
	uint32_t bottom = old.offset.y + old.extent.height;
	uint32_t x2 = delta.offset.x + delta.extent.width;
	uint32_t y2 = delta.offset.y + delta.extent.height;

	if (old.offset.x < delta.offset.x)
		delta.offset.x = old.offset.x;
	if (old.offset.y < delta.offset.y)
		delta.offset.y = old.offset.y;
	delta.extent.width = (right > x2 ? right : x2) - delta.offset.x;
	delta.extent.height = (bottom > y2 ? bottom : y2) - delta.offset.y;
	return delta;
}

VkResult vk_present_draw(struct vk_present *p, const struct ui_prim *prims,
			 uint32_t count, VkClearColorValue clear,
			 vk_present_upload_fn upload, void *user)
{
	VkDevice device = p->ctx.device;
	uint32_t slot_index = p->submitted % VK_PRESENT_SLOTS;
	struct frame_slot *slot = &p->slots[slot_index];
	VkExtent2D drawable = vk_present_extent(p);
	VkRect2D damage = p->damage;
	bool incremental = p->previous_present;
	p->damage = (VkRect2D){ 0 };
	p->previous_present = false;
	VkDeviceAddress address;
	VkResult result;
	if (!drawable.width || !drawable.height)
		return VK_NOT_READY;
	if (p->rebuild || drawable.width != p->extent.width ||
	    drawable.height != p->extent.height) {
		incremental = false;
		result = rebuild_swapchain(p, p->window);
		if (result != VK_SUCCESS)
			return result;
	}
	result = wait_for(p, slot->done, FRAME_WAIT_NS);
	if (result != VK_SUCCESS)
		return result;
	result = stage_primitives(p, slot, prims, count, &address);
	if (result != VK_SUCCESS)
		return result;
	uint32_t index;

	result = vkAcquireNextImageKHR(device, p->swapchain, FRAME_WAIT_NS,
				       slot->acquire, VK_NULL_HANDLE, &index);
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		p->rebuild = true;
		incremental = false;
		result = rebuild_swapchain(p, p->window);
		if (result != VK_SUCCESS)
			return result;
		result = vkAcquireNextImageKHR(device, p->swapchain,
					       FRAME_WAIT_NS, slot->acquire,
					       VK_NULL_HANDLE, &index);
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
			p->rebuild = true;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		return result;
	bool suboptimal = result == VK_SUBOPTIMAL_KHR;
	struct present_image *image = &p->images[index];

	if (image->pending) {
		result = vkWaitForFences(device, 1, &image->presented, VK_TRUE,
					 PRESENT_FENCE_WAIT_NS);
		if (result != VK_SUCCESS)
			/* This slot already owns an acquired image and semaphore. */
			return result == VK_TIMEOUT ?
				       VK_ERROR_INITIALIZATION_FAILED :
				       result;
		image->pending = false;
		result = vkResetFences(device, 1, &image->presented);
		if (result != VK_SUCCESS)
			return result;
	}
	incremental = incremental && p->retain_images && damage.extent.width &&
		      damage.extent.height && damage.offset.x >= 0 &&
		      damage.offset.y >= 0 &&
		      (uint64_t)damage.offset.x + damage.extent.width <=
			      p->extent.width &&
		      (uint64_t)damage.offset.y + damage.extent.height <=
			      p->extent.height;
	VkRect2D area = { .extent = p->extent };
	if (p->retain_images) {
		/* A fresh chain or unknown delta invalidates every image. */
		for (uint32_t i = 0; i < p->image_count; i++)
			p->images[i].repair =
				incremental ?
					include_damage(p->images[i].repair,
						       damage) :
					area;
		area = image->repair;
		image->repair = (VkRect2D){ 0 };
	}
	bool full_canvas =
		p->opaque && count && prims[0].prim_type == UI_PRIM_CANVAS &&
		prims[0].clip_rect_idx == UI_NO_CLIP && prims[0].pos[0] == 0 &&
		prims[0].pos[1] == 0 && prims[0].size[0] == p->extent.width &&
		prims[0].size[1] == p->extent.height;
	result = record_frame(p, slot->cmd, image, address, count, clear,
			      full_canvas, area, slot_index, upload, user);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferSubmitInfo command = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = slot->cmd,
	};
	VkSemaphoreSubmitInfo acquire = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = slot->acquire,
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	};
	VkSemaphoreSubmitInfo signals[] = {
		{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		  .semaphore = image->ready,
		  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
		{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		  .semaphore = p->timeline,
		  .value = p->submitted + 1,
		  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
	};
	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = 1,
		.pWaitSemaphoreInfos = &acquire,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &command,
		.signalSemaphoreInfoCount = 2,
		.pSignalSemaphoreInfos = signals,
	};

	result = vkQueueSubmit2(p->ctx.queue, 1, &submit, VK_NULL_HANDLE);
	if (result != VK_SUCCESS)
		return result;
	slot->done = ++p->submitted;
	/* Damage is relative to the previous present on the swapchain, even
	 * when a different swapchain image was just acquired and redrawn.
	 */
	VkRectLayerKHR rect = { .offset = damage.offset,
				.extent = damage.extent };
	VkPresentRegionKHR region = { .rectangleCount = 1,
				      .pRectangles = &rect };
	VkPresentRegionsKHR regions = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
		.swapchainCount = 1,
		.pRegions = &region
	};
	VkSwapchainPresentFenceInfoKHR fence = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
		.pNext = incremental ? &regions : NULL,
		.swapchainCount = 1,
		.pFences = &image->presented,
	};
	VkPresentInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext = &fence,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &image->ready,
		.swapchainCount = 1,
		.pSwapchains = &p->swapchain,
		.pImageIndices = &index,
	};

	result = vkQueuePresentKHR(p->ctx.queue, &info);
	p->previous_present = result == VK_SUCCESS ||
			      result == VK_SUBOPTIMAL_KHR;
	image->pending = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
			 result == VK_ERROR_OUT_OF_DATE_KHR ||
			 result == VK_ERROR_SURFACE_LOST_KHR;
	p->rebuild = suboptimal || result == VK_SUBOPTIMAL_KHR ||
		     result == VK_ERROR_OUT_OF_DATE_KHR;
	return result == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : result;
}

struct vk_present *vk_present_open(SDL_Window *window, uint32_t vendor,
				   bool log, bool opaque)
{
	struct vk_present *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->window = window;
	p->log = log;
	p->opaque = opaque;
	VkResult result = init_context(p, window, vendor);
	if (result == VK_SUCCESS)
		result = init_frames(p);
	if (result == VK_SUCCESS)
		result = rebuild_swapchain(p, window);
	if (result == VK_NOT_READY) {
		p->rebuild = true;
		return p;
	}
	if (result != VK_SUCCESS) {
		if (log)
			fprintf(stderr,
				"Vulkan presenter initialization failed (%d)\n",
				result);
		vk_present_close(p);
		return NULL;
	}
	return p;
}

void vk_present_close(struct vk_present *p)
{
	if (!p)
		return;
	if (p->ctx.device) {
		vkDeviceWaitIdle(p->ctx.device);
		destroy_swapchain(p);
		vkDestroyPipeline(p->ctx.device, p->pipeline, NULL);
		for (unsigned i = 0; i < VK_PRESENT_SLOTS; i++) {
			vkDestroySemaphore(p->ctx.device, p->slots[i].acquire,
					   NULL);
			vk_buffer_destroy(&p->ctx, &p->slots[i].prims);
		}
		vkDestroySemaphore(p->ctx.device, p->timeline, NULL);
		vkDestroyCommandPool(p->ctx.device, p->pool, NULL);
		vk_ctx_destroy(&p->ctx);
	}
	free(p);
}
