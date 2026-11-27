// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_frame.h"
#include "vk_nv12.h"
#include "vk_desc_buffer.h"
#include "line_ui_spirv.h"
#include <stdio.h>

static VkResult create_pipeline(const struct vk_ctx *ctx, VkFormat format,
				bool opaque, VkPipelineLayout nv12,
				VkPipeline *pipeline)
{
	VkShaderModuleCreateInfo code[] = {
		{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		  .codeSize = sizeof(ui_uber_vert_spv),
		  .pCode = ui_uber_vert_spv },
		{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		  .codeSize = nv12 ? sizeof(ui_nv12_frag_spv) :
				     sizeof(ui_uber_frag_spv),
		  .pCode = nv12 ? ui_nv12_frag_spv : ui_uber_frag_spv },
	};
	VkShaderModule modules[2] = { 0 };
	VkResult result =
		vkCreateShaderModule(ctx->device, &code[0], NULL, &modules[0]);

	if (result != VK_SUCCESS)
		return result;
	result = vkCreateShaderModule(ctx->device, &code[1], NULL, &modules[1]);
	if (result != VK_SUCCESS)
		goto finish;
	VkDescriptorSetAndBindingMappingEXT mappings[] = {
		{ .sType =
			  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
		  .firstBinding = 0,
		  .bindingCount = 1,
		  .resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
		  .source =
			  VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
		  .sourceData.constantOffset.heapArrayStride =
			  ctx->resources.stride },
		{ .sType =
			  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
		  .firstBinding = 1,
		  .bindingCount = 1,
		  .resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
		  .source =
			  VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
		  .sourceData.constantOffset.heapArrayStride =
			  ctx->samplers.stride },
	};
	VkShaderDescriptorSetAndBindingMappingInfoEXT mapping = {
		.sType =
			VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
		.mappingCount = 2,
		.pMappings = mappings,
	};
	const VkBool32 mode = opaque;
	const VkSpecializationMapEntry entry = { .constantID = 0,
						 .size = sizeof(mode) };
	const VkSpecializationInfo specialization = {
		.mapEntryCount = 1,
		.pMapEntries = &entry,
		.dataSize = sizeof(mode),
		.pData = &mode,
	};
	VkPipelineShaderStageCreateInfo stages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = modules[0],
		  .pSpecializationInfo = &specialization,
		  .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .pNext = !nv12 && ctx->descriptor_heap ? &mapping : NULL,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = modules[1],
		  .pSpecializationInfo = nv12 ? NULL : &specialization,
		  .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vertex = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.lineWidth = 1,
	};
	VkPipelineMultisampleStateCreateInfo samples = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState attachment = {
		.blendEnable = !opaque,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
	};
	VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT,
				    VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = states,
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &format,
	};
	VkPipelineCreateFlags2CreateInfo flags = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
		.pNext = &rendering,
		.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
	};
	VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = !nv12 && ctx->descriptor_heap ? (void *)&flags :
							 (void *)&rendering,
		.flags = nv12 || ctx->descriptor_heap ?
				 0 :
				 VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
		.layout = nv12		   ? nv12 :
			  ctx->descriptors ? ctx->descriptors->layout :
					     VK_NULL_HANDLE,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &raster,
		.pMultisampleState = &samples,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
	};

	VkPipelineCreationFeedback feedback = { 0 };
	VkPipelineCreationFeedbackCreateInfo report = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
		.pNext = info.pNext,
		.pPipelineCreationFeedback = &feedback,
	};
	if (ctx->log_pipeline)
		info.pNext = &report;
	result = vkCreateGraphicsPipelines(ctx->device, ctx->pipeline_cache, 1,
					   &info, NULL, pipeline);
	if (result == VK_SUCCESS && ctx->log_pipeline &&
	    (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT))
		fprintf(stderr,
			"Vulkan %s pipeline: %.3f ms, application cache %s\n",
			nv12 ? "NV12" : "UI", feedback.duration / 1000000.,
			(feedback.flags &
			 VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ?
				"hit" :
				"miss");
finish:
	vkDestroyShaderModule(ctx->device, modules[1], NULL);
	vkDestroyShaderModule(ctx->device, modules[0], NULL);
	return result;
}

VkResult vk_ui_pipeline_mode(const struct vk_ctx *ctx, VkFormat format,
			     bool opaque, VkPipeline *pipeline)
{
	return create_pipeline(ctx, format, opaque, VK_NULL_HANDLE, pipeline);
}

VkResult vk_ui_nv12_pipeline(const struct vk_ctx *ctx, VkFormat format,
			     VkPipelineLayout layout, VkPipeline *pipeline)
{
	return create_pipeline(ctx, format, true, layout, pipeline);
}

/* Called for opaque batches only, with the acquired image's repair area. */
bool vk_visible_layers(const struct ui_prim **prims, uint32_t *count,
		       VkRect2D area)
{
	for (uint32_t i = *count; *prims && i-- > 0;) {
		const struct ui_prim *item = &(*prims)[i];
		uint32_t kind = item->prim_type & 255;

		if ((kind != UI_PRIM_CANVAS && kind != UI_PRIM_I420 &&
		     kind != UI_PRIM_NV12) ||
		    item->clip_rect_idx != UI_NO_CLIP ||
		    item->pos[0] > area.offset.x ||
		    item->pos[1] > area.offset.y ||
		    item->pos[0] + item->size[0] <
			    area.offset.x + area.extent.width ||
		    item->pos[1] + item->size[1] <
			    area.offset.y + area.extent.height)
			continue;
		*prims += i;
		*count -= i;
		return true;
	}
	return false;
}

void vk_ui_draw_mode(const struct vk_ctx *ctx, VkCommandBuffer cmd,
		     VkPipeline pipeline, VkImageView target, VkExtent2D extent,
		     VkRect2D area, const struct ui_push *push, uint32_t count,
		     VkClearColorValue clear, bool discard,
		     const struct vk_nv12_batch *external,
		     const struct ui_prim *items)
{
	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = target,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = discard ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
				    VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue.color = clear,
	};
	VkRenderingInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = area,
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	VkPushDataInfoEXT data = {
		.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
		.data = { push, offsetof(struct ui_push, clip_count) +
					sizeof(push->clip_count) },
	};
	VkViewport viewport = { 0, 0, extent.width, extent.height, 0, 1 };

	vkCmdBeginRendering(cmd, &rendering);
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &rendering.renderArea);
	for (uint32_t i = 0; i < count;) {
		uint32_t first = i++;

		if (external &&
		    (items[first].prim_type & 255) == UI_PRIM_NV12) {
			vk_nv12_draw(external->images[items[first].tex_index],
				     cmd, push, first);
			continue;
		}
		if (external)
			while (i < count &&
			       (items[i].prim_type & 255) != UI_PRIM_NV12)
				i++;
		else
			i = count;
		if (ctx->descriptor_heap) {
			vkCmdBindResourceHeapEXT(cmd, &ctx->resources.bind);
			vkCmdBindSamplerHeapEXT(cmd, &ctx->samplers.bind);
			vkCmdPushDataEXT(cmd, &data);
		} else {
			vk_desc_buffer_bind(ctx, cmd, push);
		}
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				  pipeline);
		vkCmdDraw(cmd, 4, i - first, 0, first);
	}
	vkCmdEndRendering(cmd);
}
