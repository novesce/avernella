/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

struct gpu_rect {
	int x, y, w, h;
};

struct gpu_copy {
	struct gpu_rect src, dst;
};

struct gpu_video {
	const uint8_t *plane[3];
	int stride[3];
	uint32_t width, height, rotation;
	uint64_t generation;
	/* Displayed bounds after clockwise rotation, in drawable pixels. */
	float x, y, w, h;
};

/* All pointers belong to the UI thread and are valid through draw(). */
struct gpu_scene {
	const void *pixels; /* native-endian Cairo RGB24, full backing image */
	uint32_t width, height, pitch;
	struct gpu_rect damage; /* empty for video-only presentation */
	/* Sparse CPU-source to texture-destination updates replace damage. */
	struct gpu_copy uploads[8];
	uint32_t upload_count;
	/* Zero copies means a single quad covering the full canvas. */
	struct gpu_copy copies[8];
	uint32_t copy_count;
	struct gpu_video video[3]; /* peer camera, own camera, shared screen */
};
