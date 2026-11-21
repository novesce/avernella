// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/vk_frame.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_FLIGHT 2
#define NS_SECOND UINT64_C(1000000000)
#define TEST_FRAMES 60

struct present_image {
	VkImage image;
	VkImageView view;
	VkSemaphore ready;
	VkFence presented;
	bool pending;
};

struct frame_slot {
	VkCommandBuffer cmd;
	VkSemaphore acquire;
	uint64_t done;
};

struct preview {
	struct vk_ctx ctx;
	VkSwapchainKHR swapchain;
	struct present_image *images;
	uint32_t image_count;
	VkExtent2D extent;
	VkFormat format;
	VkPipeline pipeline;
	VkCommandPool pool;
	VkSemaphore timeline;
	struct vk_buffer prims;
	VkDeviceSize stride;
	struct frame_slot slots[IN_FLIGHT];
	uint64_t submitted;
	unsigned frames, rebuilds;
	double host_total_ms, host_max_ms;
};

static VkResult create_surface(VkInstance instance, void *window,
			       VkSurfaceKHR *surface)
{
	if (SDL_Vulkan_CreateSurface(window, instance, surface))
		return VK_SUCCESS;
	fprintf(stderr, "SDL Vulkan surface: %s\n", SDL_GetError());
	return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult init_context(struct preview *p, SDL_Window *window,
			     uint32_t vendor)
{
	unsigned count = 0;

	if (!SDL_Vulkan_GetInstanceExtensions(window, &count, NULL))
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	const char **extensions = calloc(count + 2, sizeof(*extensions));

	if (!extensions)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;

	if (SDL_Vulkan_GetInstanceExtensions(window, &count, extensions)) {
		extensions[count++] =
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
		extensions[count++] =
			VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
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

static VkResult init_frames(struct preview *p)
{
	VkDevice device = p->ctx.device;
	VkPhysicalDeviceProperties properties;

	vkGetPhysicalDeviceProperties(p->ctx.gpu, &properties);
	printf("Device: %s, API %u.%u.%u, SDL %s\n", properties.deviceName,
	       VK_API_VERSION_MAJOR(properties.apiVersion),
	       VK_API_VERSION_MINOR(properties.apiVersion),
	       VK_API_VERSION_PATCH(properties.apiVersion),
	       SDL_GetCurrentVideoDriver());
	VkDeviceSize atom = properties.limits.nonCoherentAtomSize;
	VkDeviceSize alignment = atom > 64 ? atom : 64;

	p->stride = (4 * sizeof(struct ui_prim) + alignment - 1) &
		    ~(alignment - 1);
	VkResult result =
		vk_buffer_init(&p->ctx, &p->prims, IN_FLIGHT * p->stride,
			       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
				       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (result != VK_SUCCESS)
		return result;
	if ((p->prims.address & 63) || ((uintptr_t)p->prims.mapped & 63))
		return VK_ERROR_FEATURE_NOT_PRESENT;
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = p->ctx.family,
	};
	result = vkCreateCommandPool(device, &pool, NULL, &p->pool);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBuffer commands[IN_FLIGHT];
	VkCommandBufferAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = p->pool,
		.commandBufferCount = IN_FLIGHT,
	};
	result = vkAllocateCommandBuffers(device, &alloc, commands);
	if (result != VK_SUCCESS)
		return result;
	VkSemaphoreCreateInfo semaphore = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	for (unsigned i = 0; i < IN_FLIGHT; i++) {
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

static void destroy_swapchain(struct preview *p)
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

static VkResult choose_format(const struct vk_ctx *ctx,
			      VkSurfaceFormatKHR *format)
{
	uint32_t count = 0;
	VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(
		ctx->gpu, ctx->surface, &count, NULL);
	if (result != VK_SUCCESS || !count)
		return result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED :
					      result;
	VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));

	if (!formats)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->gpu, ctx->surface,
						      &count, formats);
	if (result == VK_SUCCESS && count) {
		*format = formats[0];
		for (uint32_t i = 0; i < count; i++)
			if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
			     formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
			    formats[i].colorSpace ==
				    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				*format = formats[i];
				break;
			}
	}
	free(formats);
	return result == VK_SUCCESS && !count ? VK_ERROR_FORMAT_NOT_SUPPORTED :
						result;
}

static VkResult init_images(struct preview *p, VkFormat format)
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

static VkResult rebuild_swapchain(struct preview *p, SDL_Window *window)
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

	result = choose_format(ctx, &format);
	if (result != VK_SUCCESS)
		return result;
	/* Use FIFO for the diagnostic so the compositor controls presentation cadence. */
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
		result = vk_ui_pipeline(ctx, format.format, &p->pipeline);
		if (result != VK_SUCCESS)
			return result;
		p->format = format.format;
	}
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
		.clipped = VK_TRUE,
	};
	result = vkCreateSwapchainKHR(ctx->device, &info, NULL, &p->swapchain);
	if (result != VK_SUCCESS)
		return result;
	result = init_images(p, format.format);
	if (result != VK_SUCCESS)
		return result;
	if (!p->rebuilds || p->extent.width != extent.width ||
	    p->extent.height != extent.height)
		p->rebuilds++;
	p->extent = extent;
	int logical_w, logical_h;

	SDL_GetWindowSize(window, &logical_w, &logical_h);
	printf("Swapchain: logical %dx%d, drawable %ux%u, %u images, FIFO\n",
	       logical_w, logical_h, extent.width, extent.height,
	       p->image_count);
	return VK_SUCCESS;
}

static void transition(VkCommandBuffer cmd, VkImage image, bool present)
{
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
		.oldLayout = present ?
				     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
				     VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR :
				       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

static VkResult record_frame(struct preview *p, VkCommandBuffer cmd,
			     const struct present_image *image,
			     VkDeviceSize offset)
{
	float w = p->extent.width, h = p->extent.height;
	struct ui_prim *prims = (void *)((char *)p->prims.mapped + offset);
	float progress = (p->frames % TEST_FRAMES + 1) / (float)TEST_FRAMES;

	prims[0] = (struct ui_prim){ .size = { w, h },
				     .color = 0xff181615,
				     .clip_rect_idx = UI_NO_CLIP };
	prims[1] = (struct ui_prim){ .pos = { w * .08f, h * .14f },
				     .size = { w * .84f, h * .72f },
				     .color = 0xff2c2522,
				     .border_color = 0xff403b35,
				     .border_width = 2,
				     .corner_radius = 18,
				     .clip_rect_idx = UI_NO_CLIP };
	prims[2] = (struct ui_prim){ .pos = { w * .16f, h * .48f },
				     .size = { w * .68f, h * .08f },
				     .color = 0xff171412,
				     .corner_radius = 10,
				     .clip_rect_idx = UI_NO_CLIP };
	prims[3] = (struct ui_prim){ .pos = { w * .16f, h * .48f },
				     .size = { w * .68f * progress, h * .08f },
				     .color = 0xffe8a670,
				     .corner_radius = 10,
				     .clip_rect_idx = UI_NO_CLIP };
	if (!(p->prims.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		VkMappedMemoryRange range = {
			.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
			.memory = p->prims.memory,
			.offset = offset,
			.size = p->stride,
		};
		VkResult result =
			vkFlushMappedMemoryRanges(p->ctx.device, 1, &range);

		if (result != VK_SUCCESS)
			return result;
	}
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkResult result = vkBeginCommandBuffer(cmd, &begin);

	if (result != VK_SUCCESS)
		return result;
	const struct ui_push push = {
		.prims = p->prims.address + offset,
		.inv_viewport = { 2 / w, 2 / h },
	};
	transition(cmd, image->image, false);
	vk_ui_draw(&p->ctx, cmd, p->pipeline, image->view, p->extent, &push, 4);
	transition(cmd, image->image, true);
	return vkEndCommandBuffer(cmd);
}

static VkResult present(struct preview *p)
{
	VkDevice device = p->ctx.device;
	uint64_t start = SDL_GetPerformanceCounter();
	unsigned slot_index = p->submitted % IN_FLIGHT;
	struct frame_slot *slot = &p->slots[slot_index];
	VkSemaphoreWaitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &p->timeline,
		.pValues = &slot->done,
	};
	VkResult result = vkWaitSemaphores(device, &wait, NS_SECOND);

	if (result != VK_SUCCESS)
		return result;
	uint32_t index;

	result = vkAcquireNextImageKHR(device, p->swapchain, NS_SECOND,
				       slot->acquire, VK_NULL_HANDLE, &index);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		return result;
	bool suboptimal = result == VK_SUBOPTIMAL_KHR;
	struct present_image *image = &p->images[index];

	if (image->pending) {
		result = vkWaitForFences(device, 1, &image->presented, VK_TRUE,
					 NS_SECOND);
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
	result = record_frame(p, slot->cmd, image, p->stride * slot_index);
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
	VkSwapchainPresentFenceInfoKHR fence = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
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
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		return result;
	image->pending = true;
	p->frames++;
	double ms = (SDL_GetPerformanceCounter() - start) * 1000.0 /
		    SDL_GetPerformanceFrequency();

	p->host_total_ms += ms;
	if (ms > p->host_max_ms)
		p->host_max_ms = ms;
	return suboptimal ? VK_SUBOPTIMAL_KHR : result;
}

static bool run(struct preview *p, SDL_Window *window, bool test)
{
	bool dirty = true, resize = true, running = true;
	unsigned checkpoints = 0;
	uint64_t next = 0, show_at = 0, idle_submissions = 0;
	uint64_t deadline = SDL_GetTicks64() + 10000;

	while (running) {
		uint64_t now = SDL_GetTicks64();

		if (test && (now >= deadline || p->frames >= TEST_FRAMES))
			break;
		if (show_at && now >= show_at) {
			if (p->submitted != idle_submissions)
				return false;
			puts("Hidden window: zero submissions during the 180 ms pause");
			SDL_ShowWindow(window);
			show_at = 0;
			dirty = resize = true;
		}
		if (test && checkpoints < 3 &&
		    p->frames >= (checkpoints + 1) * 12) {
			if (checkpoints == 2) {
				SDL_HideWindow(window);
				idle_submissions = p->submitted;
				show_at = now + 180;
			} else {
				SDL_SetWindowSize(window,
						  checkpoints ? 520 : 830,
						  checkpoints ? 320 : 510);
				resize = dirty = true;
			}
			checkpoints++;
		}
		bool visible = !show_at &&
			       !(SDL_GetWindowFlags(window) &
				 (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
		if (visible && (dirty || (test && now >= next))) {
			VkResult result = VK_SUCCESS;

			if (resize)
				result = rebuild_swapchain(p, window);
			if (result == VK_SUCCESS) {
				resize = false;
				result = present(p);
			}
			if (result != VK_SUCCESS &&
			    result != VK_SUBOPTIMAL_KHR &&
			    result != VK_ERROR_OUT_OF_DATE_KHR &&
			    result != VK_NOT_READY && result != VK_TIMEOUT) {
				fprintf(stderr,
					"Vulkan presentation failed (%d)\n",
					result);
				return false;
			}
			resize |= result == VK_ERROR_OUT_OF_DATE_KHR ||
				  result == VK_SUBOPTIMAL_KHR;
			dirty = result == VK_ERROR_OUT_OF_DATE_KHR ||
				result == VK_NOT_READY || result == VK_TIMEOUT;
			next = now + 16;
		}
		SDL_Event event;
		bool received =
			test || (visible && dirty) ?
				SDL_WaitEventTimeout(&event, test ? 8 : 16) :
				SDL_WaitEvent(&event);

		while (received) {
			if (event.type == SDL_QUIT ||
			    (event.type == SDL_KEYDOWN &&
			     event.key.keysym.sym == SDLK_ESCAPE))
				running = false;
			if (event.type == SDL_WINDOWEVENT) {
				switch (event.window.event) {
				case SDL_WINDOWEVENT_CLOSE:
					running = false;
					break;
				case SDL_WINDOWEVENT_SIZE_CHANGED:
				case SDL_WINDOWEVENT_DISPLAY_CHANGED: {
					int w, h;

					SDL_Vulkan_GetDrawableSize(window, &w,
								   &h);
					resize |= w < 0 || h < 0 ||
						  (uint32_t)w !=
							  p->extent.width ||
						  (uint32_t)h !=
							  p->extent.height;
					dirty = true;
					break;
				}
				case SDL_WINDOWEVENT_RESTORED:
				case SDL_WINDOWEVENT_SHOWN:
				case SDL_WINDOWEVENT_EXPOSED:
					dirty = true;
					break;
				}
			}
			received = SDL_PollEvent(&event);
		}
	}
	return !test || (p->frames == TEST_FRAMES && p->rebuilds >= 3 &&
			 checkpoints == 3 && !show_at);
}

int main(int argc, char **argv)
{
	char *end = NULL;
	unsigned long vendor = argc > 1 ? strtoul(argv[1], &end, 0) : 0;
	bool test = argc == 3 && !strcmp(argv[2], "--test");

	if (argc > 3 || (argc == 3 && !test) ||
	    (end && (end == argv[1] || *end || vendor > UINT32_MAX))) {
		fprintf(stderr, "Usage: %s [vendor-id [--test]]\n", argv[0]);
		return 2;
	}
	SDL_Window *window = NULL;
	struct preview p = { 0 };
	bool ok = false;

	if (!SDL_Init(SDL_INIT_VIDEO))
		window = SDL_CreateWindow(
			"LINE Vulkan Preview", SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED, 640, 400,
			SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
				SDL_WINDOW_ALLOW_HIGHDPI);
	if (!window) {
		fprintf(stderr, "SDL window: %s\n", SDL_GetError());
		goto done;
	}
	VkResult result = init_context(&p, window, (uint32_t)vendor);

	if (result == VK_SUCCESS)
		result = init_frames(&p);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "Vulkan preview initialization failed (%d)\n",
			result);
		goto done;
	}
	ok = run(&p, window, test);
	printf("Summary: %u frames, %u drawable sizes, host frame mean %.2f ms, max %.2f ms\n",
	       p.frames, p.rebuilds, p.frames ? p.host_total_ms / p.frames : 0,
	       p.host_max_ms);
done:
	if (p.ctx.device) {
		vkDeviceWaitIdle(p.ctx.device);
		destroy_swapchain(&p);
		vkDestroyPipeline(p.ctx.device, p.pipeline, NULL);
		for (unsigned i = 0; i < IN_FLIGHT; i++)
			vkDestroySemaphore(p.ctx.device, p.slots[i].acquire,
					   NULL);
		vkDestroySemaphore(p.ctx.device, p.timeline, NULL);
		vkDestroyCommandPool(p.ctx.device, p.pool, NULL);
		vk_buffer_destroy(&p.ctx, &p.prims);
		vk_ctx_destroy(&p.ctx);
	}
	SDL_DestroyWindow(window);
	SDL_Quit();
	return ok ? 0 : 1;
}
