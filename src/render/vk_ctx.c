// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_ctx.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Include only features used by the descriptor-heap renderer. */
struct vk_features {
	VkPhysicalDeviceFeatures2 core;
	VkPhysicalDeviceVulkan12Features v12;
	VkPhysicalDeviceVulkan13Features v13;
	VkPhysicalDeviceVulkan14Features v14;
	VkPhysicalDeviceDescriptorHeapFeaturesEXT heap;
};

static void feature_chain(struct vk_features *f)
{
	*f = (struct vk_features){
		.core = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			  .pNext = &f->v12 },
		.v12 = { .sType =
				 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			 .pNext = &f->v13 },
		.v13 = { .sType =
				 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			 .pNext = &f->v14 },
		.v14 = { .sType =
				 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
			 .pNext = &f->heap },
		.heap = { .sType =
				  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT },
	};
}

static bool has_extensions(VkPhysicalDevice gpu, bool present)
{
	uint32_t count = 0;

	if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL) !=
		    VK_SUCCESS ||
	    !count)
		return false;
	VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
	bool heap = false, swapchain = false, maintenance = false;

	if (extensions &&
	    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count,
						 extensions) == VK_SUCCESS) {
		for (uint32_t i = 0; i < count; i++) {
			const char *name = extensions[i].extensionName;

			heap |= !strcmp(name,
					VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
			swapchain |=
				!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
			maintenance |= !strcmp(
				name,
				VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		}
	}
	free(extensions);
	return heap && (!present || (swapchain && maintenance));
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

static VkResult pick_gpu(struct vk_ctx *ctx, uint32_t vendor)
{
	uint32_t count = 0;
	VkResult result =
		vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);

	if (result != VK_SUCCESS)
		return result;
	if (!count)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkPhysicalDevice *devices = calloc(count, sizeof(*devices));

	if (!devices)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	result = vkEnumeratePhysicalDevices(ctx->instance, &count, devices);
	if (result == VK_SUCCESS) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
		for (uint32_t i = 0; i < count; i++) {
			VkPhysicalDeviceProperties properties;
			struct vk_features features;

			vkGetPhysicalDeviceProperties(devices[i], &properties);
			if (properties.apiVersion < VK_API_VERSION_1_4 ||
			    (vendor && vendor != properties.vendorID) ||
			    !has_extensions(devices[i],
					    ctx->surface != VK_NULL_HANDLE))
				continue;
			VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance = {
				.sType =
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
			};
			feature_chain(&features);
			if (ctx->surface)
				features.heap.pNext = &maintenance;
			vkGetPhysicalDeviceFeatures2(devices[i],
						     &features.core);
			if (!features.heap.descriptorHeap ||
			    !features.v12.bufferDeviceAddress ||
			    !features.v12.scalarBlockLayout ||
			    !features.v12
				     .shaderSampledImageArrayNonUniformIndexing ||
			    !features.v12.timelineSemaphore ||
			    !features.v13.dynamicRendering ||
			    !features.v13.synchronization2 ||
			    !features.v13.shaderDemoteToHelperInvocation ||
			    !features.v14.hostImageCopy ||
			    (ctx->surface &&
			     !maintenance.swapchainMaintenance1))
				continue;
			uint32_t family =
				graphics_family(devices[i], ctx->surface);

			if (family == UINT32_MAX)
				continue;
			ctx->gpu = devices[i];
			ctx->family = family;
			result = VK_SUCCESS;
			break;
		}
	}
	free(devices);
	return result;
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
	ctx->free_head = 1; /* zero is the persistent glyph atlas */
	for (uint32_t i = 1; i < UI_TEXTURE_SLOTS; i++)
		ctx->free_next[i] = i + 1;
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

VkResult vk_ctx_init(struct vk_ctx *ctx, uint32_t vendor,
		     const struct vk_surface_config *surface)
{
	*ctx = (struct vk_ctx){ 0 };
	VkResult result = volkInitialize();

	if (result != VK_SUCCESS)
		return result;
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "LINE Vulkan renderer",
		.apiVersion = VK_API_VERSION_1_4,
	};
	VkInstanceCreateInfo instance = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	if (surface) {
		instance.enabledExtensionCount = surface->extension_count;
		instance.ppEnabledExtensionNames = surface->extensions;
	}

	if (volkGetInstanceVersion() < VK_API_VERSION_1_4) {
		result = VK_ERROR_INCOMPATIBLE_DRIVER;
		goto fail;
	}
	result = vkCreateInstance(&instance, NULL, &ctx->instance);
	if (result != VK_SUCCESS)
		goto fail;
	volkLoadInstanceOnly(ctx->instance);
	if (surface) {
		result = surface->create(ctx->instance, surface->window,
					 &ctx->surface);
		if (result != VK_SUCCESS)
			goto fail;
	}
	result = pick_gpu(ctx, vendor);
	if (result != VK_SUCCESS)
		goto fail;
	struct vk_features features;

	feature_chain(&features);
	features.v12.bufferDeviceAddress = VK_TRUE;
	features.v12.scalarBlockLayout = VK_TRUE;
	features.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	features.v12.timelineSemaphore = VK_TRUE;
	features.v13.dynamicRendering = VK_TRUE;
	features.v13.synchronization2 = VK_TRUE;
	features.v13.shaderDemoteToHelperInvocation = VK_TRUE;
	features.v14.hostImageCopy = VK_TRUE;
	features.heap.descriptorHeap = VK_TRUE;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.swapchainMaintenance1 = VK_TRUE,
	};
	if (surface)
		features.heap.pNext = &maintenance;
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = ctx->family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	const char *extensions[] = {
		VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
	};
	VkDeviceCreateInfo device = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features.core,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue,
		.enabledExtensionCount = surface ? 3 : 1,
		.ppEnabledExtensionNames = extensions,
	};

	result = vkCreateDevice(ctx->gpu, &device, NULL, &ctx->device);
	if (result != VK_SUCCESS)
		goto fail;
	volkLoadDevice(ctx->device);
	vkGetDeviceQueue(ctx->device, ctx->family, 0, &ctx->queue);
	vkGetPhysicalDeviceMemoryProperties(ctx->gpu, &ctx->memory);
	result = init_texture_layout(ctx);
	if (result == VK_SUCCESS)
		result = init_heaps(ctx);
	if (result == VK_SUCCESS)
		return result;
fail:
	vk_ctx_destroy(ctx);
	return result;
}

void vk_ctx_destroy(struct vk_ctx *ctx)
{
	if (ctx->device) {
		vkDeviceWaitIdle(ctx->device);
		vk_buffer_destroy(ctx, &ctx->samplers.buffer);
		vk_buffer_destroy(ctx, &ctx->resources.buffer);
		vkDestroyDevice(ctx->device, NULL);
	}
	if (ctx->surface)
		vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
	if (ctx->instance)
		vkDestroyInstance(ctx->instance, NULL);
	volkFinalize();
	*ctx = (struct vk_ctx){ 0 };
}
