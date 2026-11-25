// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NVIDIA GPU health against small synthetic sysfs trees.
 */
#include "../src/gpu_health.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct gpu {
	const char *address, *vendor, *class, *driver;
	gboolean drm;
};

static void write_file(const char *dir, const char *name, const char *text)
{
	g_autofree char *path = g_build_filename(dir, name, NULL);

	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* A sysfs root with the NVIDIA modules (or not) and these PCI devices. */
static char *fake_sysfs(gboolean nvidia, const char *modeset,
			const struct gpu *gpus, gsize count)
{
	char *root = g_dir_make_tmp("gpu-health-XXXXXX", NULL);
	g_autofree char *devices =
		g_build_filename(root, "bus", "pci", "devices", NULL);
	g_autofree char *drivers =
		g_build_filename(root, "bus", "pci", "drivers", NULL);

	g_assert_nonnull(root);
	g_assert_cmpint(g_mkdir_with_parents(devices, 0700), ==, 0);
	if (nvidia) {
		g_autofree char *module =
			g_build_filename(root, "module", "nvidia", NULL);

		g_assert_cmpint(g_mkdir_with_parents(module, 0700), ==, 0);
	}
	if (modeset) {
		g_autofree char *params = g_build_filename(
			root, "module", "nvidia_drm", "parameters", NULL);

		g_assert_cmpint(g_mkdir_with_parents(params, 0700), ==, 0);
		write_file(params, "modeset", modeset);
	}
	for (gsize i = 0; i < count; i++) {
		g_autofree char *device =
			g_build_filename(devices, gpus[i].address, NULL);

		g_assert_cmpint(g_mkdir_with_parents(device, 0700), ==, 0);
		write_file(device, "vendor", gpus[i].vendor);
		write_file(device, "class", gpus[i].class);
		if (gpus[i].drm) {
			g_autofree char *drm =
				g_build_filename(device, "drm", NULL);

			g_assert_cmpint(g_mkdir(drm, 0700), ==, 0);
		}
		if (gpus[i].driver) {
			g_autofree char *driver =
				g_build_filename(drivers, gpus[i].driver, NULL);
			g_autofree char *link =
				g_build_filename(device, "driver", NULL);

			g_assert_cmpint(g_mkdir_with_parents(driver, 0700), ==,
					0);
			g_assert_cmpint(symlink(driver, link), ==, 0);
		}
	}
	return root;
}

static void remove_tree(const char *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const char *name;

	while (dir && (name = g_dir_read_name(dir))) {
		g_autofree char *child = g_build_filename(path, name, NULL);

		if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
		    !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
			remove_tree(child);
		else
			g_unlink(child);
	}
	g_rmdir(path);
}

static const struct gpu amd = { "0000:65:00.0", "0x1002", "0x030000", "amdgpu",
				TRUE };

struct health_case {
	gboolean nvidia;
	const char *modeset;
	struct gpu gpu;
	gboolean present, unusable;
	const char *reason;
};

static void health(gconstpointer data)
{
	const struct health_case *c = data;
	struct gpu gpus[] = { amd, c->gpu };
	g_autofree char *root =
		fake_sysfs(c->nvidia, c->modeset, gpus, c->present ? 2 : 1);
	char why[160] = "";

	g_assert_cmpint(line_nvidia_unusable(root, why, sizeof(why)), ==,
			c->unusable);
	if (c->reason)
		g_assert_nonnull(strstr(why, c->reason));
	remove_tree(root);
}

static const struct health_case cases[] = {
	/* Bound to the driver with its display device: fine. */
	{ TRUE,
	  "Y",
	  { "0000:01:00.0", "0x10de", "0x030000", "nvidia", TRUE },
	  TRUE,
	  FALSE,
	  NULL },
	/* A 3D controller (class 0x0302) counts as well. */
	{ TRUE,
	  "Y",
	  { "0000:01:00.0", "0x10de", "0x030200", "nvidia", TRUE },
	  TRUE,
	  FALSE,
	  NULL },
	/* Removal left the device without a driver. */
	{ TRUE,
	  "Y",
	  { "0000:01:00.0", "0x10de", "0x030000", NULL, FALSE },
	  TRUE,
	  TRUE,
	  "0000:01:00.0 has no driver" },
	/* Bound, but the display device went away. */
	{ TRUE,
	  "Y",
	  { "0000:01:00.0", "0x10de", "0x030000", "nvidia", FALSE },
	  TRUE,
	  TRUE,
	  "has no display device" },
	/* Without modesetting there is no display device to expect. */
	{ TRUE,
	  "N",
	  { "0000:01:00.0", "0x10de", "0x030000", "nvidia", FALSE },
	  TRUE,
	  FALSE,
	  NULL },
	/* Handed to a virtual machine. */
	{ TRUE,
	  "Y",
	  { "0000:01:00.0", "0x10de", "0x030000", "vfio-pci", TRUE },
	  TRUE,
	  TRUE,
	  "is not on the NVIDIA driver" },
	/* The driver is loaded, but its GPU left the bus. */
	{ TRUE, "Y", { 0 }, FALSE, TRUE, "its GPU is gone" },
	/* No NVIDIA driver: nothing can block. */
	{ FALSE,
	  NULL,
	  { "0000:01:00.0", "0x10de", "0x030000", "nouveau", TRUE },
	  TRUE,
	  FALSE,
	  NULL },
	/* An NVIDIA audio function is not a GPU. */
	{ TRUE,
	  "Y",
	  { "0000:01:00.1", "0x10de", "0x040300", NULL, FALSE },
	  TRUE,
	  TRUE,
	  "its GPU is gone" },
};

static void startup(void)
{
	struct gpu broken[] = {
		amd, { "0000:01:00.0", "0x10de", "0x030000", NULL, FALSE }
	};
	struct gpu healthy[] = {
		amd, { "0000:01:00.0", "0x10de", "0x030000", "nvidia", TRUE }
	};
	g_autofree char *bad = fake_sysfs(TRUE, "Y", broken, 2);
	g_autofree char *good = fake_sysfs(TRUE, "Y", healthy, 2);

	g_unsetenv("VK_LOADER_DRIVERS_DISABLE");
	g_unsetenv("VK_LOADER_DRIVERS_SELECT");
	g_unsetenv("VK_DRIVER_FILES");
	g_unsetenv("VK_ICD_FILENAMES");
	g_unsetenv("LINE_DISABLE_GPU");
	g_assert_false(line_gpu_avoid_broken_nvidia(good));
	g_assert_null(g_getenv("VK_LOADER_DRIVERS_DISABLE"));
	g_assert_true(line_gpu_avoid_broken_nvidia(bad));
	g_assert_cmpstr(g_getenv("VK_LOADER_DRIVERS_DISABLE"), ==, "*nvidia*");
	/* The user's own driver choice wins. */
	g_setenv("VK_LOADER_DRIVERS_DISABLE", "*intel*", TRUE);
	g_assert_false(line_gpu_avoid_broken_nvidia(bad));
	g_assert_cmpstr(g_getenv("VK_LOADER_DRIVERS_DISABLE"), ==, "*intel*");
	g_unsetenv("VK_LOADER_DRIVERS_DISABLE");
	g_setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/x.json", TRUE);
	g_assert_false(line_gpu_avoid_broken_nvidia(bad));
	g_assert_null(g_getenv("VK_LOADER_DRIVERS_DISABLE"));
	g_unsetenv("VK_DRIVER_FILES");
	/* No GPU drawing at all: nothing to steer. */
	g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	g_assert_false(line_gpu_avoid_broken_nvidia(bad));
	g_unsetenv("LINE_DISABLE_GPU");
	remove_tree(bad);
	remove_tree(good);
}

int main(int argc, char **argv)
{
	static const char *const names[] = {
		"/gpu-health/healthy",	      "/gpu-health/3d-controller",
		"/gpu-health/no-driver",      "/gpu-health/no-display-device",
		"/gpu-health/modeset-off",    "/gpu-health/passthrough",
		"/gpu-health/gpu-gone",	      "/gpu-health/no-nvidia-driver",
		"/gpu-health/audio-function",
	};

	G_STATIC_ASSERT(G_N_ELEMENTS(names) == G_N_ELEMENTS(cases));
	g_test_init(&argc, &argv, NULL);
	for (gsize i = 0; i < G_N_ELEMENTS(cases); i++)
		g_test_add_data_func(names[i], &cases[i], health);
	g_test_add_func("/gpu-health/startup", startup);
	return g_test_run();
}
