/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gpu_scene.h"
#include "vk_present.h"

struct vk_canvas;

struct vk_canvas *vk_canvas_open(struct vk_present *p);
void vk_canvas_close(struct vk_canvas *canvas);
VkResult vk_canvas_draw(struct vk_canvas *canvas,
			const struct gpu_scene *scene);
