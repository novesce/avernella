// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NVIDIA GPU health from sysfs.
 *
 * When a laptop's NVIDIA GPU drops off the PCI bus, the kernel removes it
 * while the NVIDIA driver stays loaded. The removal can hang and hold the
 * driver's locks; from then on, opening /dev/nvidia-modeset waits forever.
 * NVIDIA's Vulkan driver does that while the loader starts up, before the
 * renderer can try another GPU, and so did Avernella's window.
 *
 * Reading these sysfs files never wakes a suspended GPU. A GPU in runtime
 * power saving keeps its driver binding and display device, so it counts
 * as healthy.
 */
#include "gpu_health.h"
#include <stdlib.h>
#include <string.h>

static char *read_line(const char *dir, const char *name)
{
	g_autofree char *path = g_build_filename(dir, name, NULL);
	char *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return NULL;
	g_strstrip(text);
	return text;
}

static gboolean exists(const char *dir, const char *name)
{
	g_autofree char *path = g_build_filename(dir, name, NULL);

	return g_file_test(path, G_FILE_TEST_EXISTS);
}

gboolean line_nvidia_unusable(const char *sysfs, char *why, gsize size)
{
	const char *root = sysfs ? sysfs : "/sys";
	g_autofree char *module = g_build_filename(root, "module", NULL);
	g_autofree char *modeset_file = g_build_filename(
		module, "nvidia_drm", "parameters", "modeset", NULL);
	g_autofree char *modeset = NULL;
	g_autofree char *devices =
		g_build_filename(root, "bus", "pci", "devices", NULL);
	g_autoptr(GDir) dir = NULL;
	g_autofree char *broken = NULL;
	const char *reason = NULL;
	const char *name;

	/* Without the kernel driver, its Vulkan driver fails at once. */
	if (!exists(module, "nvidia"))
		return FALSE;
	g_file_get_contents(modeset_file, &modeset, NULL, NULL);
	gboolean needs_drm = modeset && modeset[0] == 'Y';

	dir = g_dir_open(devices, 0, NULL);
	while (dir && (name = g_dir_read_name(dir))) {
		g_autofree char *device = g_build_filename(devices, name, NULL);
		g_autofree char *vendor = read_line(device, "vendor");
		g_autofree char *class = read_line(device, "class");

		if (g_strcmp0(vendor, "0x10de") || !class ||
		    !g_str_has_prefix(class, "0x03"))
			continue;
		g_autofree char *link =
			g_build_filename(device, "driver", NULL);
		g_autofree char *driver = g_file_read_link(link, NULL);
		g_autofree char *bound = driver ? g_path_get_basename(driver) :
						  NULL;

		if (!g_strcmp0(bound, "nvidia") &&
		    (!needs_drm || exists(device, "drm")))
			return FALSE;
		if (!broken) {
			broken = g_strdup(name);
			reason = !bound ? "has no driver" :
				 g_strcmp0(bound, "nvidia") ?
					  "is not on the NVIDIA driver" :
					  "has no display device";
		}
	}
	if (why && size) {
		if (broken)
			g_snprintf(why, size, "the NVIDIA GPU at %s %s", broken,
				   reason);
		else
			g_snprintf(
				why, size,
				"the NVIDIA driver is loaded but its GPU is gone");
	}
	return TRUE;
}

gboolean line_gpu_avoid_broken_nvidia(const char *sysfs)
{
	static const char *const chosen[] = {
		"VK_LOADER_DRIVERS_DISABLE",
		"VK_LOADER_DRIVERS_SELECT",
		"VK_DRIVER_FILES",
		"VK_ICD_FILENAMES",
	};
	char why[160];

	if (!g_strcmp0(g_getenv("LINE_DISABLE_GPU"), "1") ||
	    !line_nvidia_unusable(sysfs, why, sizeof(why)))
		return FALSE;
	for (gsize i = 0; i < G_N_ELEMENTS(chosen); i++) {
		if (g_getenv(chosen[i])) {
			g_printerr("avernella: %s; keeping your %s setting\n",
				   why, chosen[i]);
			return FALSE;
		}
	}
	/* The loader matches this against NVIDIA's nvidia_icd.json. */
	if (setenv("VK_LOADER_DRIVERS_DISABLE", "*nvidia*", 1))
		return FALSE;
	g_printerr("avernella: %s; drawing on another GPU\n", why);
	return TRUE;
}
