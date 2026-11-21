/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "vk_ctx.h"
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/* Match the Linux device number, including paths through /dev/dri/by-path. */
static inline bool vk_drm_matches(VkPhysicalDevice gpu, const char *render_node,
				  bool supports_drm)
{
	struct stat st;

	if (!render_node || !*render_node)
		return true;
	if (!supports_drm || stat(render_node, &st) || !S_ISCHR(st.st_mode))
		return false;
	VkPhysicalDeviceDrmPropertiesEXT drm = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &drm,
	};

	vkGetPhysicalDeviceProperties2(gpu, &props);
	return drm.hasRender && drm.renderMajor == major(st.st_rdev) &&
	       drm.renderMinor == minor(st.st_rdev);
}
