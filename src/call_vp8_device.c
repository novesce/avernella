// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_vp8_device.h"
#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/*
 * A CUDA ordinal is independent of the Linux render node number. Resolve
 * the node's PCI address through the driver, including CUDA_VISIBLE_DEVICES.
 */
gboolean call_vp8_cuda_ordinal(const char *pci, char result[16])
{
	void *library = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
	int (*init)(unsigned int flags);
	int (*lookup)(int *device, const char *pci);
	int device;
	gboolean found = FALSE;

	if (!library)
		return FALSE;
	void *symbol = dlsym(library, "cuInit");

	memcpy(&init, &symbol, sizeof(init));
	symbol = dlsym(library, "cuDeviceGetByPCIBusId");
	memcpy(&lookup, &symbol, sizeof(lookup));
	if (init && lookup && !init(0) && !lookup(&device, pci)) {
		g_snprintf(result, 16, "%d", device);
		found = TRUE;
	}
	dlclose(library);
	return found;
}

static guint render_vendor(const char *node, char **pci)
{
	struct stat st;

	if (stat(node, &st) || !S_ISCHR(st.st_mode))
		return 0;
	g_autofree char *device = g_strdup_printf("/sys/dev/char/%u:%u/device",
						  major(st.st_rdev),
						  minor(st.st_rdev));
	g_autofree char *vendor_file = g_build_filename(device, "vendor", NULL);
	g_autofree char *vendor = NULL;
	g_autofree char *target = g_file_read_link(device, NULL);

	if (!target || !g_file_get_contents(vendor_file, &vendor, NULL, NULL))
		return 0;
	*pci = g_path_get_basename(target);
	return g_ascii_strtoull(vendor, NULL, 0);
}

gboolean call_vp8_try_render_nodes(gboolean allow_alternates,
				   call_vp8_probe_fn probe, gpointer data)
{
	const char *node = g_getenv("AVERNELLA_RENDER_NODE");
	const char *hint = g_getenv("AVERNELLA_MEDIA_GPU_VENDOR");
	guint preferred = hint ? g_ascii_strtoull(hint, NULL, 0) : 0;
	gboolean pinned = node && *node;
	g_autofree char *first_device = NULL;

	if (!pinned)
		node = g_getenv("AVERNELLA_MEDIA_RENDER_NODE");
	if (node && *node) {
		guint vendor = render_vendor(node, &first_device);

		if (vendor && probe(node, vendor, first_device, data))
			return TRUE;
		if (pinned || !allow_alternates)
			return FALSE;
	}

	g_autoptr(GDir) dir = g_dir_open("/dev/dri", 0, NULL);
	const char *name;

	if (!dir)
		return FALSE;
	guint passes = preferred && !allow_alternates ? 1 : 2;
	if (!preferred)
		preferred = 0x10de;

	for (guint pass = 0; pass < passes; pass++) {
		while ((name = g_dir_read_name(dir))) {
			if (!g_str_has_prefix(name, "renderD"))
				continue;
			g_autofree char *path =
				g_build_filename("/dev/dri", name, NULL);
			g_autofree char *pci = NULL;
			guint vendor = render_vendor(path, &pci);

			if (!vendor || (vendor == preferred) != (pass == 0) ||
			    !g_strcmp0(first_device, pci))
				continue;
			if (probe(path, vendor, pci, data))
				return TRUE;
		}
		g_dir_rewind(dir);
	}
	return FALSE;
}
