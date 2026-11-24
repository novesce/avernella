/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <volk.h>

struct vk_adapter {
	VkPhysicalDevice gpu;
	VkPhysicalDeviceType type;
	uint32_t family, vendor;
	int render_minor;
	bool heap, maintenance_khr, incremental, host_copy;
};

/* Null handles represent incompatible adapters or failed device opens.
 * Prefer NVIDIA discrete, other discrete, then integrated. Resolve ties
 * using descriptor support and retain loader order for equal candidates.
 */
static inline struct vk_adapter *vk_preferred_adapter(struct vk_adapter *list,
						      uint32_t count)
{
	struct vk_adapter *best = NULL;
	int best_score = -1;

	for (uint32_t i = 0; i < count; i++) {
		struct vk_adapter *adapter = &list[i];
		int score;

		if (!adapter->gpu)
			continue;
		switch (adapter->type) {
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			score = adapter->vendor == 0x10de ? 8 : 6;
			break;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			score = 4;
			break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			score = 0;
			break;
		default:
			score = 2;
			break;
		}
		score += adapter->heap;
		if (score > best_score) {
			best = adapter;
			best_score = score;
		}
	}
	return best;
}
