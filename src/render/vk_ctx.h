/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_VK_CTX_H
#define LINE_VK_CTX_H

#include "ui_batch.h"
#include <volk.h>
#include <stdbool.h>

struct vk_buffer {
	VkBuffer handle;
	VkDeviceMemory memory;
	VkDeviceAddress address;
	void *mapped;
	VkMemoryPropertyFlags properties;
};

struct vk_heap {
	struct vk_buffer buffer;
	VkBindHeapInfoEXT bind;
	uint32_t stride, descriptor_size;
};

struct vk_surface_config {
	const char *const *extensions;
	uint32_t extension_count;
	VkResult (*create)(VkInstance instance, void *window,
			   VkSurfaceKHR *surface);
	void *window;
};

/* volkLoadDevice uses global dispatch. Keep one context active per process. */
struct vk_ctx {
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice gpu;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkPhysicalDeviceMemoryProperties memory;
	VkImageLayout texture_layout;
	bool descriptor_heap, host_image_copy, incremental_present;
	struct vk_desc_buffer *descriptors;
	struct vk_heap resources, samplers;
	uint32_t free_head, free_next[UI_TEXTURE_SLOTS];
};

VkResult vk_ctx_init(struct vk_ctx *ctx, uint32_t vendor,
		     const struct vk_surface_config *surface);
void vk_ctx_destroy(struct vk_ctx *ctx);
uint32_t vk_memory_type(const struct vk_ctx *ctx, uint32_t bits,
			VkMemoryPropertyFlags required,
			VkMemoryPropertyFlags preferred);
VkResult vk_buffer_init(const struct vk_ctx *ctx, struct vk_buffer *buffer,
			VkDeviceSize size, VkBufferUsageFlags usage,
			VkMemoryPropertyFlags preferred);
void vk_buffer_destroy(const struct vk_ctx *ctx, struct vk_buffer *buffer);
VkResult vk_buffer_flush(const struct vk_ctx *ctx,
			 const struct vk_buffer *buffer);
VkResult vk_buffer_invalidate(const struct vk_ctx *ctx,
			      const struct vk_buffer *buffer);
uint32_t vk_image_slot_alloc(struct vk_ctx *ctx);
/* The caller must retire every draw using this slot before freeing it. */
void vk_image_slot_free(struct vk_ctx *ctx, uint32_t slot);
VkResult vk_image_slot_write(const struct vk_ctx *ctx, uint32_t slot,
			     const VkImageViewCreateInfo *view,
			     VkImageLayout layout);

#endif
