// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vk_cache.h"
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* One UI pipeline. Bound both file input and optional disk use. */
#define CACHE_LIMIT (16u << 20)
#define DIGEST_LEN 64u

static bool valid_blob(const uint8_t *file, size_t size,
		       const VkPhysicalDeviceProperties *props)
{
	uint32_t header[4];

	if (size < DIGEST_LEN + sizeof(header) + VK_UUID_SIZE)
		return false;
	const uint8_t *blob = file + DIGEST_LEN;

	memcpy(header, blob, sizeof(header));
	if (GUINT32_FROM_LE(header[0]) != sizeof(header) + VK_UUID_SIZE ||
	    GUINT32_FROM_LE(header[1]) !=
		    VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
	    GUINT32_FROM_LE(header[2]) != props->vendorID ||
	    GUINT32_FROM_LE(header[3]) != props->deviceID ||
	    memcmp(blob + sizeof(header), props->pipelineCacheUUID,
		   VK_UUID_SIZE))
		return false;
	g_autofree char *digest = g_compute_checksum_for_data(
		G_CHECKSUM_SHA256, blob, size - DIGEST_LEN);
	return !memcmp(file, digest, DIGEST_LEN);
}

static uint8_t *read_cache(const char *path, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	uint8_t *file = NULL;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size <= DIGEST_LEN || st.st_size > CACHE_LIMIT + DIGEST_LEN)
		goto done;
	file = g_try_malloc(st.st_size);
	if (!file)
		goto done;
	size_t used = 0;

	while (used < (size_t)st.st_size) {
		ssize_t n = read(fd, file + used, st.st_size - used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			g_clear_pointer(&file, g_free);
			break;
		}
		used += n;
	}
	*size = used;
done:
	close(fd);
	return file;
}

void vk_cache_open(struct vk_ctx *ctx)
{
	VkPhysicalDeviceProperties props;

	vkGetPhysicalDeviceProperties(ctx->gpu, &props);
	g_autofree char *dir =
		g_build_filename(g_get_user_cache_dir(), "avernella", NULL);
	if (g_mkdir_with_parents(dir, 0700))
		return;
	g_autofree char *name = g_strdup_printf("ui-%04x-%04x-%x-%zu.vkcache",
						props.vendorID, props.deviceID,
						props.driverVersion,
						sizeof(void *));
	ctx->cache_path = g_build_filename(dir, name, NULL);
	size_t size = 0;
	g_autofree uint8_t *file = read_cache(ctx->cache_path, &size);
	VkPipelineCacheCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
	};
	if (file && valid_blob(file, size, &props)) {
		info.pInitialData = file + DIGEST_LEN;
		info.initialDataSize = size - DIGEST_LEN;
		memcpy(ctx->cache_digest, file, DIGEST_LEN);
	}
	if (vkCreatePipelineCache(ctx->device, &info, NULL,
				  &ctx->pipeline_cache) != VK_SUCCESS) {
		info.pInitialData = NULL;
		info.initialDataSize = 0;
		ctx->pipeline_cache = VK_NULL_HANDLE;
		if (vkCreatePipelineCache(ctx->device, &info, NULL,
					  &ctx->pipeline_cache) != VK_SUCCESS)
			ctx->pipeline_cache = VK_NULL_HANDLE;
	}
}

void vk_cache_save(struct vk_ctx *ctx)
{
	if (!ctx->pipeline_cache)
		goto done;
	size_t size = 0;

	if (vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &size,
				   NULL) != VK_SUCCESS ||
	    size <= sizeof(VkPipelineCacheHeaderVersionOne) ||
	    size > CACHE_LIMIT)
		goto destroy;
	uint8_t *file = g_try_malloc(DIGEST_LEN + size);

	if (!file)
		goto destroy;
	if (vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &size,
				   file + DIGEST_LEN) == VK_SUCCESS) {
		g_autofree char *digest = g_compute_checksum_for_data(
			G_CHECKSUM_SHA256, file + DIGEST_LEN, size);
		if (memcmp(ctx->cache_digest, digest, DIGEST_LEN)) {
			memcpy(file, digest, DIGEST_LEN);
			g_file_set_contents_full(ctx->cache_path,
						 (const char *)file,
						 DIGEST_LEN + size,
						 G_FILE_SET_CONTENTS_CONSISTENT,
						 0600, NULL);
		}
	}
	g_free(file);
destroy:
	vkDestroyPipelineCache(ctx->device, ctx->pipeline_cache, NULL);
	ctx->pipeline_cache = VK_NULL_HANDLE;
done:
	g_clear_pointer(&ctx->cache_path, g_free);
}
