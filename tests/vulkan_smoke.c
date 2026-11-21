// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/vk_canvas.h"
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
	VkPipeline pipeline, opaque;
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
	const uint8_t glyph[] = { 255, 128 };
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
	INIT(vk_image_init(ctx, &test->glyph, (VkExtent2D){ 2, 1 },
			   VK_FORMAT_R8_UNORM, sampled));
	INIT(vk_image_upload(ctx, &test->glyph, glyph, sizeof(glyph)));
	INIT(vk_image_slot_write(ctx, 0, &test->glyph.view,
				 ctx->texture_layout));
	test->image_slot = vk_image_slot_alloc(&test->ctx);
	INIT(vk_image_init(ctx, &test->image, (VkExtent2D){ 1, 1 }, RGBA,
			   sampled));
	INIT(vk_image_upload(ctx, &test->image, image, sizeof(image)));
	INIT(vk_image_slot_write(ctx, test->image_slot, &test->image.view,
				 ctx->texture_layout));
	INIT(vk_buffer_init(ctx, &test->primitives, 64 + 1024 * FRAME_COUNT,
			    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, mapped));
	INIT(vk_buffer_init(ctx, &test->readback, WIDTH * HEIGHT * 4,
			    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
				    VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
	INIT(vk_ui_pipeline(ctx, RGBA, &test->pipeline));
	INIT(vk_ui_pipeline_mode(ctx, RGBA, true, &test->opaque));
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

static VkResult draw_readback(struct render_test *test, uint64_t frame,
			      const struct ui_prim *items, uint32_t count,
			      const struct ui_clip *clips, uint32_t clip_count,
			      VkClearColorValue clear,
			      vk_present_upload_fn upload, void *user)
{
	const struct vk_ctx *ctx = &test->ctx;
	VkCommandBuffer cmd = test->commands[frame % FRAME_COUNT];
	size_t prim_bytes = count * sizeof(*items);
	size_t offset = ((0 - test->primitives.address) & 63) +
			1024 * (frame % FRAME_COUNT);
	char *mapped = (char *)test->primitives.mapped + offset;
	struct ui_push push = {
		.prims = test->primitives.address + offset,
		.clips = test->primitives.address + offset + prim_bytes,
		.inv_viewport = { 2.0f / WIDTH, 2.0f / HEIGHT },
		.clip_count = clip_count,
	};

	memcpy(mapped, items, prim_bytes);
	if (clip_count)
		memcpy(mapped + prim_bytes, clips, clip_count * sizeof(*clips));
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
	if (upload) {
		result = upload(user, cmd, frame % VK_PRESENT_SLOTS);
		if (result != VK_SUCCESS)
			return result;
	}
	image_barrier(cmd, test->target.handle,
		      frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
			      VK_IMAGE_LAYOUT_UNDEFINED,
		      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
	/* The synthetic presenter can discard the clear only for a complete
	 * canvas. The sparse-copy case deliberately leaves pixels uncovered.
	 */
	bool covers = upload && count && items[0].prim_type == UI_PRIM_CANVAS &&
		      items[0].pos[0] == 0 && items[0].pos[1] == 0 &&
		      items[0].size[0] == WIDTH && items[0].size[1] == HEIGHT;
	vk_ui_draw_mode(ctx, cmd, upload ? test->opaque : test->pipeline,
			test->view, test->target.extent, &push, count, clear,
			covers);
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

static VkResult draw(struct render_test *test, uint64_t frame)
{
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
		  .uv_min = { 1, 0 },
		  .uv_max = { 0, 1 },
		  .color = 0xffff0000,
		  .prim_type = UI_PRIM_GLYPH,
		  .clip_rect_idx = 1 },
		{ .pos = { 60, 4 },
		  .size = { 20, 20 },
		  .uv_max = { 1, 1 },
		  .color = 0xffffffff,
		  .corner_radius = 3,
		  .tex_index = test->image_slot,
		  .smp_index = 1,
		  .prim_type = UI_PRIM_IMAGE },
	};
	const struct ui_clip clips[] = {
		{ .bounds = { 0, 0, 72, HEIGHT } },
		{ .bounds = { 38.5f, 0, 43.5f, HEIGHT } },
	};
	return draw_readback(test, frame, items, sizeof(items) / sizeof(*items),
			     clips, sizeof(clips) / sizeof(*clips),
			     (VkClearColorValue){ 0 }, NULL, NULL);
}

/* Exercise the production upload callback with real GPU images and a readable
 * target. draw_readback retires every submission before exposing its pixels.
 */
struct vk_present {
	struct render_test *test;
	uint64_t frame;
	VkRect2D damage;
};

VkResult vk_present_wait(struct vk_present *p)
{
	(void)p;
	return VK_SUCCESS;
}

struct vk_ctx *vk_present_context(struct vk_present *p)
{
	return &p->test->ctx;
}

void vk_present_set_damage(struct vk_present *p, const VkRect2D *changed)
{
	p->damage = changed ? *changed : (VkRect2D){ 0 };
}

VkResult vk_present_draw(struct vk_present *p, const struct ui_prim *prims,
			 uint32_t count, VkClearColorValue clear,
			 vk_present_upload_fn upload, void *user)
{
	return draw_readback(p->test, p->frame++, prims, count, NULL, 0, clear,
			     upload, user);
}

static bool check_color(struct render_test *test, unsigned x, unsigned y,
			unsigned r, unsigned g, unsigned b, int tolerance)
{
	const uint8_t *pixel =
		(const uint8_t *)test->readback.mapped + 4 * (WIDTH * y + x);
	if (abs(pixel[0] - (int)r) <= tolerance &&
	    abs(pixel[1] - (int)g) <= tolerance &&
	    abs(pixel[2] - (int)b) <= tolerance && pixel[3] == 255)
		return true;
	fprintf(stderr,
		"Canvas pixel %u,%u was %u,%u,%u,%u; expected %u,%u,%u,255\n",
		x, y, pixel[0], pixel[1], pixel[2], pixel[3], r, g, b);
	return false;
}

static bool check_video(struct render_test *test, const struct gpu_video *v,
			const uint8_t y[3][4], int cb, int cr)
{
	for (int j = 0; j < 3; j++) {
		for (int i = 0; i < 2; i++) {
			int x = i, row = j;
			switch (v->rotation) {
			case 90:
				x = 2 - j;
				row = i;
				break;
			case 180:
				x = 1 - i;
				row = 2 - j;
				break;
			case 270:
				x = j;
				row = 1 - i;
				break;
			}
			int luma =
				298 * (y[j][i] > 16 ? y[j][i] - 16 : 0) + 128;
			int r = (luma + 409 * cr) >> 8;
			int g = (luma - 100 * cb - 208 * cr) >> 8;
			int b = (luma + 516 * cb) >> 8;
			if (!check_color(test, v->x + x, v->y + row,
					 r < 0	 ? 0 :
					 r > 255 ? 255 :
						   r,
					 g < 0	 ? 0 :
					 g > 255 ? 255 :
						   g,
					 b < 0	 ? 0 :
					 b > 255 ? 255 :
						   b,
					 1))
				return false;
		}
	}
	return true;
}

static VkResult check_canvas(struct render_test *test)
{
	struct vk_present p = { .test = test, .frame = 3 };
	struct vk_canvas *canvas = vk_canvas_open(&p);
	if (!canvas)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	uint32_t background[HEIGHT][WIDTH + 2];
	const uint8_t original_y[3][4] = { { 32, 64 },
					   { 96, 128 },
					   { 160, 192 } };
	uint8_t y[3][4], u[] = { 144, 144 }, v[] = { 110, 110 };
	struct gpu_scene s = { .pixels = background,
			       .width = WIDTH,
			       .height = HEIGHT,
			       .pitch = sizeof(background[0]) };
	struct gpu_video *tile = &s.video[0];
	memcpy(y, original_y, sizeof(y));
	for (unsigned j = 0; j < HEIGHT; j++)
		for (unsigned i = 0; i < WIDTH; i++)
			background[j][i] = 0x007c4521;
	*tile = (struct gpu_video){ .plane = { y[0], u, v },
				    .stride = { 4, 1, 1 },
				    .width = 2,
				    .height = 3,
				    .generation = 1,
				    .x = 10,
				    .y = 5,
				    .w = 2,
				    .h = 3 };
	VkResult result = VK_SUCCESS;
	for (int turns = 0; turns < 4; turns++) {
		tile->rotation = 90 * turns;
		tile->w = turns & 1 ? 3 : 2;
		tile->h = turns & 1 ? 2 : 3;
		if (turns == 1) {
			/* Poison unchanged CPU pixels and test retention across slots. */
			memset(background, 0, sizeof(background));
			memset(y, 0, sizeof(y));
			background[2][1] = 0x0000ff80;
			s.damage = (struct gpu_rect){ 1, 2, 1, 1 };
		}
		result = vk_canvas_draw(canvas, &s);
		if (result != VK_SUCCESS || (!turns && p.damage.extent.width) ||
		    (turns == 1 &&
		     (p.damage.offset.x != 1 || p.damage.offset.y != 2 ||
		      p.damage.extent.width != 12 ||
		      p.damage.extent.height != 6)) ||
		    (turns >= 2 &&
		     (p.damage.offset.x != 10 || p.damage.offset.y != 5 ||
		      p.damage.extent.width != 3 ||
		      p.damage.extent.height != 3)) ||
		    !check_color(test, 0, 0, 124, 69, 33, 0) ||
		    (turns && !check_color(test, 1, 2, 0, 255, 128, 0)) ||
		    !check_video(test, tile, original_y, 16, -18))
			goto failed;
		s.damage = (struct gpu_rect){ 0 };
	}

	/* Dispatch two disjoint transfers into the same retained texture, then
	 * display them at different positions by sampling source rectangles.
	 */
	background[0][5] = 0x22cc1133;
	background[0][6] = 0x0099bb44;
	s.upload_count = s.copy_count = 2;
	for (int i = 0; i < 2; i++) {
		s.uploads[i] = (struct gpu_copy){ { 5 + i, 0, 1, 1 },
						  { 7 + i, 3, 1, 1 } };
		s.copies[i] =
			(struct gpu_copy){ { 7 + i, 3, 1, 1 }, { i, 0, 1, 1 } };
	}
	tile->generation++;
	result = vk_canvas_draw(canvas, &s);
	if (result != VK_SUCCESS || !check_color(test, 0, 0, 204, 17, 51, 0) ||
	    !check_color(test, 1, 0, 153, 187, 68, 0) ||
	    !check_color(test, 2, 0, 0, 0, 0, 0) ||
	    !check_video(test, tile, (const uint8_t (*)[4])y, 16, -18))
		goto failed;
	vk_canvas_close(canvas);
	return VK_SUCCESS;
failed:
	vk_canvas_close(canvas);
	return result == VK_SUCCESS ? VK_ERROR_UNKNOWN : result;
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
		{ 37, 8, { 0, 0, 0, 0 } }, /* before clip */
		{ 38, 8, { 0, 0, 128, 128 } }, /* inclusive left edge */
		{ 41, 8, { 0, 0, 128, 128 } }, /* clipped, flipped glyph */
		{ 42, 8, { 0, 0, 255, 255 } },
		{ 43, 8, { 0, 0, 0, 0 } }, /* exclusive right edge */
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
	vkDestroyPipeline(ctx->device, test->opaque, NULL);
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
			"A compatible Vulkan descriptor heap or buffer device is required (%d).\n",
			result);
		return 1;
	}
	VkPhysicalDeviceProperties properties;

	vkGetPhysicalDeviceProperties(test.ctx.gpu, &properties);
	printf("%s (descriptor %s)\n", properties.deviceName,
	       test.ctx.descriptor_heap ? "heap" : "buffer");
	result = init_test(&test);
	for (uint64_t frame = 0; frame < 3 && result == VK_SUCCESS; frame++) {
		result = draw(&test, frame);
		if (result == VK_SUCCESS && !pixels_match(&test))
			result = VK_ERROR_UNKNOWN;
	}
	if (result == VK_SUCCESS)
		result = check_canvas(&test);
	finish(&test);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "Vulkan offscreen check failed (%d).\n",
			result);
		return 1;
	}
	puts("Verified UI shapes, retained canvas uploads, presentation damage and rotated I420 video.");
	return 0;
}
