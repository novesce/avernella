// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/vk_frame.h"
#include "render/vk_tex.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 96
#define HEIGHT 48
#define FRAME_COUNT 2
#define RGBA VK_FORMAT_R8G8B8A8_UNORM

struct render_test {
	struct vk_ctx ctx;
	struct vk_image target, glyph, image;
	struct vk_buffer primitives, readback;
	VkImageView view;
	VkPipeline pipeline;
	VkCommandPool pool;
	VkCommandBuffer commands[FRAME_COUNT];
	VkSemaphore timeline;
	uint32_t image_slot;
};

static VkResult init_test(struct render_test *test)
{
	const struct vk_ctx *ctx = &test->ctx;
	const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT |
					  VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
	const VkMemoryPropertyFlags mapped =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const uint8_t glyph = 128;
	const uint8_t image[] = { 255, 200, 0, 255 };
	VkResult result;

	/* Keep failures attributable to the operation that returned them. */
#define INIT(call)                                                          \
	do {                                                                \
		result = (call);                                            \
		if (result != VK_SUCCESS) {                                 \
			fprintf(stderr, "%s returned %d\n", #call, result); \
			return result;                                      \
		}                                                           \
	} while (0)
	INIT(vk_image_init(ctx, &test->target, (VkExtent2D){ WIDTH, HEIGHT },
			   RGBA,
			   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				   VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	INIT(vkCreateImageView(ctx->device, &test->target.view, NULL,
			       &test->view));
	INIT(vk_image_init(ctx, &test->glyph, (VkExtent2D){ 1, 1 },
			   VK_FORMAT_R8_UNORM, sampled));
	INIT(vk_image_upload(ctx, &test->glyph, &glyph));
	INIT(vk_image_slot_write(ctx, 0, &test->glyph.view,
				 ctx->texture_layout));
	test->image_slot = vk_image_slot_alloc(&test->ctx);
	INIT(vk_image_init(ctx, &test->image, (VkExtent2D){ 1, 1 }, RGBA,
			   sampled));
	INIT(vk_image_upload(ctx, &test->image, image));
	INIT(vk_image_slot_write(ctx, test->image_slot, &test->image.view,
				 ctx->texture_layout));
	INIT(vk_buffer_init(ctx, &test->primitives, 64 + 256 * FRAME_COUNT,
			    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, mapped));
	INIT(vk_buffer_init(ctx, &test->readback, WIDTH * HEIGHT * 4,
			    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
				    VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
	INIT(vk_ui_pipeline(ctx, RGBA, &test->pipeline));
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = ctx->family,
	};
	INIT(vkCreateCommandPool(ctx->device, &pool, NULL, &test->pool));
	VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = test->pool,
		.commandBufferCount = FRAME_COUNT,
	};
	INIT(vkAllocateCommandBuffers(ctx->device, &commands, test->commands));
	VkSemaphoreTypeCreateInfo type = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	VkSemaphoreCreateInfo semaphore = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &type,
	};
	INIT(vkCreateSemaphore(ctx->device, &semaphore, NULL, &test->timeline));
#undef INIT
	return VK_SUCCESS;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image,
			  VkImageLayout from, VkImageLayout to, bool copy)
{
	VkImageMemoryBarrier2 barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask =
			copy ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT :
			from == VK_IMAGE_LAYOUT_UNDEFINED ?
			       VK_PIPELINE_STAGE_2_NONE :
			       VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = copy ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT :
				 from == VK_IMAGE_LAYOUT_UNDEFINED ?
					VK_ACCESS_2_NONE :
					VK_ACCESS_2_TRANSFER_READ_BIT,
		.dstStageMask =
			copy ? VK_PIPELINE_STAGE_2_COPY_BIT :
			       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = copy ? VK_ACCESS_2_TRANSFER_READ_BIT :
					VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo dependency = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &barrier,
	};

	vkCmdPipelineBarrier2(cmd, &dependency);
}

static VkResult draw(struct render_test *test, uint64_t frame)
{
	const struct vk_ctx *ctx = &test->ctx;
	VkCommandBuffer cmd = test->commands[frame % FRAME_COUNT];
	const struct ui_prim items[] = {
		{ .pos = { 4, 4 },
		  .size = { 24, 24 },
		  .color = 0xff0000ff,
		  .border_color = 0xff00ff00,
		  .corner_radius = 6,
		  .border_width = 2,
		  .clip_rect_idx = UI_NO_CLIP },
		{ .pos = { 36, 4 },
		  .size = { 12, 12 },
		  .uv_max = { 1, 1 },
		  .color = 0xffff0000,
		  .prim_type = UI_PRIM_GLYPH,
		  .clip_rect_idx = UI_NO_CLIP },
		{ .pos = { 60, 4 },
		  .size = { 20, 20 },
		  .uv_max = { 1, 1 },
		  .color = 0xffffffff,
		  .corner_radius = 3,
		  .tex_index = test->image_slot,
		  .smp_index = 1,
		  .prim_type = UI_PRIM_IMAGE },
	};
	const struct ui_clip clip = { .bounds = { 0, 0, 72, HEIGHT } };
	size_t offset = ((0 - test->primitives.address) & 63) +
			256 * (frame % FRAME_COUNT);
	char *mapped = (char *)test->primitives.mapped + offset;
	struct ui_push push = {
		.prims = test->primitives.address + offset,
		.clips = test->primitives.address + offset + sizeof(items),
		.inv_viewport = { 2.0f / WIDTH, 2.0f / HEIGHT },
		.clip_count = 1,
	};

	memcpy(mapped, items, sizeof(items));
	memcpy(mapped + sizeof(items), &clip, sizeof(clip));
	VkResult result = vk_buffer_flush(ctx, &test->primitives);

	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	result = vkBeginCommandBuffer(cmd, &begin);
	if (result != VK_SUCCESS)
		return result;
	image_barrier(cmd, test->target.handle,
		      frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
			      VK_IMAGE_LAYOUT_UNDEFINED,
		      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
	vk_ui_draw(ctx, cmd, test->pipeline, test->view, test->target.extent,
		   &push, sizeof(items) / sizeof(items[0]));
	image_barrier(cmd, test->target.handle,
		      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
	VkBufferImageCopy copy = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { WIDTH, HEIGHT, 1 },
	};

	vkCmdCopyImageToBuffer(cmd, test->target.handle,
			       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       test->readback.handle, 1, &copy);
	VkMemoryBarrier2 readback = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
		.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &readback,
	};
	vkCmdPipelineBarrier2(cmd, &dep);
	result = vkEndCommandBuffer(cmd);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferSubmitInfo buffer = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = cmd,
	};
	VkSemaphoreSubmitInfo signal = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = test->timeline,
		.value = frame + 1,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &buffer,
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signal,
	};

	result = vkQueueSubmit2(ctx->queue, 1, &submit, VK_NULL_HANDLE);
	if (result != VK_SUCCESS)
		return result;
	VkSemaphoreWaitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &test->timeline,
		.pValues = &signal.value,
	};

	result = vkWaitSemaphores(ctx->device, &wait, 5ull * 1000000000);
	return result == VK_SUCCESS ?
		       vk_buffer_invalidate(ctx, &test->readback) :
		       result;
}

static bool pixels_match(const struct render_test *test)
{
	const struct {
		uint32_t x, y;
		uint8_t expected[4];
	} checks[] = {
		{ 4, 4, { 0, 0, 0, 0 } }, /* rounded corner */
		{ 5, 16, { 0, 255, 0, 255 } }, /* border */
		{ 16, 16, { 255, 0, 0, 255 } }, /* SDF fill */
		{ 40, 8, { 0, 0, 128, 128 } }, /* R8 coverage */
		{ 65, 12, { 255, 200, 0, 255 } }, /* image */
		{ 73, 12, { 0, 0, 0, 0 } }, /* clipping */
	};
	const uint8_t *pixels = test->readback.mapped;

	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		const uint8_t *pixel =
			pixels + 4 * (WIDTH * checks[i].y + checks[i].x);

		if (memcmp(pixel, checks[i].expected, 4)) {
			fprintf(stderr, "Pixel %u,%u was %u,%u,%u,%u\n",
				checks[i].x, checks[i].y, pixel[0], pixel[1],
				pixel[2], pixel[3]);
			return false;
		}
	}
	return true;
}

static void finish(struct render_test *test)
{
	struct vk_ctx *ctx = &test->ctx;

	vkDeviceWaitIdle(ctx->device);
	vkDestroyCommandPool(ctx->device, test->pool, NULL);
	vkDestroySemaphore(ctx->device, test->timeline, NULL);
	vkDestroyPipeline(ctx->device, test->pipeline, NULL);
	vkDestroyImageView(ctx->device, test->view, NULL);
	vk_image_slot_free(ctx, test->image_slot);
	vk_image_destroy(ctx, &test->image);
	vk_image_destroy(ctx, &test->glyph);
	vk_image_destroy(ctx, &test->target);
	vk_buffer_destroy(ctx, &test->readback);
	vk_buffer_destroy(ctx, &test->primitives);
	vk_ctx_destroy(ctx);
}

int main(int argc, char **argv)
{
	char *end = NULL;
	unsigned long vendor = argc == 2 ? strtoul(argv[1], &end, 0) : 0;

	if (argc > 2 ||
	    (end && (end == argv[1] || *end || errno || vendor > UINT32_MAX))) {
		fprintf(stderr, "Usage: %s [vendor-id]\n", argv[0]);
		return 2;
	}
	struct render_test test = { 0 };
	VkResult result = vk_ctx_init(&test.ctx, (uint32_t)vendor, NULL);

	if (result != VK_SUCCESS) {
		fprintf(stderr,
			"A Vulkan 1.4 descriptor-heap device is required (%d).\n",
			result);
		return 1;
	}
	VkPhysicalDeviceProperties properties;

	vkGetPhysicalDeviceProperties(test.ctx.gpu, &properties);
	printf("%s\n", properties.deviceName);
	result = init_test(&test);
	for (uint64_t frame = 0; frame < 3 && result == VK_SUCCESS; frame++) {
		result = draw(&test, frame);
		if (result == VK_SUCCESS && !pixels_match(&test))
			result = VK_ERROR_UNKNOWN;
	}
	finish(&test);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "Vulkan offscreen check failed (%d).\n",
			result);
		return 1;
	}
	puts("Verified rounded fill, border, glyph coverage, image and clipping with one draw.");
	return 0;
}
