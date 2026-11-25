/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Whether the NVIDIA GPU can be used. A GPU that has left the PCI bus can
 * leave the NVIDIA kernel driver blocked; its Vulkan driver then waits
 * forever when it opens the device, so Avernella must not load it.
 */
#ifndef LINE_GPU_HEALTH_H
#define LINE_GPU_HEALTH_H

#include <glib.h>

/*
 * TRUE when the NVIDIA kernel driver is loaded but no NVIDIA display
 * device is bound to it with a working display (DRM) device. why receives
 * a short English reason. sysfs is "/sys" (NULL) outside tests.
 */
gboolean line_nvidia_unusable(const char *sysfs, char *why, gsize size);

/*
 * At UI start-up, before any thread: keeps an unusable NVIDIA GPU's Vulkan
 * driver from loading (VK_LOADER_DRIVERS_DISABLE), so the renderer picks
 * another GPU. Respects the user's own Vulkan driver settings and
 * LINE_DISABLE_GPU=1. Returns TRUE when it turned the driver off.
 */
gboolean line_gpu_avoid_broken_nvidia(const char *sysfs);

#endif
