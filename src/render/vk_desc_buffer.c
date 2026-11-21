// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_desc_buffer.h"
#include <stdlib.h>

#define DESC_USAGE                                           \
	(VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | \
	 VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT)

static void *mapped_at(const struct vk_desc_buffer *d, VkDeviceSize offset)
{
	return (char *)d->memory.mapped + d->base + offset;
}

VkResult vk_desc_buffer_open(struct vk_ctx *ctx)
{
	struct vk_desc_buffer *d = calloc(1, sizeof(*d));
	if (!d)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	ctx->descriptors = d;
	VkPhysicalDeviceDescriptorBufferPropertiesEXT props = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &props,
	};
	vkGetPhysicalDeviceProperties2(ctx->gpu, &properties);
	d->image_size = props.sampledImageDescriptorSize;
	d->sampler_size = props.samplerDescriptorSize;
	const VkDescriptorSetLayoutBinding bindings[] = {
		{ .binding = 0,
		  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		  .descriptorCount = UI_TEXTURE_SLOTS,
		  .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
		  .descriptorCount = UI_SAMPLER_SLOTS,
		  .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
	};
	VkDescriptorSetLayoutCreateInfo set = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags =
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
		.bindingCount = 2,
		.pBindings = bindings,
	};
	VkDescriptorSetLayoutSupport support = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
	};
	vkGetDescriptorSetLayoutSupport(ctx->device, &set, &support);
	if (!support.supported)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	VkResult result =
		vkCreateDescriptorSetLayout(ctx->device, &set, NULL, &d->set);
	if (result != VK_SUCCESS)
		return result;
	VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
				     .size = sizeof(struct ui_push) };
	VkPipelineLayoutCreateInfo pipeline = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &d->set,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	result = vkCreatePipelineLayout(ctx->device, &pipeline, NULL,
					&d->layout);
	if (result != VK_SUCCESS)
		return result;

	VkDeviceSize size;
	vkGetDescriptorSetLayoutSizeEXT(ctx->device, d->set, &size);
	if (size > props.maxResourceDescriptorBufferRange ||
	    size > props.maxSamplerDescriptorBufferRange)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	vkGetDescriptorSetLayoutBindingOffsetEXT(ctx->device, d->set, 0,
						 &d->images);
	vkGetDescriptorSetLayoutBindingOffsetEXT(ctx->device, d->set, 1,
						 &d->samplers);
	result = vk_buffer_init(
		ctx, &d->memory,
		size + props.descriptorBufferOffsetAlignment - 1,
		DESC_USAGE | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (result != VK_SUCCESS)
		return result;
	d->base = -d->memory.address &
		  (props.descriptorBufferOffsetAlignment - 1);
	for (uint32_t i = 0; i < UI_SAMPLER_SLOTS; i++) {
		VkFilter filter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		VkSamplerCreateInfo sampler = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = filter,
			.minFilter = filter,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		result = vkCreateSampler(ctx->device, &sampler, NULL,
					 &d->sampler[i]);
		if (result != VK_SUCCESS)
			return result;
		VkDescriptorGetInfoEXT get = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
			.type = VK_DESCRIPTOR_TYPE_SAMPLER,
			.data.pSampler = &d->sampler[i],
		};
		vkGetDescriptorEXT(
			ctx->device, &get, d->sampler_size,
			mapped_at(d, d->samplers + i * d->sampler_size));
	}
	return vk_buffer_flush(ctx, &d->memory);
}

VkResult vk_desc_buffer_write(const struct vk_ctx *ctx, uint32_t slot,
			      const VkImageViewCreateInfo *view,
			      VkImageLayout layout)
{
	struct vk_desc_buffer *d = ctx->descriptors;
	/* Noncoherent atom ranges may cover adjacent, live array entries. */
	if (!(d->memory.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		VkResult idle = vkQueueWaitIdle(ctx->queue);
		if (idle != VK_SUCCESS)
			return idle;
	}
	VkDescriptorImageInfo image = { .imageLayout = layout };
	VkResult result =
		vkCreateImageView(ctx->device, view, NULL, &image.imageView);
	if (result != VK_SUCCESS)
		return result;
	VkDescriptorGetInfoEXT get = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
		.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		.data.pSampledImage = &image,
	};
	vkGetDescriptorEXT(ctx->device, &get, d->image_size,
			   mapped_at(d, d->images + slot * d->image_size));
	vkDestroyImageView(ctx->device, d->view[slot], NULL);
	d->view[slot] = image.imageView;
	return vk_buffer_flush(ctx, &d->memory);
}

void vk_desc_buffer_bind(const struct vk_ctx *ctx, VkCommandBuffer cmd,
			 const struct ui_push *push)
{
	const struct vk_desc_buffer *d = ctx->descriptors;
	const VkDescriptorBufferBindingInfoEXT binding = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
		.address = d->memory.address + d->base,
		.usage = DESC_USAGE,
	};
	const uint32_t index = 0;
	const VkDeviceSize offset = 0;
	vkCmdBindDescriptorBuffersEXT(cmd, 1, &binding);
	vkCmdSetDescriptorBufferOffsetsEXT(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
					   d->layout, 0, 1, &index, &offset);
	vkCmdPushConstants(cmd, d->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
			   offsetof(struct ui_push, clip_count) +
				   sizeof(push->clip_count),
			   push);
}

void vk_desc_buffer_close(struct vk_ctx *ctx)
{
	struct vk_desc_buffer *d = ctx->descriptors;
	if (!d)
		return;
	for (uint32_t i = 0; i < UI_TEXTURE_SLOTS; i++)
		if (d->view[i])
			vkDestroyImageView(ctx->device, d->view[i], NULL);
	for (uint32_t i = 0; i < UI_SAMPLER_SLOTS; i++)
		vkDestroySampler(ctx->device, d->sampler[i], NULL);
	vk_buffer_destroy(ctx, &d->memory);
	vkDestroyPipelineLayout(ctx->device, d->layout, NULL);
	vkDestroyDescriptorSetLayout(ctx->device, d->set, NULL);
	free(d);
	ctx->descriptors = NULL;
}
