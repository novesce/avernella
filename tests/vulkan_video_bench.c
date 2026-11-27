// SPDX-License-Identifier: GPL-3.0-or-later
/* A synthetic decoded 1080p call, rendered through the real WSI and GPU. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "render/vk_canvas.h"
#include "nv12_fixture.h"
#include <SDL.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	WIDTH = 1920,
	HEIGHT = 1080,
	WARMUP = 30,
	SAMPLES = 240
};

static PFN_vkBeginCommandBuffer begin_command;
static PFN_vkEndCommandBuffer end_command;
static PFN_vkQueueSubmit2 submit_commands;
static VkQueryPool queries;
static unsigned int query;
static uint64_t submit_ticks;
static struct nv12_fixture surfaces[3];
static uint8_t planes[WIDTH * HEIGHT * 3 / 2];

static VkResult VKAPI_CALL begin(VkCommandBuffer cmd,
				 const VkCommandBufferBeginInfo *info)
{
	VkResult result = begin_command(cmd, info);

	if (result == VK_SUCCESS) {
		vkCmdResetQueryPool(cmd, queries, query, 2);
		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
				     queries, query);
	}
	return result;
}

static VkResult VKAPI_CALL end(VkCommandBuffer cmd)
{
	vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
			     queries, query + 1);
	return end_command(cmd);
}

static VkResult VKAPI_CALL submit(VkQueue queue, uint32_t count,
				  const VkSubmitInfo2 *info, VkFence fence)
{
	uint64_t start = SDL_GetPerformanceCounter();
	VkResult result = submit_commands(queue, count, info, fence);

	submit_ticks = SDL_GetPerformanceCounter() - start;
	return result;
}

static int compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void distribution(const char *name, double data[SAMPLES])
{
	qsort(data, SAMPLES, sizeof(*data), compare);
	printf("%-22s %.3f / %.3f / %.3f ms\n", name,
	       data[(50 * SAMPLES + 99) / 100 - 1],
	       data[(95 * SAMPLES + 99) / 100 - 1],
	       data[(99 * SAMPLES + 99) / 100 - 1]);
}

static bool pace(uint64_t epoch, unsigned int frame)
{
	uint64_t ns = epoch + (uint64_t)frame * 1000000000 / 60;
	struct timespec time = { ns / 1000000000, ns % 1000000000 };
	int result;

	do {
		result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time,
					 NULL);
	} while (result == EINTR);
	return result == 0;
}

static bool benchmark(struct vk_present *p, struct vk_canvas *canvas,
		      bool paced, bool dmabuf)
{
	struct vk_ctx *ctx = vk_present_context(p);
	VkPhysicalDeviceProperties props;
	uint32_t families = 0;
	uint64_t stamps[2 * (SAMPLES + WARMUP)];
	double host[SAMPLES], gpu[SAMPLES], queue[SAMPLES];
	uint64_t deadline = SDL_GetTicks64() + 30000;
	double ms_per_tick = 1000.0 / SDL_GetPerformanceFrequency();
	VkExtent2D extent = vk_present_extent(p);

	vkGetPhysicalDeviceProperties(ctx->gpu, &props);
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->gpu, &families, NULL);
	VkQueueFamilyProperties *family = calloc(families, sizeof(*family));

	if (!family || ctx->family >= families) {
		free(family);
		return false;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->gpu, &families, family);
	uint32_t bits = family[ctx->family].timestampValidBits;

	free(family);
	if (!bits || bits > 64)
		return false;
	uint64_t mask = UINT64_MAX >> (64 - bits);
	VkQueryPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = 2 * (SAMPLES + WARMUP),
	};
	if (vkCreateQueryPool(ctx->device, &pool, NULL, &queries) != VK_SUCCESS)
		return false;

	struct gpu_scene s = {
		.width = extent.width,
		.height = extent.height,
		.pitch = extent.width * 4,
	};
	uint32_t *background = calloc(s.width, (size_t)s.height * 4);

	s.pixels = background;
	s.video[0] = (struct gpu_video){
		.plane = { planes, planes + WIDTH * HEIGHT,
			   planes + WIDTH * HEIGHT * 5 / 4 },
		.stride = { WIDTH, WIDTH / 2, WIDTH / 2 },
		.width = WIDTH,
		.height = HEIGHT,
		.w = s.width,
		.h = s.height,
	};
	for (size_t i = 0; i < sizeof(planes); i++)
		planes[i] = 16 + (i % 220);
	bool ready = background != NULL;

	for (unsigned int j = 0; dmabuf && ready && j < 3; j++) {
		/* Producer-owned immutable images, rotated through the same
		 * surface pool as a decoder. Never rewrite a retained lease.
		 */
		/* Model a decoder's padded allocation, distinct from the crop. */
		enum {
			PITCH = 2048,
			ROWS = 1088
		};
		ready = nv12_fixture_open(&surfaces[j], PITCH, ROWS);
		if (!ready)
			break;
		uint8_t *out = surfaces[j].pixels;

		for (unsigned int row = 0; row < HEIGHT; row++)
			memcpy(out + PITCH * row, planes + WIDTH * row, WIDTH);
		out[0] = 16 + j;
		for (unsigned int row = 0; row < HEIGHT / 2; row++) {
			uint8_t *uv = out + PITCH * (ROWS + row);

			for (unsigned int col = 0; col < WIDTH / 2; col++) {
				size_t k = row * (WIDTH / 2) + col;

				uv[2 * col] = s.video[0].plane[1][k];
				uv[2 * col + 1] = s.video[0].plane[2][k];
			}
		}
		ready = nv12_fixture_sync(&surfaces[j], DMA_BUF_SYNC_END);
	}
	begin_command = vkBeginCommandBuffer;
	end_command = vkEndCommandBuffer;
	submit_commands = vkQueueSubmit2;
	vkBeginCommandBuffer = begin;
	vkEndCommandBuffer = end;
	vkQueueSubmit2 = submit;

	bool ok = false;
	unsigned int attempts = 0, missed = 0;
	uint64_t first_attempt = 0;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t epoch = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;

	for (unsigned int i = 0; ready && i < SAMPLES + WARMUP;) {
		if (SDL_GetTicks64() >= deadline)
			goto done;
		if (!first_attempt) {
			if (paced && !pace(epoch, i))
				goto done;
			first_attempt = SDL_GetPerformanceCounter();
		}
		SDL_PumpEvents();
		query = 2 * i;
		s.video[0].generation = i + 1;
		planes[0] = 16 + i % 220;
		unsigned int rejected = 0;
		const struct gpu_dmabuf *frames[] = { &surfaces[i % 3].frame,
						      NULL, NULL };
		VkResult result = vk_canvas_draw_dmabuf(
			canvas, &s, dmabuf ? frames : NULL, &rejected);
		if (rejected) {
			fputs("This GPU rejected the synthetic linear NV12 modifier.\n",
			      stderr);
			goto done;
		}
		uint64_t elapsed = SDL_GetPerformanceCounter() - first_attempt;

		attempts++;
		if (result == VK_TIMEOUT || result == VK_NOT_READY) {
			SDL_Delay(1);
			continue;
		}
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			goto done;
		if (i >= WARMUP) {
			host[i - WARMUP] = elapsed * ms_per_tick;
			queue[i - WARMUP] = submit_ticks * ms_per_tick;
			if (paced) {
				clock_gettime(CLOCK_MONOTONIC, &now);
				uint64_t ns =
					(uint64_t)now.tv_sec * 1000000000 +
					now.tv_nsec;
				missed += ns - epoch >
					  (uint64_t)(i + 1) * 1000000000 / 60;
			}
		}
		first_attempt = 0;
		i++;
	}
	if (!ready || vk_present_wait(p, UINT64_MAX) != VK_SUCCESS ||
	    vkGetQueryPoolResults(ctx->device, queries, 0, pool.queryCount,
				  sizeof(stamps), stamps, sizeof(*stamps),
				  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
		goto done;
	for (unsigned int i = 0; i < SAMPLES; i++) {
		unsigned int n = 2 * (i + WARMUP);
		uint64_t duration = (stamps[n + 1] - stamps[n]) & mask;

		gpu[i] = duration * (props.limits.timestampPeriod / 1000000.);
	}
	printf("%s, %ux%u viewport, 1080p %s, %u frames, %u retries\n",
	       props.deviceName, s.width, s.height,
	       dmabuf ? "NV12 dma-buf" : "I420 upload", SAMPLES,
	       attempts - SAMPLES - WARMUP);
	if (paced)
		printf("60 fps feed, %u/%u host frame deadlines missed\n",
		       missed, SAMPLES);
	puts("                             p50 / p95 / p99");
	distribution("GPU command execution", gpu);
	distribution("Host queue submit", queue);
	distribution("Host draw and present", host);
	ok = true;
done:
	vkBeginCommandBuffer = begin_command;
	vkEndCommandBuffer = end_command;
	vkQueueSubmit2 = submit_commands;
	vkDeviceWaitIdle(ctx->device);
	vkDestroyQueryPool(ctx->device, queries, NULL);
	free(background);
	return ok;
}

int main(int argc, char **argv)
{
	bool paced = true, dmabuf = false;

	for (int j = 1; j < argc; j++) {
		if (!strcmp(argv[j], "--saturate"))
			paced = false;
		else if (!strcmp(argv[j], "--dmabuf"))
			dmabuf = true;
		else {
			fprintf(stderr, "Usage: %s [--saturate] [--dmabuf]\n",
				argv[0]);
			return 2;
		}
	}
	if (SDL_Init(SDL_INIT_VIDEO))
		return 1;
	SDL_Window *window = SDL_CreateWindow(
		"Avernella synthetic video benchmark", SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_VULKAN);
	struct vk_present *present =
		window ? vk_present_open(window, 0, true, true) : NULL;
	struct vk_canvas *canvas = present ? vk_canvas_open(present) : NULL;
	bool ok = canvas && benchmark(present, canvas, paced, dmabuf);

	vk_canvas_close(canvas);
	vk_present_close(present);
	for (unsigned int j = 0; j < 3; j++) {
		ok &= !surfaces[j].refs;
		nv12_fixture_close(&surfaces[j]);
	}
	SDL_DestroyWindow(window);
	SDL_Quit();
	return ok ? 0 : 1;
}
