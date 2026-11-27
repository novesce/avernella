// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vk_nv12.h"
#include "vk_frame.h"
#include "vk_present.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FORMAT VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define HANDLE VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define SURFACES (3 * (VK_PRESENT_SLOTS + 1))

struct color_pipeline {
	VkSamplerYcbcrConversion conversion;
	VkSampler sampler;
	VkDescriptorSetLayout descriptors;
	VkPipelineLayout layout;
	VkPipeline pipeline;
};

struct vk_nv12_image {
	struct color_pipeline *color;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	uint64_t serial;
	dev_t dev;
	ino_t ino;
	struct gpu_dmabuf desc;
};

struct vk_nv12 {
	const struct vk_ctx *ctx;
	VkDrmFormatModifierPropertiesEXT *mods;
	uint32_t count, max_descriptors;
	VkFormat target;
	struct color_pipeline colors[2];
	struct vk_nv12_image images[SURFACES];
};

static void drop_lease(struct vk_nv12_image *i)
{
	if (i->desc.lease) {
		i->desc.release(i->desc.lease);
		i->desc.lease = NULL;
	}
}

static void destroy_image(VkDevice device, struct vk_nv12_image *i)
{
	vkDestroyDescriptorPool(device, i->pool, NULL);
	vkDestroyImageView(device, i->view, NULL);
	vkDestroyImage(device, i->image, NULL);
	vkFreeMemory(device, i->memory, NULL);
	drop_lease(i);
	*i = (struct vk_nv12_image){ 0 };
}

static void destroy_colors(struct vk_nv12 *n)
{
	VkDevice d = n->ctx->device;

	for (unsigned int j = 0; j < 2; j++) {
		struct color_pipeline *c = &n->colors[j];

		vkDestroyPipeline(d, c->pipeline, NULL);
		vkDestroyPipelineLayout(d, c->layout, NULL);
		vkDestroyDescriptorSetLayout(d, c->descriptors, NULL);
		vkDestroySampler(d, c->sampler, NULL);
		vkDestroySamplerYcbcrConversion(d, c->conversion, NULL);
		*c = (struct color_pipeline){ 0 };
	}
}

struct vk_nv12 *vk_nv12_open(const struct vk_ctx *ctx)
{
	if (!ctx->nv12)
		return NULL;
	struct vk_nv12 *n = calloc(1, sizeof(*n));

	if (!n)
		return NULL;
	n->ctx = ctx;
	VkPhysicalDeviceProperties props0;

	vkGetPhysicalDeviceProperties(ctx->gpu, &props0);
	if (props0.apiVersion >= VK_API_VERSION_1_4) {
		VkPhysicalDeviceMaintenance6Properties limits = {
			.sType =
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES,
		};
		VkPhysicalDeviceProperties2 properties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &limits,
		};
		vkGetPhysicalDeviceProperties2(ctx->gpu, &properties);
		n->max_descriptors =
			limits.maxCombinedImageSamplerDescriptorCount;
	}
	VkDrmFormatModifierPropertiesListEXT list = {
		.sType =
			VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list
	};
	vkGetPhysicalDeviceFormatProperties2(ctx->gpu, FORMAT, &props);
	n->mods = calloc(list.drmFormatModifierCount, sizeof(*n->mods));
	if (!n->mods || !list.drmFormatModifierCount)
		goto unsupported;
	list.pDrmFormatModifierProperties = n->mods;
	vkGetPhysicalDeviceFormatProperties2(ctx->gpu, FORMAT, &props);
	const VkFormatFeatureFlags features =
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT |
		VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
	for (unsigned int j = 0; j < list.drmFormatModifierCount; j++)
		if (n->mods[j].drmFormatModifierPlaneCount == 2 &&
		    (n->mods[j].drmFormatModifierTilingFeatures & features) ==
			    features)
			n->mods[n->count++] = n->mods[j];
	if (n->count)
		return n;
unsupported:
	free(n->mods);
	free(n);
	return NULL;
}

void vk_nv12_close(struct vk_nv12 *n)
{
	if (!n)
		return;
	for (unsigned int j = 0; j < SURFACES; j++)
		destroy_image(n->ctx->device, &n->images[j]);
	destroy_colors(n);
	free(n->mods);
	free(n);
}

static VkResult create_color(struct vk_nv12 *n, bool full)
{
	struct color_pipeline *c = &n->colors[full];

	if (c->pipeline)
		return VK_SUCCESS;
	VkDevice d = n->ctx->device;
	VkSamplerYcbcrConversionCreateInfo conversion = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.format = FORMAT,
		.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
		.ycbcrRange = full ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL :
				     VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
		.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
		.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
		.chromaFilter = VK_FILTER_LINEAR,
	};
	VkResult r = vkCreateSamplerYcbcrConversion(d, &conversion, NULL,
						    &c->conversion);
	if (r != VK_SUCCESS)
		return r;
	VkSamplerYcbcrConversionInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = c->conversion,
	};
	VkSamplerCreateInfo sampler = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &ci,
		.minFilter = VK_FILTER_LINEAR,
		.magFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	r = vkCreateSampler(d, &sampler, NULL, &c->sampler);
	if (r != VK_SUCCESS)
		goto fail;
	VkDescriptorSetLayoutBinding binding = {
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.pImmutableSamplers = &c->sampler,
	};
	VkDescriptorSetLayoutCreateInfo descriptors = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	r = vkCreateDescriptorSetLayout(d, &descriptors, NULL, &c->descriptors);
	if (r != VK_SUCCESS)
		goto fail;
	VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
				     .size = sizeof(struct ui_push) };
	VkPipelineLayoutCreateInfo layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &c->descriptors,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	r = vkCreatePipelineLayout(d, &layout, NULL, &c->layout);
	if (r == VK_SUCCESS)
		r = vk_ui_nv12_pipeline(n->ctx, n->target, c->layout,
					&c->pipeline);
	if (r == VK_SUCCESS)
		return r;
fail:
	vkDestroyPipelineLayout(d, c->layout, NULL);
	vkDestroyDescriptorSetLayout(d, c->descriptors, NULL);
	vkDestroySampler(d, c->sampler, NULL);
	vkDestroySamplerYcbcrConversion(d, c->conversion, NULL);
	*c = (struct color_pipeline){ 0 };
	return r;
}

static VkResult image_support(struct vk_nv12 *n, const struct gpu_dmabuf *f,
			      uint32_t *descriptors)
{
	for (uint32_t j = 0;; j++) {
		if (j == n->count)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
		if (n->mods[j].drmFormatModifier == f->modifier)
			break;
	}
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.drmFormatModifier = f->modifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkPhysicalDeviceExternalImageFormatInfo ext = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &modifier,
		.handleType = HANDLE,
	};
	VkPhysicalDeviceImageFormatInfo2 image = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &ext,
		.format = FORMAT,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};
	VkSamplerYcbcrConversionImageFormatProperties conversion = {
		.sType =
			VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES,
	};
	VkExternalImageFormatProperties external = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
		.pNext = &conversion,
	};
	VkImageFormatProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &external,
	};
	VkResult r = vkGetPhysicalDeviceImageFormatProperties2(n->ctx->gpu,
							       &image, &props);
	if (r != VK_SUCCESS)
		return r;
	if (!(external.externalMemoryProperties.externalMemoryFeatures &
	      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
	    f->width > props.imageFormatProperties.maxExtent.width ||
	    f->height > props.imageFormatProperties.maxExtent.height ||
	    !conversion.combinedImageSamplerDescriptorCount)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	*descriptors = conversion.combinedImageSamplerDescriptorCount;
	if (*descriptors < n->max_descriptors)
		*descriptors = n->max_descriptors;
	return VK_SUCCESS;
}

static VkResult import_image(struct vk_nv12 *n, struct vk_nv12_image *i,
			     const struct gpu_dmabuf *f, VkDeviceSize fd_size)
{
	VkDevice d = n->ctx->device;
	uint32_t descriptors;
	VkResult r = image_support(n, f, &descriptors);

	if (r != VK_SUCCESS)
		return r;
	r = create_color(n, f->full_range);
	if (r != VK_SUCCESS)
		return r;
	i->color = &n->colors[f->full_range];
	VkSubresourceLayout planes[2] = {
		{ .offset = f->offset[0], .rowPitch = f->stride[0] },
		{ .offset = f->offset[1], .rowPitch = f->stride[1] },
	};
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
		.sType =
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier = f->modifier,
		.drmFormatModifierPlaneCount = 2,
		.pPlaneLayouts = planes,
	};
	VkExternalMemoryImageCreateInfo ext = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &modifier,
		.handleTypes = HANDLE,
	};
	VkImageCreateInfo image = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FORMAT,
		.extent = { f->width, f->height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	r = vkCreateImage(d, &image, NULL, &i->image);
	if (r != VK_SUCCESS)
		return r;
	VkMemoryRequirements req;

	vkGetImageMemoryRequirements(d, i->image, &req);
	VkMemoryFdPropertiesKHR fd = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
	};
	r = vkGetMemoryFdPropertiesKHR(d, HANDLE, f->fd, &fd);
	if (r != VK_SUCCESS)
		return r;
	uint32_t type = vk_memory_type(n->ctx,
				       fd.memoryTypeBits & req.memoryTypeBits,
				       0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX || req.size > fd_size)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	VkMemoryDedicatedAllocateInfo dedicated = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = i->image,
	};
	VkImportMemoryFdInfoKHR import = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = &dedicated,
		.handleType = HANDLE,
		.fd = fcntl(f->fd, F_DUPFD_CLOEXEC, 0),
	};
	if (import.fd < 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	r = vkAllocateMemory(d, &alloc, NULL, &i->memory);
	if (r != VK_SUCCESS) {
		close(import.fd);
		return r;
	}
	r = vkBindImageMemory(d, i->image, i->memory, 0);
	if (r != VK_SUCCESS)
		return r;
	VkSamplerYcbcrConversionInfo ycbcr = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = i->color->conversion,
	};
	VkImageViewCreateInfo view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.pNext = &ycbcr,
		.image = i->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = FORMAT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	r = vkCreateImageView(d, &view, NULL, &i->view);
	if (r != VK_SUCCESS)
		return r;
	VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				      descriptors };
	VkDescriptorPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	r = vkCreateDescriptorPool(d, &pool, NULL, &i->pool);
	if (r != VK_SUCCESS)
		return r;
	VkDescriptorSetAllocateInfo set = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = i->pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &i->color->descriptors,
	};
	r = vkAllocateDescriptorSets(d, &set, &i->set);
	if (r != VK_SUCCESS)
		return r;
	VkDescriptorImageInfo sample = {
		.imageView = i->view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = i->set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &sample,
	};
	vkUpdateDescriptorSets(d, 1, &write, 0, NULL);
	return VK_SUCCESS;
}

static bool same_storage(const struct vk_nv12_image *i,
			 const struct gpu_dmabuf *f, const struct stat *st)
{
	return i->image && i->dev == st->st_dev && i->ino == st->st_ino &&
	       i->desc.width == f->width && i->desc.height == f->height &&
	       i->desc.modifier == f->modifier &&
	       i->desc.full_range == f->full_range &&
	       !memcmp(i->desc.offset, f->offset, sizeof(f->offset)) &&
	       !memcmp(i->desc.stride, f->stride, sizeof(f->stride));
}

static VkResult prepare_image(struct vk_nv12 *n, const struct gpu_dmabuf *f,
			      struct vk_nv12_image **image)
{
	/* The caller holds its stable frame lease throughout draw. A
	 * repeated presentation of the same frame needs no descriptor I/O.
	 */
	if (f->lease)
		for (unsigned int j = 0; j < SURFACES; j++)
			if (n->images[j].desc.lease == f->lease) {
				*image = &n->images[j];
				return VK_SUCCESS;
			}
	struct stat st;

	if (!f->lease || !f->retain || !f->release || !f->width || !f->height ||
	    ((f->width | f->height) & 1) || f->fd < 0 || fstat(f->fd, &st) ||
	    st.st_size <= 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	for (unsigned int plane = 0; plane < 2; plane++) {
		uint32_t rows = f->height >> plane;
		uint64_t end = f->offset[plane] +
			       (uint64_t)f->stride[plane] * (rows - 1) +
			       f->width;

		if (f->stride[plane] < f->width || end > (uint64_t)st.st_size)
			return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	struct vk_nv12_image *slot = NULL;

	for (unsigned int j = 0; j < SURFACES; j++) {
		struct vk_nv12_image *i = &n->images[j];

		if (i->desc.lease)
			continue;
		if (same_storage(i, f, &st)) {
			slot = i;
			break;
		}
		if (!slot || !i->image ||
		    (slot->image && i->serial < slot->serial))
			slot = i;
	}
	if (!slot)
		return VK_NOT_READY;
	if (!same_storage(slot, f, &st)) {
		destroy_image(n->ctx->device, slot);
		VkResult r = import_image(n, slot, f, st.st_size);

		if (r != VK_SUCCESS) {
			destroy_image(n->ctx->device, slot);
			return r;
		}
		slot->dev = st.st_dev;
		slot->ino = st.st_ino;
	}
	slot->desc = *f;
	f->retain(f->lease);
	*image = slot;
	return VK_SUCCESS;
}

VkResult vk_nv12_prepare(struct vk_nv12 *n, VkFormat target,
			 const struct gpu_dmabuf *const frames[3],
			 uint64_t completed, struct vk_nv12_batch *batch,
			 unsigned int *rejected)
{
	bool active = frames && (frames[0] || frames[1] || frames[2]);

	for (unsigned int j = 0; j < SURFACES; j++) {
		struct vk_nv12_image *i = &n->images[j];

		if (!i->image)
			continue;
		if (i->serial > completed) {
			if (n->target != target)
				return VK_NOT_READY;
			continue;
		}
		if (!active || n->target != target) {
			destroy_image(n->ctx->device, i);
			continue;
		}
		bool used = false;

		for (unsigned int k = 0; k < 3; k++)
			used |= frames[k] && frames[k]->lease == i->desc.lease;
		if (!used)
			drop_lease(i);
	}
	if (n->target != target) {
		destroy_colors(n);
		n->target = target;
	}
	for (unsigned int j = 0; active && j < 3; j++) {
		if (!frames[j])
			continue;
		VkResult r = prepare_image(n, frames[j], &batch->images[j]);

		if (r == VK_ERROR_DEVICE_LOST || r == VK_NOT_READY)
			return r;
		if (r != VK_SUCCESS)
			*rejected |= 1u << j;
	}
	return active && *rejected ? VK_NOT_READY : VK_SUCCESS;
}

void vk_nv12_submitted(const struct vk_nv12_batch *batch, uint64_t serial)
{
	for (unsigned int j = 0; j < 3; j++)
		if (batch->images[j])
			batch->images[j]->serial = serial;
}

void vk_nv12_barriers(const struct vk_nv12_batch *batch, VkCommandBuffer cmd,
		      uint32_t family, bool acquire)
{
	VkImageMemoryBarrier2 barriers[3];
	uint32_t count = 0;

	for (unsigned int j = 0; j < 3; j++) {
		const struct vk_nv12_image *i = batch->images[j];

		if (!i || (j && (i == batch->images[0] ||
				 (j == 2 && i == batch->images[1]))))
			continue;
		barriers[count++] = (VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask =
				acquire ?
					VK_PIPELINE_STAGE_2_NONE :
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.srcAccessMask =
				acquire ? 0 :
					  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.dstStageMask =
				acquire ?
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT :
					VK_PIPELINE_STAGE_2_NONE,
			.dstAccessMask =
				acquire ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT :
					  0,
			.oldLayout =
				acquire ?
					VK_IMAGE_LAYOUT_GENERAL :
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout =
				acquire ?
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
					VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex =
				acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : family,
			.dstQueueFamilyIndex =
				acquire ? family : VK_QUEUE_FAMILY_FOREIGN_EXT,
			.image = i->image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
					      0, 1 },
		};
	}
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = count,
		.pImageMemoryBarriers = barriers,
	};
	if (count)
		vkCmdPipelineBarrier2(cmd, &dep);
}

void vk_nv12_draw(const struct vk_nv12_image *i, VkCommandBuffer cmd,
		  const struct ui_push *push, uint32_t index)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			  i->color->pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				i->color->layout, 0, 1, &i->set, 0, NULL);
	vkCmdPushConstants(cmd, i->color->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
			   sizeof(*push), push);
	vkCmdDraw(cmd, 4, 1, 0, index);
}
