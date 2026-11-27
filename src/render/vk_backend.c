// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_backend.h"
#include "gpu_dmabuf.h"
#include "vk_present.h"
#include "vk_canvas.h"
#include <stdlib.h>
#include <string.h>

struct backend {
	struct vk_present *present;
	struct vk_canvas *canvas;
};

static void close_backend(void *state)
{
	struct backend *b = state;

	if (b->canvas)
		vk_canvas_close(b->canvas);
	if (b->present)
		vk_present_close(b->present);
	free(b);
}

static void *open_backend(SDL_Window *window, bool log)
{
	struct backend *b = calloc(1, sizeof(*b));

	if (!b)
		return NULL;
	b->present = vk_present_open(window, 0, log, true);
	if (b->present)
		b->canvas = vk_canvas_open(b->present);
	if (b->canvas)
		return b;
	close_backend(b);
	return NULL;
}

static void drawable(void *state, int *w, int *h)
{
	struct backend *b = state;
	VkExtent2D size = vk_present_extent(b->present);

	*w = size.width;
	*h = size.height;
}

static int draw_dmabuf(void *state, const struct gpu_scene *scene,
		       const struct gpu_dmabuf *const frames[3],
		       unsigned int *rejected)
{
	struct backend *b = state;

	if (rejected)
		*rejected = 0;
	VkResult result =
		vk_canvas_draw_dmabuf(b->canvas, scene, frames, rejected);

	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
		return 1;
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_NOT_READY ||
	    result == VK_TIMEOUT)
		return 0;
	return -1;
}

static int draw(void *state, const struct gpu_scene *scene)
{
	return draw_dmabuf(state, scene, NULL, NULL);
}

static bool dmabuf_supported(void *state)
{
	struct backend *b = state;

	return vk_present_dmabuf_supported(b->present);
}

__attribute__((visibility("default")))
const struct gpu_dmabuf_backend avernella_gpu_dmabuf_v1 = {
	.supported = dmabuf_supported,
	.draw = draw_dmabuf,
};

static void get_device(void *state, struct gpu_device_info *device)
{
	struct backend *b = state;
	const struct vk_ctx *ctx = vk_present_context(b->present);
	VkPhysicalDeviceProperties props;

	vkGetPhysicalDeviceProperties(ctx->gpu, &props);
	_Static_assert(sizeof(device->name) == sizeof(props.deviceName),
		       "Vulkan physical device name size");
	memcpy(device->name, props.deviceName, sizeof(device->name));
	device->vendor = props.vendorID;
}

__attribute__((visibility("default")))
const struct gpu_device_query avernella_gpu_device_v1 = { .get = get_device };

static int render_minor(void *state)
{
	struct backend *b = state;

	return vk_present_context(b->present)->render_minor;
}

__attribute__((visibility("default")))
const struct gpu_drm_query avernella_gpu_drm_v1 = { .render_minor =
							    render_minor };

__attribute__((visibility("default")))
const struct gpu_backend avernella_gpu_v1 = {
	.open = open_backend,
	.close = close_backend,
	.size = drawable,
	.draw = draw,
};
