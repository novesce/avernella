// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_canvas.h"
#include "vk_tex.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_COUNT 3
#define MAX_UPLOADS (8 + VIDEO_COUNT * 3)

struct texture {
	struct vk_image image;
	uint32_t slot;
	bool uploaded;
};

struct video_textures {
	struct texture plane[3];
	uint64_t generation;
};

struct staging {
	struct vk_buffer buffer;
	VkDeviceSize capacity;
};

struct video_stamp {
	float box[4];
	uint32_t width, height, rotation;
	uint64_t generation;
};

struct vk_canvas {
	struct vk_present *present;
	struct vk_ctx *ctx;
	VkDeviceSize atom;
	struct texture canvas;
	struct video_textures video[VIDEO_COUNT];
	struct staging staging[VK_PRESENT_SLOTS];
	struct video_stamp last_video[VIDEO_COUNT];
	struct gpu_copy last_copies[8];
	uint32_t last_count;
	bool presented;
};

struct upload {
	struct texture *texture;
	struct gpu_rect rect;
	const uint8_t *pixels;
	int stride;
	size_t row_bytes;
	VkDeviceSize offset;
};

struct uploads {
	struct vk_canvas *canvas;
	struct upload copies[MAX_UPLOADS];
	uint32_t count;
	VkDeviceSize size;
	bool retired, recorded;
};

static void release_texture(struct vk_ctx *ctx, struct texture *t)
{
	vk_image_slot_free(ctx, t->slot);
	vk_image_destroy(ctx, &t->image);
	*t = (struct texture){ 0 };
}

static VkResult retire(struct uploads *u)
{
	if (u->retired)
		return VK_SUCCESS;
	VkResult result = vk_present_wait(u->canvas->present, 0);
	u->retired = result == VK_SUCCESS;
	return result;
}

/* Keep the same sampled image across frame slots. Only a size change needs
 * to retire readers before replacing its memory and heap descriptor.
 */
static VkResult ensure_texture(struct uploads *u, struct texture *t, uint32_t w,
			       uint32_t h, VkFormat format)
{
	struct vk_ctx *ctx = u->canvas->ctx;
	if (t->image.extent.width == w && t->image.extent.height == h)
		return VK_SUCCESS;

	VkResult result = retire(u);
	if (result != VK_SUCCESS)
		return result;
	release_texture(ctx, t);
	result = vk_image_init(ctx, &t->image, (VkExtent2D){ w, h }, format,
			       VK_IMAGE_USAGE_SAMPLED_BIT |
				       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (result != VK_SUCCESS)
		return result;
	t->slot = vk_image_slot_alloc(ctx);
	if (t->slot == UINT32_MAX)
		return VK_ERROR_OUT_OF_POOL_MEMORY;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	/* Native Cairo RGB24 is X,R,G,B in memory on a big-endian CPU. */
	if (format == VK_FORMAT_R8G8B8A8_UNORM)
		t->image.view.components = (VkComponentMapping){
			VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
			VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE
		};
#else
	if (format == VK_FORMAT_B8G8R8A8_UNORM)
		t->image.view.components.a = VK_COMPONENT_SWIZZLE_ONE;
#endif
	return vk_image_slot_write(ctx, t->slot, &t->image.view,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void add_upload(struct uploads *u, struct texture *t,
		       struct gpu_rect rect, const uint8_t *pixels, int stride,
		       size_t bytes)
{
	VkDeviceSize offset = (u->size + 3) & ~(VkDeviceSize)3;
	u->copies[u->count++] = (struct upload){
		.texture = t,
		.rect = rect,
		.pixels = pixels,
		.stride = stride,
		.row_bytes = bytes,
		.offset = offset,
	};
	u->size = offset + (VkDeviceSize)rect.h * bytes;
}

static VkImageMemoryBarrier2 barrier(const struct texture *t, bool complete)
{
	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = t->image.handle,
		.subresourceRange = t->image.view.subresourceRange,
	};
	if (complete) {
		b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	} else {
		if (t->uploaded) {
			b.srcStageMask =
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
	return b;
}

static VkResult record_uploads(void *data, VkCommandBuffer cmd, uint32_t slot)
{
	struct uploads *u = data;
	struct vk_ctx *ctx = u->canvas->ctx;
	if (!u->count)
		return VK_SUCCESS;

	struct staging *s = &u->canvas->staging[slot];
	if (s->capacity < u->size) {
		VkDeviceSize capacity = (u->size + 65535) &
					~(VkDeviceSize)65535;
		vk_buffer_destroy(ctx, &s->buffer);
		s->capacity = 0;
		VkResult result =
			vk_buffer_init(ctx, &s->buffer, capacity,
				       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (result != VK_SUCCESS)
			return result;
		s->capacity = capacity;
	}
	VkImageMemoryBarrier2 barriers[MAX_UPLOADS];
	VkBufferImageCopy regions[MAX_UPLOADS];
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.pImageMemoryBarriers = barriers,
	};
	for (uint32_t i = 0; i < u->count; i++) {
		const struct upload *c = &u->copies[i];
		uint8_t *dst = (uint8_t *)s->buffer.mapped + c->offset;
		const uint8_t *src = c->pixels;

		if (c->stride == (int)c->row_bytes)
			memcpy(dst, src, c->row_bytes * c->rect.h);
		else
			for (int row = 0; row < c->rect.h; row++)
				memcpy(dst + row * c->row_bytes,
				       src + (ptrdiff_t)row * c->stride,
				       c->row_bytes);
		if (!i || c->texture != u->copies[i - 1].texture)
			barriers[dep.imageMemoryBarrierCount++] =
				barrier(c->texture, false);
		regions[i] = (VkBufferImageCopy){
			.bufferOffset = c->offset,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
					      1 },
			.imageOffset = { c->rect.x, c->rect.y, 0 },
			.imageExtent = { c->rect.w, c->rect.h, 1 },
		};
	}
	if (!(s->buffer.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		VkDeviceSize atom = u->canvas->atom;
		VkDeviceSize end = (u->size + atom - 1) & ~(atom - 1);
		VkMappedMemoryRange range = {
			.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
			.memory = s->buffer.memory,
			.size = end <= s->capacity ? end : VK_WHOLE_SIZE,
		};
		VkResult result =
			vkFlushMappedMemoryRanges(ctx->device, 1, &range);
		if (result != VK_SUCCESS)
			return result;
	}
	vkCmdPipelineBarrier2(cmd, &dep);
	dep.imageMemoryBarrierCount = 0;
	for (uint32_t i = 0; i < u->count;) {
		uint32_t first = i++;
		struct texture *t = u->copies[first].texture;
		while (i < u->count && u->copies[i].texture == t)
			i++;
		vkCmdCopyBufferToImage(cmd, s->buffer.handle, t->image.handle,
				       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				       i - first, regions + first);
		barriers[dep.imageMemoryBarrierCount++] = barrier(t, true);
	}
	vkCmdPipelineBarrier2(cmd, &dep);
	u->recorded = true;
	return VK_SUCCESS;
}

static bool inside(struct gpu_rect r, uint32_t w, uint32_t h)
{
	return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
	       (uint64_t)r.x + r.w <= w && (uint64_t)r.y + r.h <= h;
}

static struct ui_prim canvas_copy(const struct gpu_scene *s,
				  struct gpu_rect src, struct gpu_rect dst,
				  uint32_t slot)
{
	return (struct ui_prim){
		.pos = { dst.x, dst.y },
		.size = { dst.w, dst.h },
		.uv_min = { (float)src.x / s->width, (float)src.y / s->height },
		.uv_max = { (float)(src.x + src.w) / s->width,
			    (float)(src.y + src.h) / s->height },
		.tex_index = slot,
		.prim_type = UI_PRIM_CANVAS,
		.clip_rect_idx = UI_NO_CLIP,
	};
}

static VkResult prepare_video(struct uploads *u, struct video_textures *video,
			      const struct gpu_video *v, struct ui_prim *item)
{
	if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->w) ||
	    !isfinite(v->h) || !v->height || v->width > INT_MAX ||
	    v->height > INT_MAX || v->rotation >= 360 || v->rotation % 90)
		return VK_ERROR_INITIALIZATION_FAILED;
	for (unsigned int i = 0; i < 3; i++) {
		uint32_t w = i ? (v->width + 1) / 2 : v->width;
		uint32_t h = i ? (v->height + 1) / 2 : v->height;
		struct texture *t = &video->plane[i];
		if (!v->plane[i] ||
		    (v->stride[i] > -(int)w && v->stride[i] < (int)w))
			return VK_ERROR_INITIALIZATION_FAILED;
		VkResult result =
			ensure_texture(u, t, w, h, VK_FORMAT_R8_UNORM);
		if (result != VK_SUCCESS)
			return result;
		if (!t->uploaded || video->generation != v->generation)
			add_upload(u, t, (struct gpu_rect){ 0, 0, w, h },
				   v->plane[i], v->stride[i], w);
	}
	*item = (struct ui_prim){
		.pos = { v->x, v->y },
		.size = { v->w, v->h },
		.uv_max = { 1, 1 },
		.tex_index = video->plane[0].slot,
		.color = video->plane[1].slot,
		.border_color = video->plane[2].slot,
		.clip_rect_idx = UI_NO_CLIP,
		.prim_type = UI_PRIM_I420 |
			     (v->rotation / 90 << UI_ROTATION_SHIFT),
	};
	return VK_SUCCESS;
}

static void include_box(VkRect2D *damage, const float box[4],
			const struct gpu_scene *scene)
{
	/* Clip before converting to integers. Include fractional edge pixels. */
	if (box[2] <= 0 || box[3] <= 0)
		return;
	double left = box[0], top = box[1];
	double right = left + box[2], bottom = top + box[3];
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > scene->width)
		right = scene->width;
	if (bottom > scene->height)
		bottom = scene->height;
	if (left >= right || top >= bottom)
		return;
	int x = left, y = top, end_x = right, end_y = bottom;
	end_x += end_x < right;
	end_y += end_y < bottom;
	if (damage->extent.width && damage->extent.height) {
		int old_right = damage->offset.x + (int)damage->extent.width;
		int old_bottom = damage->offset.y + (int)damage->extent.height;
		if (x > damage->offset.x)
			x = damage->offset.x;
		if (y > damage->offset.y)
			y = damage->offset.y;
		if (end_x < old_right)
			end_x = old_right;
		if (end_y < old_bottom)
			end_y = old_bottom;
	}
	*damage = (VkRect2D){ { x, y }, { end_x - x, end_y - y } };
}

static void present_damage(struct vk_canvas *c, const struct gpu_scene *s,
			   struct video_stamp next[VIDEO_COUNT])
{
	VkRect2D damage = { 0 };
	bool known = c->presented && c->canvas.uploaded;
	if (inside(s->damage, s->width, s->height)) {
		damage = (VkRect2D){ { s->damage.x, s->damage.y },
				     { s->damage.w, s->damage.h } };
	} else if (s->upload_count || c->last_count != s->copy_count ||
		   memcmp(c->last_copies, s->copies,
			  s->copy_count * sizeof(s->copies[0]))) {
		known = false;
	}
	for (uint32_t i = 0; i < VIDEO_COUNT; i++) {
		const struct gpu_video *v = &s->video[i];
		const struct video_stamp *old = &c->last_video[i];
		if (v->width && v->w > 0 && v->h > 0)
			next[i] = (struct video_stamp){
				.box = { v->x, v->y, v->w, v->h },
				.generation = v->generation,
				.rotation = v->rotation,
				.width = v->width,
				.height = v->height,
			};
		if (old->generation != next[i].generation ||
		    old->rotation != next[i].rotation ||
		    old->width != next[i].width ||
		    old->height != next[i].height ||
		    memcmp(old->box, next[i].box, sizeof(old->box))) {
			include_box(&damage, old->box, s);
			include_box(&damage, next[i].box, s);
		}
	}
	vk_present_set_damage(c->present,
			      known && damage.extent.width ? &damage : NULL);
}

VkResult vk_canvas_draw(struct vk_canvas *c, const struct gpu_scene *s)
{
	struct uploads u = { .canvas = c };
	struct ui_prim items[8 + VIDEO_COUNT];
	struct video_stamp next[VIDEO_COUNT] = { 0 };
	uint32_t count = 0;
	if (!s->pixels || !s->width || !s->height || s->width > INT_MAX / 4 ||
	    s->height > INT_MAX || s->pitch < s->width * 4 ||
	    s->pitch > INT_MAX || s->copy_count > 8 || s->upload_count > 8)
		return VK_ERROR_INITIALIZATION_FAILED;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
#else
	const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
	VkResult result =
		ensure_texture(&u, &c->canvas, s->width, s->height, format);
	if (result != VK_SUCCESS)
		return result;

	struct gpu_rect full = { 0, 0, s->width, s->height };
	struct gpu_rect damage = c->canvas.uploaded ? s->damage : full;
	if (!c->canvas.uploaded && s->upload_count)
		return VK_ERROR_INITIALIZATION_FAILED;
	uint32_t uploads = s->upload_count ? s->upload_count :
					     damage.w > 0 && damage.h > 0;
	for (uint32_t i = 0; i < uploads; i++) {
		struct gpu_rect r = s->upload_count ? s->uploads[i].src :
						      damage;
		struct gpu_rect dst = s->upload_count ? s->uploads[i].dst :
							damage;
		if (!inside(r, s->width, s->height) ||
		    !inside(dst, s->width, s->height) || r.w != dst.w ||
		    r.h != dst.h)
			return VK_ERROR_INITIALIZATION_FAILED;
		const uint8_t *pixels = (const uint8_t *)s->pixels +
					(size_t)r.y * s->pitch + 4 * r.x;
		add_upload(&u, &c->canvas, dst, pixels, s->pitch,
			   (size_t)r.w * 4);
	}
	if (!s->copy_count)
		items[count++] = canvas_copy(s, full, full, c->canvas.slot);
	for (uint32_t i = 0; i < s->copy_count; i++) {
		const struct gpu_copy *copy = &s->copies[i];
		if (!inside(copy->src, s->width, s->height))
			return VK_ERROR_INITIALIZATION_FAILED;
		items[count++] =
			canvas_copy(s, copy->src, copy->dst, c->canvas.slot);
	}
	for (uint32_t i = 0; i < VIDEO_COUNT; i++) {
		const struct gpu_video *v = &s->video[i];
		struct video_textures *video = &c->video[i];
		if (!v->width && video->plane[0].image.handle) {
			result = retire(&u);
			if (result != VK_SUCCESS)
				return result;
			for (uint32_t j = 0; j < 3; j++)
				release_texture(c->ctx, &video->plane[j]);
		}
		if (!v->width || v->w <= 0 || v->h <= 0)
			continue;
		result = prepare_video(&u, video, v, &items[count++]);
		if (result != VK_SUCCESS)
			return result;
	}
	present_damage(c, s, next);
	result = vk_present_draw(
		c->present, items, count,
		(VkClearColorValue){ .float32 = { 0, 0, 0, 1 } },
		record_uploads, &u);
	/* An out-of-date presentation can follow a successful queue submission. */
	if (u.recorded) {
		for (uint32_t i = 0; i < u.count; i++)
			u.copies[i].texture->uploaded = true;
		for (uint32_t i = 0; i < VIDEO_COUNT; i++)
			if (s->video[i].width && s->video[i].w > 0 &&
			    s->video[i].h > 0)
				c->video[i].generation = s->video[i].generation;
	}
	c->presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
	if (c->presented) {
		memcpy(c->last_video, next, sizeof(next));
		memcpy(c->last_copies, s->copies,
		       s->copy_count * sizeof(s->copies[0]));
		c->last_count = s->copy_count;
	}
	return result;
}

struct vk_canvas *vk_canvas_open(struct vk_present *present)
{
	struct vk_canvas *c = calloc(1, sizeof(*c));
	if (c) {
		c->present = present;
		c->ctx = vk_present_context(present);
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(c->ctx->gpu, &props);
		c->atom = props.limits.nonCoherentAtomSize;
	}
	return c;
}

void vk_canvas_close(struct vk_canvas *c)
{
	if (!c)
		return;
	vk_present_wait(c->present, UINT64_MAX);
	release_texture(c->ctx, &c->canvas);
	for (uint32_t i = 0; i < VIDEO_COUNT; i++)
		for (uint32_t j = 0; j < 3; j++)
			release_texture(c->ctx, &c->video[i].plane[j]);
	for (uint32_t i = 0; i < VK_PRESENT_SLOTS; i++)
		vk_buffer_destroy(c->ctx, &c->staging[i].buffer);
	free(c);
}
