// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_backend.h"
#include "vk_present.h"
#include "vk_canvas.h"
#include <stdlib.h>

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

static int draw(void *state, const struct gpu_scene *scene)
{
	struct backend *b = state;
	VkResult result = vk_canvas_draw(b->canvas, scene);

	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
		return 1;
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_NOT_READY ||
	    result == VK_TIMEOUT)
		return 0;
	return -1;
}

__attribute__((visibility("default")))
const struct gpu_backend avernella_gpu_v1 = {
	.open = open_backend,
	.close = close_backend,
	.size = drawable,
	.draw = draw,
};
