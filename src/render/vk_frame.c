// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_frame.h"
#include "line_ui_spirv.h"

VkResult vk_ui_pipeline(const struct vk_ctx *ctx, VkFormat format,
			VkPipeline *pipeline)
{
	VkShaderModuleCreateInfo code[] = {
		{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		  .codeSize = sizeof(ui_vert_spv),
		  .pCode = ui_vert_spv },
		{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		  .codeSize = sizeof(ui_frag_spv),
		  .pCode = ui_frag_spv },
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
	VkPipelineShaderStageCreateInfo stages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = modules[0],
		  .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .pNext = &mapping,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = modules[1],
		  .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vertex = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
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
		.blendEnable = VK_TRUE,
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
		.pNext = &flags,
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

	result = vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1,
					   &info, NULL, pipeline);
finish:
	vkDestroyShaderModule(ctx->device, modules[1], NULL);
	vkDestroyShaderModule(ctx->device, modules[0], NULL);
	return result;
}

void vk_ui_draw(const struct vk_ctx *ctx, VkCommandBuffer cmd,
		VkPipeline pipeline, VkImageView target, VkExtent2D extent,
		const struct ui_push *push, uint32_t count)
{
	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = target,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};
	VkRenderingInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea.extent = extent,
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

	vkCmdBindResourceHeapEXT(cmd, &ctx->resources.bind);
	vkCmdBindSamplerHeapEXT(cmd, &ctx->samplers.bind);
	vkCmdPushDataEXT(cmd, &data);
	vkCmdBeginRendering(cmd, &rendering);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &rendering.renderArea);
	vkCmdDraw(cmd, 6, count, 0, 0);
	vkCmdEndRendering(cmd);
}
