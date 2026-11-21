// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/vk_present.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FRAMES 60

struct preview {
	struct vk_present *presenter;
	unsigned frames, rebuilds;
	VkExtent2D extent;
	double host_total_ms, host_max_ms;
};

static VkResult present(struct preview *p)
{
	VkExtent2D extent = vk_present_extent(p->presenter);
	float w = extent.width, h = extent.height;
	struct ui_prim prims[3];
	uint64_t start = SDL_GetPerformanceCounter();
	float progress = (p->frames % TEST_FRAMES + 1) / (float)TEST_FRAMES;

	prims[0] = (struct ui_prim){ .pos = { w * .08f, h * .14f },
				     .size = { w * .84f, h * .72f },
				     .color = 0xff2c2522,
				     .border_color = 0xff403b35,
				     .border_width = 2,
				     .corner_radius = 18,
				     .clip_rect_idx = UI_NO_CLIP };
	prims[1] = (struct ui_prim){ .pos = { w * .16f, h * .48f },
				     .size = { w * .68f, h * .08f },
				     .color = 0xff171412,
				     .corner_radius = 10,
				     .clip_rect_idx = UI_NO_CLIP };
	prims[2] = (struct ui_prim){ .pos = { w * .16f, h * .48f },
				     .size = { w * .68f * progress, h * .08f },
				     .color = 0xffe8a670,
				     .corner_radius = 10,
				     .clip_rect_idx = UI_NO_CLIP };
	VkClearColorValue background = {
		.float32 = { 21 / 255.f, 22 / 255.f, 24 / 255.f, 1 },
	};
	VkRect2D changed = {
		.offset = { (int32_t)(w * .16f) - 2, (int32_t)(h * .48f) - 2 },
		.extent = { (uint32_t)(w * .68f) + 5,
			    (uint32_t)(h * .08f) + 5 },
	};
	vk_present_set_damage(p->presenter, &changed);
	VkResult result =
		vk_present_draw(p->presenter, prims, 3, background, NULL, NULL);
	if (result == VK_SUCCESS) {
		if (p->extent.width != extent.width ||
		    p->extent.height != extent.height)
			p->rebuilds++;
		p->extent = extent;
		p->frames++;
		double ms = (SDL_GetPerformanceCounter() - start) * 1000.0 /
			    SDL_GetPerformanceFrequency();
		p->host_total_ms += ms;
		if (ms > p->host_max_ms)
			p->host_max_ms = ms;
	}
	return result;
}

static bool run(struct preview *p, SDL_Window *window, bool test)
{
	bool dirty = true, running = true;
	unsigned checkpoints = 0;
	uint64_t next = 0, show_at = 0, idle_submissions = 0;
	uint64_t deadline = SDL_GetTicks64() + 10000;

	while (running) {
		uint64_t now = SDL_GetTicks64();

		if (test && (now >= deadline || p->frames >= TEST_FRAMES))
			break;
		if (show_at && now >= show_at) {
			if (p->frames != idle_submissions)
				return false;
			puts("Hidden window: zero submissions during the 180 ms pause");
			SDL_ShowWindow(window);
			show_at = 0;
			dirty = true;
		}
		if (test && checkpoints < 3 &&
		    p->frames >= (checkpoints + 1) * 12) {
			if (checkpoints == 2) {
				SDL_HideWindow(window);
				idle_submissions = p->frames;
				show_at = now + 180;
			} else {
				SDL_SetWindowSize(window,
						  checkpoints ? 520 : 830,
						  checkpoints ? 320 : 510);
				dirty = true;
			}
			checkpoints++;
		}
		bool visible = !show_at &&
			       !(SDL_GetWindowFlags(window) &
				 (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
		if (visible && (dirty || (test && now >= next))) {
			VkResult result = present(p);
			if (result != VK_SUCCESS &&
			    result != VK_SUBOPTIMAL_KHR &&
			    result != VK_ERROR_OUT_OF_DATE_KHR &&
			    result != VK_NOT_READY && result != VK_TIMEOUT) {
				fprintf(stderr,
					"Vulkan presentation failed (%d)\n",
					result);
				return false;
			}
			dirty = result == VK_ERROR_OUT_OF_DATE_KHR ||
				result == VK_NOT_READY || result == VK_TIMEOUT;
			next = now + 16;
		}
		SDL_Event event;
		bool received =
			test || (visible && dirty) ?
				SDL_WaitEventTimeout(&event, test ? 8 : 16) :
				SDL_WaitEvent(&event);

		while (received) {
			if (event.type == SDL_QUIT ||
			    (event.type == SDL_KEYDOWN &&
			     event.key.keysym.sym == SDLK_ESCAPE))
				running = false;
			if (event.type == SDL_WINDOWEVENT) {
				switch (event.window.event) {
				case SDL_WINDOWEVENT_CLOSE:
					running = false;
					break;
				case SDL_WINDOWEVENT_SIZE_CHANGED:
				case SDL_WINDOWEVENT_DISPLAY_CHANGED:
				case SDL_WINDOWEVENT_RESTORED:
				case SDL_WINDOWEVENT_SHOWN:
				case SDL_WINDOWEVENT_EXPOSED:
					dirty = true;
					break;
				}
			}
			received = SDL_PollEvent(&event);
		}
	}
	return !test || (p->frames == TEST_FRAMES && p->rebuilds >= 3 &&
			 checkpoints == 3 && !show_at);
}

int main(int argc, char **argv)
{
	char *end = NULL;
	unsigned long vendor = argc > 1 ? strtoul(argv[1], &end, 0) : 0;
	bool test = argc == 3 && !strcmp(argv[2], "--test");

	if (argc > 3 || (argc == 3 && !test) ||
	    (end && (end == argv[1] || *end || vendor > UINT32_MAX))) {
		fprintf(stderr, "Usage: %s [vendor-id [--test]]\n", argv[0]);
		return 2;
	}
	SDL_Window *window = NULL;
	struct preview p = { 0 };
	bool ok = false;

	if (!SDL_Init(SDL_INIT_VIDEO))
		window = SDL_CreateWindow(
			"LINE Vulkan Preview", SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED, 640, 400,
			SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
				SDL_WINDOW_ALLOW_HIGHDPI);
	if (!window) {
		fprintf(stderr, "SDL window: %s\n", SDL_GetError());
		goto done;
	}
	p.presenter = vk_present_open(window, (uint32_t)vendor, true, false);
	if (!p.presenter)
		goto done;
	ok = run(&p, window, test);
	printf("Summary: %u frames, %u drawable sizes, host frame mean %.2f ms, max %.2f ms\n",
	       p.frames, p.rebuilds, p.frames ? p.host_total_ms / p.frames : 0,
	       p.host_max_ms);
done:
	vk_present_close(p.presenter);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return ok ? 0 : 1;
}
