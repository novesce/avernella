// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_ctx.h"
#include "vk_adapter.h"
#include "vk_desc_buffer.h"
#include "vk_drm.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Only the chosen descriptor model enters the device feature chain. */
struct vk_features {
	VkPhysicalDeviceFeatures2 core;
	VkPhysicalDeviceVulkan12Features v12;
	VkPhysicalDeviceVulkan13Features v13;
	VkPhysicalDeviceVulkan14Features v14;
	VkPhysicalDeviceDescriptorHeapFeaturesEXT heap;
	VkPhysicalDeviceDescriptorBufferFeaturesEXT buffer;
};

static void feature_chain(struct vk_features *f, bool v14, bool heap,
			  bool buffer, void *tail)
{
	*f = (struct vk_features){
		.core = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			  .pNext = &f->v12 },
		.v12 = { .sType =
				 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			 .pNext = &f->v13 },
		.v13 = { .sType =
				 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES },
		.v14 = { .sType =
				 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES },
		.heap = { .sType =
				  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT },
		.buffer = { .sType =
				    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT },
	};
	if (buffer) {
		f->buffer.pNext = tail;
		tail = &f->buffer;
	}
	if (heap) {
		f->heap.pNext = tail;
		tail = &f->heap;
	}
	if (v14) {
		f->v14.pNext = tail;
		tail = &f->v14;
	}
	f->v13.pNext = tail;
}

struct device_caps {
	bool heap, buffer, drm, swapchain, incremental;
	bool timing, id2, calibrated;
	bool modifier, dmabuf, fd, foreign;
	unsigned maintenance, latest_ready;
};

enum maintenance {
	MAINTENANCE_KHR = 1,
	MAINTENANCE_EXT = 2,
};

/* Both instance extension names may be advertised on a hybrid system even
 * when one of its physical devices only has the original EXT device name.
 */
static VkResult create_instance(struct vk_ctx *ctx,
				const VkApplicationInfo *app,
				const struct vk_surface_config *surface,
				unsigned *maintenance)
{
	VkInstanceCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = app
	};
	if (!surface)
		return vkCreateInstance(&info, NULL, &ctx->instance);
	uint32_t count = 0;
	VkResult result =
		vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
	if (result != VK_SUCCESS)
		return result;
	VkExtensionProperties *available = calloc(count, sizeof(*available));
	const char **names =
		calloc((size_t)surface->extension_count + 3, sizeof(*names));
	if (!available || !names) {
		result = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto done;
	}
	result =
		vkEnumerateInstanceExtensionProperties(NULL, &count, available);
	if (result != VK_SUCCESS)
		goto done;
	memcpy(names, surface->extensions,
	       (size_t)surface->extension_count * sizeof(*names));
	uint32_t n = surface->extension_count;
	for (uint32_t i = 0; i < count; i++) {
		const char *name = available[i].extensionName;
		if (!strcmp(name,
			    VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
			names[n++] = name;
			*maintenance |= MAINTENANCE_KHR;
		} else if (!strcmp(name,
				   VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
			names[n++] = name;
			*maintenance |= MAINTENANCE_EXT;
		} else if (
			!strcmp(name,
				VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
			names[n++] = name;
		}
	}
	if (!*maintenance) {
		result = VK_ERROR_EXTENSION_NOT_PRESENT;
		goto done;
	}
	info.enabledExtensionCount = n;
	info.ppEnabledExtensionNames = names;
	result = vkCreateInstance(&info, NULL, &ctx->instance);
done:
	free(names);
	free(available);
	return result;
}

static bool read_extensions(VkPhysicalDevice gpu, struct device_caps *caps)
{
	uint32_t count = 0;

	if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL) !=
		    VK_SUCCESS ||
	    !count)
		return false;
	VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));

	if (extensions &&
	    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count,
						 extensions) == VK_SUCCESS) {
		for (uint32_t i = 0; i < count; i++) {
			const char *name = extensions[i].extensionName;

			caps->heap |= !strcmp(
				name, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
			caps->buffer |= !strcmp(
				name, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
			caps->drm |= !strcmp(
				name,
				VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
			caps->modifier |= !strcmp(
				name,
				VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
			caps->dmabuf |= !strcmp(
				name,
				VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
			caps->fd |= !strcmp(
				name, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
			caps->foreign |= !strcmp(
				name,
				VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
			caps->timing |= !strcmp(
				name, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
			caps->id2 |= !strcmp(
				name, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
			caps->calibrated |= !strcmp(
				name,
				VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
			caps->swapchain |=
				!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
			caps->incremental |= !strcmp(
				name,
				VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME);
			if (!strcmp(name,
				    VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
				caps->maintenance |= MAINTENANCE_KHR;
			else if (!strcmp(name,
					 VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
				caps->maintenance |= MAINTENANCE_EXT;
			else if (!strcmp(name,
					 VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME))
				caps->latest_ready |= MAINTENANCE_KHR;
			else if (!strcmp(name,
					 VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME))
				caps->latest_ready |= MAINTENANCE_EXT;
		}
	}
	free(extensions);
	return caps->heap || caps->buffer;
}

static uint32_t graphics_family(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
	uint32_t count = 0, family = UINT32_MAX;

	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
	VkQueueFamilyProperties *queues = calloc(count, sizeof(*queues));

	if (!queues)
		return family;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues);
	for (uint32_t i = 0; i < count; i++) {
		if (!queues[i].queueCount ||
		    !(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			continue;
		VkBool32 supported = VK_TRUE;

		if (surface &&
		    (vkGetPhysicalDeviceSurfaceSupportKHR(
			     gpu, i, surface, &supported) != VK_SUCCESS ||
		     !supported))
			continue;
		family = i;
		break;
	}
	free(queues);
	return family;
}

VkResult vk_surface_format(VkPhysicalDevice gpu, VkSurfaceKHR surface,
			   VkSurfaceFormatKHR *format)
{
	uint32_t count = 0;
	VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface,
							       &count, NULL);
	if (result != VK_SUCCESS || !count)
		return result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED :
					      result;
	VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));

	if (!formats)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count,
						      formats);
	if (result == VK_SUCCESS) {
		result = VK_ERROR_FORMAT_NOT_SUPPORTED;
		/* Cairo pixels are already encoded in the sRGB color space. */
		for (uint32_t i = 0; i < count; i++) {
			VkSurfaceFormatKHR candidate = formats[i];
			if (candidate.colorSpace !=
			    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				continue;
			if (count == 1 &&
			    candidate.format == VK_FORMAT_UNDEFINED)
				candidate.format = VK_FORMAT_B8G8R8A8_UNORM;
			if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
			    candidate.format == VK_FORMAT_R8G8B8A8_UNORM) {
				*format = candidate;
				result = VK_SUCCESS;
				break;
			}
		}
	}
	free(formats);
	return result;
}

/* FIFO_LATEST_READY is measured on NVIDIA's Wayland driver, where FIFO blocks
 * vkQueuePresentKHR for a refresh interval. AVERNELLA_PRESENT_MODE=fifo turns
 * it off, and =latest allows it on other drivers that offer it.
 */
static bool want_latest_ready(uint32_t vendor)
{
	const char *mode = getenv("AVERNELLA_PRESENT_MODE");

	if (mode && !strcmp(mode, "fifo"))
		return false;
	return vendor == 0x10de || (mode && !strcmp(mode, "latest"));
}

static void inspect_adapter(struct vk_adapter *adapter, VkPhysicalDevice gpu,
			    VkSurfaceKHR surface, uint32_t vendor,
			    unsigned maintenance, const char *render_node,
			    bool force_heap, bool force_buffer)
{
	VkPhysicalDeviceProperties properties;
	struct device_caps caps = { 0 };
	struct vk_features f;
	int render_minor;

	vkGetPhysicalDeviceProperties(gpu, &properties);
	bool v14 = properties.apiVersion >= VK_API_VERSION_1_4;
	bool cpu = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
	if (cpu && surface && !vendor && !force_heap && !force_buffer &&
	    (!render_node || !*render_node))
		return;
	if (properties.apiVersion < VK_API_VERSION_1_3 ||
	    (vendor && vendor != properties.vendorID) ||
	    !read_extensions(gpu, &caps) ||
	    !vk_drm_matches(gpu, render_node, caps.drm, &render_minor) ||
	    (surface && (!caps.swapchain || !(caps.maintenance & maintenance))))
		return;
	bool timing = caps.timing && caps.id2 && caps.calibrated;
	VkPhysicalDevicePresentTimingFeaturesEXT timings = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
	};
	VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR latest = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
		.pNext = timing ? &timings : NULL,
	};
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR present = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.pNext = caps.latest_ready ? (void *)&latest :
					     (timing ? &timings : NULL),
	};
	feature_chain(&f, v14, caps.heap, caps.buffer,
		      surface ? &present : NULL);
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
		.pNext = f.core.pNext,
	};
	f.core.pNext = &ycbcr;
	vkGetPhysicalDeviceFeatures2(gpu, &f.core);
	bool heap = v14 && caps.heap && f.heap.descriptorHeap && !force_buffer;
	VkPhysicalDeviceLimits *limits = &properties.limits;
	bool buffer = caps.buffer && f.buffer.descriptorBuffer && !force_heap &&
		      limits->maxPerStageDescriptorSampledImages >=
			      UI_TEXTURE_SLOTS &&
		      limits->maxDescriptorSetSampledImages >=
			      UI_TEXTURE_SLOTS &&
		      limits->maxPerStageResources >= UI_TEXTURE_SLOTS + 1;
	if ((!heap && !buffer) || !f.v12.bufferDeviceAddress ||
	    !f.v12.scalarBlockLayout ||
	    !f.v12.shaderSampledImageArrayNonUniformIndexing ||
	    !f.v12.timelineSemaphore || !f.v13.dynamicRendering ||
	    !f.v13.synchronization2 || !f.v13.shaderDemoteToHelperInvocation ||
	    (surface && !present.swapchainMaintenance1))
		return;
	uint32_t family = graphics_family(gpu, surface);
	if (family == UINT32_MAX)
		return;
	if (surface) {
		VkSurfaceCapabilitiesKHR usable;
		VkSurfaceFormatKHR format;

		if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
			    gpu, surface, &usable) != VK_SUCCESS ||
		    !(usable.supportedUsageFlags &
		      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
		    !usable.supportedCompositeAlpha ||
		    vk_surface_format(gpu, surface, &format) != VK_SUCCESS)
			return;
	}
	*adapter = (struct vk_adapter){
		.gpu = gpu,
		.type = properties.deviceType,
		.vendor = properties.vendorID,
		.family = family,
		.render_minor = render_minor,
		.heap = heap,
		.maintenance_khr = caps.maintenance & maintenance &
				   MAINTENANCE_KHR,
		.incremental = surface && caps.incremental,
		.host_copy = v14 && f.v14.hostImageCopy,
		.present_timing = surface && timings.presentTiming,
		.nv12 = ycbcr.samplerYcbcrConversion && caps.modifier &&
			caps.dmabuf && caps.fd && caps.foreign,
		.latest_ready =
			surface && latest.presentModeFifoLatestReady &&
					want_latest_ready(properties.vendorID) ?
				caps.latest_ready :
				0,
	};
}

uint32_t vk_memory_type(const struct vk_ctx *ctx, uint32_t bits,
			VkMemoryPropertyFlags required,
			VkMemoryPropertyFlags preferred)
{
	uint32_t type = UINT32_MAX;
	int best = -1;

	for (uint32_t i = 0; i < ctx->memory.memoryTypeCount; i++) {
		VkMemoryPropertyFlags flags =
			ctx->memory.memoryTypes[i].propertyFlags;

		if (!(bits & (1u << i)) || (flags & required) != required)
			continue;
		int score = __builtin_popcount(flags & preferred);

		if (score > best) {
			best = score;
			type = i;
		}
	}
	return type;
}

void vk_buffer_destroy(const struct vk_ctx *ctx, struct vk_buffer *buffer)
{
	if (buffer->mapped)
		vkUnmapMemory(ctx->device, buffer->memory);
	vkDestroyBuffer(ctx->device, buffer->handle, NULL);
	vkFreeMemory(ctx->device, buffer->memory, NULL);
	*buffer = (struct vk_buffer){ 0 };
}

VkResult vk_buffer_init(const struct vk_ctx *ctx, struct vk_buffer *buffer,
			VkDeviceSize size, VkBufferUsageFlags usage,
			VkMemoryPropertyFlags preferred)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
	};
	VkResult result =
		vkCreateBuffer(ctx->device, &info, NULL, &buffer->handle);

	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements req;

	vkGetBufferMemoryRequirements(ctx->device, buffer->handle, &req);
	uint32_t type = vk_memory_type(ctx, req.memoryTypeBits,
				       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				       preferred);
	if (type == UINT32_MAX) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
		goto fail;
	}
	VkMemoryAllocateFlagsInfo flags = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
		.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
	};
	bool address = usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocation = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = address ? &flags : NULL,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};

	result = vkAllocateMemory(ctx->device, &allocation, NULL,
				  &buffer->memory);
	if (result != VK_SUCCESS)
		goto fail;
	result = vkBindBufferMemory(ctx->device, buffer->handle, buffer->memory,
				    0);
	if (result != VK_SUCCESS)
		goto fail;
	result = vkMapMemory(ctx->device, buffer->memory, 0, VK_WHOLE_SIZE, 0,
			     &buffer->mapped);
	if (result != VK_SUCCESS)
		goto fail;
	buffer->properties = ctx->memory.memoryTypes[type].propertyFlags;
	if (address) {
		VkBufferDeviceAddressInfo addr = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = buffer->handle,
		};

		buffer->address = vkGetBufferDeviceAddress(ctx->device, &addr);
	}
	return VK_SUCCESS;
fail:
	vk_buffer_destroy(ctx, buffer);
	return result;
}

static VkResult mapped_sync(const struct vk_ctx *ctx,
			    const struct vk_buffer *buffer, bool flush)
{
	if (buffer->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		return VK_SUCCESS;
	VkMappedMemoryRange range = {
		.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
		.memory = buffer->memory,
		.size = VK_WHOLE_SIZE,
	};

	return flush ? vkFlushMappedMemoryRanges(ctx->device, 1, &range) :
		       vkInvalidateMappedMemoryRanges(ctx->device, 1, &range);
}

VkResult vk_buffer_flush(const struct vk_ctx *ctx,
			 const struct vk_buffer *buffer)
{
	return mapped_sync(ctx, buffer, true);
}

VkResult vk_buffer_invalidate(const struct vk_ctx *ctx,
			      const struct vk_buffer *buffer)
{
	return mapped_sync(ctx, buffer, false);
}

static VkDeviceSize align_up(VkDeviceSize size, VkDeviceSize alignment)
{
	return (size + alignment - 1) & ~(alignment - 1);
}

static VkResult heap_init(struct vk_ctx *ctx, struct vk_heap *heap,
			  VkDeviceSize alignment, VkDeviceSize descriptor_align,
			  VkDeviceSize descriptor_size, uint32_t slots,
			  VkDeviceSize reserved, VkDeviceSize limit)
{
	VkDeviceSize stride = align_up(descriptor_size, descriptor_align);
	VkDeviceSize first_reserved = stride * slots;
	VkDeviceSize size = first_reserved + reserved;

	if (size > limit || stride > UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	VkResult result = vk_buffer_init(
		ctx, &heap->buffer, size + alignment - 1,
		VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT |
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	if (result != VK_SUCCESS)
		return result;
	heap->stride = (uint32_t)stride;
	heap->descriptor_size = (uint32_t)descriptor_size;
	heap->bind = (VkBindHeapInfoEXT){
		.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
		.heapRange = { align_up(heap->buffer.address, alignment),
			       size },
		.reservedRangeOffset = reserved ? first_reserved : 0,
		.reservedRangeSize = reserved,
	};
	return VK_SUCCESS;
}

static VkHostAddressRangeEXT descriptor_at(const struct vk_heap *heap,
					   uint32_t slot, VkDeviceSize size)
{
	VkDeviceSize offset =
		heap->bind.heapRange.address - heap->buffer.address;

	return (VkHostAddressRangeEXT){
		.address = (char *)heap->buffer.mapped + offset +
			   (size_t)slot * heap->stride,
		.size = size,
	};
}

static VkResult init_heaps(struct vk_ctx *ctx)
{
	VkPhysicalDeviceDescriptorHeapPropertiesEXT heap = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &heap,
	};

	vkGetPhysicalDeviceProperties2(ctx->gpu, &properties);
	if (heap.maxPushDataSize < sizeof(struct ui_push))
		return VK_ERROR_FEATURE_NOT_PRESENT;
	VkDeviceSize resource_align = heap.imageDescriptorAlignment;

	if (heap.bufferDescriptorAlignment > resource_align)
		resource_align = heap.bufferDescriptorAlignment;
	VkResult result = heap_init(ctx, &ctx->resources,
				    heap.resourceHeapAlignment, resource_align,
				    heap.imageDescriptorSize, UI_TEXTURE_SLOTS,
				    heap.minResourceHeapReservedRange,
				    heap.maxResourceHeapSize);

	if (result != VK_SUCCESS)
		return result;
	result = heap_init(ctx, &ctx->samplers, heap.samplerHeapAlignment,
			   heap.samplerDescriptorAlignment,
			   heap.samplerDescriptorSize, UI_SAMPLER_SLOTS,
			   heap.minSamplerHeapReservedRange,
			   heap.maxSamplerHeapSize);
	if (result != VK_SUCCESS)
		return result;
	VkSamplerCreateInfo samplers[UI_SAMPLER_SLOTS];
	VkHostAddressRangeEXT destinations[UI_SAMPLER_SLOTS];

	for (uint32_t i = 0; i < UI_SAMPLER_SLOTS; i++) {
		VkFilter filter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

		samplers[i] = (VkSamplerCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = filter,
			.minFilter = filter,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		destinations[i] = descriptor_at(&ctx->samplers, i,
						heap.samplerDescriptorSize);
	}
	result = vkWriteSamplerDescriptorsEXT(ctx->device, UI_SAMPLER_SLOTS,
					      samplers, destinations);
	if (result == VK_SUCCESS)
		result = vk_buffer_flush(ctx, &ctx->samplers.buffer);
	return result;
}

static VkResult init_texture_layout(struct vk_ctx *ctx)
{
	VkPhysicalDeviceHostImageCopyProperties host = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &host,
	};

	vkGetPhysicalDeviceProperties2(ctx->gpu, &properties);
	if (!host.copyDstLayoutCount)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkImageLayout *layouts =
		calloc(host.copyDstLayoutCount, sizeof(*layouts));

	if (!layouts)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	host.pCopyDstLayouts = layouts;
	vkGetPhysicalDeviceProperties2(ctx->gpu, &properties);
	for (uint32_t i = 0; i < host.copyDstLayoutCount; i++) {
		if (layouts[i] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
			ctx->texture_layout = layouts[i];
			break;
		}
		if (layouts[i] == VK_IMAGE_LAYOUT_GENERAL)
			ctx->texture_layout = layouts[i];
	}
	free(layouts);
	return ctx->texture_layout == VK_IMAGE_LAYOUT_UNDEFINED ?
		       VK_ERROR_FORMAT_NOT_SUPPORTED :
		       VK_SUCCESS;
}

uint32_t vk_image_slot_alloc(struct vk_ctx *ctx)
{
	uint32_t slot = ctx->free_head;

	if (slot == UI_TEXTURE_SLOTS)
		return UINT32_MAX;
	ctx->free_head = ctx->free_next[slot];
	ctx->free_next[slot] = UINT32_MAX;
	return slot;
}

void vk_image_slot_free(struct vk_ctx *ctx, uint32_t slot)
{
	if (!slot || slot >= UI_TEXTURE_SLOTS ||
	    ctx->free_next[slot] != UINT32_MAX)
		return;
	if (ctx->descriptors) {
		vkDestroyImageView(ctx->device, ctx->descriptors->view[slot],
				   NULL);
		ctx->descriptors->view[slot] = VK_NULL_HANDLE;
	}
	ctx->free_next[slot] = ctx->free_head;
	ctx->free_head = slot;
}

VkResult vk_image_slot_write(const struct vk_ctx *ctx, uint32_t slot,
			     const VkImageViewCreateInfo *view,
			     VkImageLayout layout)
{
	if (slot >= UI_TEXTURE_SLOTS ||
	    (slot && ctx->free_next[slot] != UINT32_MAX))
		return VK_ERROR_INITIALIZATION_FAILED;
	if (ctx->descriptors)
		return vk_desc_buffer_write(ctx, slot, view, layout);
	VkImageDescriptorInfoEXT image = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
		.pView = view,
		.layout = layout,
	};
	VkResourceDescriptorInfoEXT resource = {
		.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
		.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		.data.pImage = &image,
	};
	VkHostAddressRangeEXT destination = descriptor_at(
		&ctx->resources, slot, ctx->resources.descriptor_size);
	VkResult result = vkWriteResourceDescriptorsEXT(
		ctx->device, 1, &resource, &destination);

	return result == VK_SUCCESS ?
		       vk_buffer_flush(ctx, &ctx->resources.buffer) :
		       result;
}

static VkResult open_device(struct vk_ctx *ctx,
			    const struct vk_adapter *adapter)
{
	ctx->gpu = adapter->gpu;
	ctx->family = adapter->family;
	ctx->render_minor = adapter->render_minor;
	ctx->descriptor_heap = adapter->heap;
	ctx->incremental_present = adapter->incremental;
	ctx->host_image_copy = adapter->host_copy;
	ctx->fifo_latest_ready = adapter->latest_ready;
	ctx->present_timing = adapter->present_timing;
	ctx->nv12 = adapter->nv12;
	VkPhysicalDevicePresentTimingFeaturesEXT timings = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
		.presentTiming = VK_TRUE,
	};
	struct vk_features features;

	VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR latest = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
		.pNext = ctx->present_timing ? &timings : NULL,
		.presentModeFifoLatestReady = VK_TRUE,
	};
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.pNext = ctx->fifo_latest_ready ?
				 (void *)&latest :
				 (ctx->present_timing ? &timings : NULL),
		.swapchainMaintenance1 = VK_TRUE,
	};
	feature_chain(&features, ctx->host_image_copy || ctx->descriptor_heap,
		      ctx->descriptor_heap, !ctx->descriptor_heap,
		      ctx->surface ? &maintenance : NULL);
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
		.pNext = features.core.pNext,
		.samplerYcbcrConversion = ctx->nv12,
	};
	features.core.pNext = &ycbcr;
	features.v12.bufferDeviceAddress = VK_TRUE;
	features.v12.scalarBlockLayout = VK_TRUE;
	features.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	features.v12.timelineSemaphore = VK_TRUE;
	features.v13.dynamicRendering = VK_TRUE;
	features.v13.synchronization2 = VK_TRUE;
	features.v13.shaderDemoteToHelperInvocation = VK_TRUE;
	features.v14.hostImageCopy = ctx->host_image_copy;
	features.heap.descriptorHeap = ctx->descriptor_heap;
	features.buffer.descriptorBuffer = !ctx->descriptor_heap;
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = ctx->family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	const char *extensions[12] = {
		ctx->descriptor_heap ? VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME :
				       VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
	};
	uint32_t count = 1;

	if (ctx->surface) {
		extensions[count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		extensions[count++] =
			adapter->maintenance_khr ?
				VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME :
				VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
	}
	if (ctx->nv12) {
		extensions[count++] =
			VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
		extensions[count++] =
			VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
		extensions[count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
		extensions[count++] =
			VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
	}
	if (ctx->present_timing) {
		extensions[count++] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;
		extensions[count++] =
			VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
		extensions[count++] = VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
	}
	if (ctx->incremental_present)
		extensions[count++] = VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME;
	if (ctx->fifo_latest_ready)
		extensions[count++] =
			adapter->latest_ready & MAINTENANCE_KHR ?
				VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME :
				VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME;
	VkDeviceCreateInfo device = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features.core,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue,
		.enabledExtensionCount = count,
		.ppEnabledExtensionNames = extensions,
	};

	VkDevice handle;
	VkResult result = vkCreateDevice(ctx->gpu, &device, NULL, &handle);
	if (result != VK_SUCCESS)
		return result;
	ctx->device = handle;
	volkLoadDevice(ctx->device);
	vkGetDeviceQueue(ctx->device, ctx->family, 0, &ctx->queue);
	vkGetPhysicalDeviceMemoryProperties(ctx->gpu, &ctx->memory);
	if (ctx->host_image_copy) {
		result = init_texture_layout(ctx);
		if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
			ctx->host_image_copy = false;
		else if (result != VK_SUCCESS)
			return result;
	}
	if (!ctx->host_image_copy)
		ctx->texture_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	ctx->free_head = 1; /* zero is the persistent glyph atlas */
	for (uint32_t i = 1; i < UI_TEXTURE_SLOTS; i++)
		ctx->free_next[i] = i + 1;
	return ctx->descriptor_heap ? init_heaps(ctx) :
				      vk_desc_buffer_open(ctx);
}

static void close_device(struct vk_ctx *ctx)
{
	if (ctx->device) {
		vkDeviceWaitIdle(ctx->device);
		vk_desc_buffer_close(ctx);
		vk_buffer_destroy(ctx, &ctx->samplers.buffer);
		vk_buffer_destroy(ctx, &ctx->resources.buffer);
		vkDestroyDevice(ctx->device, NULL);
	}
	/* The instance and its window surface survive an adapter retry. */
	*ctx = (struct vk_ctx){ .instance = ctx->instance,
				.surface = ctx->surface };
}

static VkResult open_adapter(struct vk_ctx *ctx, uint32_t vendor,
			     unsigned maintenance)
{
	const char *mode = getenv("AVERNELLA_VK_DESCRIPTORS");
	const char *node = getenv("AVERNELLA_RENDER_NODE");
	bool force_heap = mode && !strcmp(mode, "heap");
	bool force_buffer = mode && !strcmp(mode, "buffer");
	uint32_t count = 0;
	VkResult result =
		vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);

	if (result != VK_SUCCESS || !count)
		return result != VK_SUCCESS ? result :
					      VK_ERROR_FEATURE_NOT_PRESENT;
	VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
	struct vk_adapter *adapters = calloc(count, sizeof(*adapters));

	if (!devices || !adapters) {
		result = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto done;
	}
	result = vkEnumeratePhysicalDevices(ctx->instance, &count, devices);
	if (result != VK_SUCCESS)
		goto done;
	for (uint32_t i = 0; i < count; i++)
		inspect_adapter(&adapters[i], devices[i], ctx->surface, vendor,
				maintenance, node, force_heap, force_buffer);
	result = VK_ERROR_FEATURE_NOT_PRESENT;
	struct vk_adapter *adapter;

	while ((adapter = vk_preferred_adapter(adapters, count))) {
		result = open_device(ctx, adapter);
		if (result == VK_SUCCESS)
			break;
		close_device(ctx);
		if (result == VK_ERROR_OUT_OF_HOST_MEMORY)
			break;
		adapter->gpu = VK_NULL_HANDLE;
	}
done:
	free(adapters);
	free(devices);
	return result;
}

VkResult vk_ctx_init(struct vk_ctx *ctx, uint32_t vendor,
		     const struct vk_surface_config *surface)
{
	*ctx = (struct vk_ctx){ 0 };
	VkResult result = volkInitialize();

	if (result != VK_SUCCESS)
		return result;
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "Avernella",
		.apiVersion = volkGetInstanceVersion() >= VK_API_VERSION_1_4 ?
				      VK_API_VERSION_1_4 :
				      VK_API_VERSION_1_3,
	};
	unsigned instance_maintenance = 0;

	if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
		result = VK_ERROR_INCOMPATIBLE_DRIVER;
		goto fail;
	}
	result = create_instance(ctx, &app, surface, &instance_maintenance);
	if (result != VK_SUCCESS)
		goto fail;
	volkLoadInstanceOnly(ctx->instance);
	if (surface) {
		result = surface->create(ctx->instance, surface->window,
					 &ctx->surface);
		if (result != VK_SUCCESS)
			goto fail;
	}
	result = open_adapter(ctx, vendor, instance_maintenance);
	if (result == VK_SUCCESS)
		return result;
fail:
	vk_ctx_destroy(ctx);
	return result;
}

void vk_ctx_destroy(struct vk_ctx *ctx)
{
	close_device(ctx);
	if (ctx->surface)
		vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
	if (ctx->instance)
		vkDestroyInstance(ctx->instance, NULL);
	volkFinalize();
	*ctx = (struct vk_ctx){ 0 };
}
