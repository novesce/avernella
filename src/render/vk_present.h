/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LINE_VK_PRESENT_H
#define LINE_VK_PRESENT_H

#include "vk_ctx.h"
#include <stdbool.h>

#define VK_PRESENT_SLOTS 2

typedef struct SDL_Window SDL_Window;
struct vk_present;

/* The slot's prior submission has retired before upload records commands. */
typedef VkResult (*vk_present_upload_fn)(void *user, VkCommandBuffer cmd,
					 uint32_t slot);

struct vk_present *vk_present_open(SDL_Window *window, uint32_t vendor,
				   bool log, bool opaque);
void vk_present_close(struct vk_present *p);
VkExtent2D vk_present_extent(struct vk_present *p);
struct vk_ctx *vk_present_context(struct vk_present *p);
VkResult vk_present_wait(struct vk_present *p);
/* Changed drawable pixels since the last present. Consumed by the next draw. */
void vk_present_set_damage(struct vk_present *p, const VkRect2D *changed);
/* A negative result other than OUT_OF_DATE requires recreating the presenter. */
VkResult vk_present_draw(struct vk_present *p, const struct ui_prim *prims,
			 uint32_t count, VkClearColorValue clear,
			 vk_present_upload_fn upload, void *user);

#endif
