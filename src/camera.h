/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"
#include <string.h>

struct line_camera;

struct line_camera_frame {
	const guint8 *plane[3];
	int stride[3];
	unsigned int width, height;
	unsigned int rotation;
};

static inline gboolean line_camera_frame_size_valid(unsigned int width,
						    unsigned int height)
{
	return width >= 2 && height >= 2 && width <= 1920 && height <= 1920 &&
	       width * height <= 1920 * 1080;
}

/* A phone presentation can carry its full portrait display over PDTP. */
static inline gboolean line_preview_size_valid(unsigned int width,
					       unsigned int height,
					       gboolean presentation)
{
	if (!presentation)
		return line_camera_frame_size_valid(width, height);
	return width >= 2 && height >= 2 && width <= 4096 && height <= 4096 &&
	       width * height <= 3840 * 2160;
}

/* Tightly packed I420 has consecutive Y, U and V planes. */
static inline gsize line_camera_i420_size(unsigned int width,
					  unsigned int height)
{
	return (gsize)width * height +
	       2 * (gsize)((width + 1) / 2) * ((height + 1) / 2);
}

static inline void line_camera_i420_frame(struct line_camera_frame *frame,
					  const guint8 *pixels,
					  unsigned int width,
					  unsigned int height)
{
	unsigned int chroma_width = (width + 1) / 2;
	const guint8 *u = pixels + (gsize)width * height;
	const guint8 *v = u + (gsize)chroma_width * ((height + 1) / 2);

	*frame = (struct line_camera_frame){
		.plane = { pixels, u, v },
		.stride = { width, chroma_width, chroma_width },
		.width = width,
		.height = height,
	};
}

static inline void line_camera_pack_i420(guint8 *out,
					 const struct line_camera_frame *frame)
{
	for (unsigned int i = 0; i < 3; i++) {
		unsigned int width = i ? (frame->width + 1) / 2 : frame->width;
		unsigned int rows = i ? (frame->height + 1) / 2 : frame->height;
		const guint8 *row = frame->plane[i];

		if (frame->stride[i] == (int)width) {
			gsize size = (gsize)width * rows;

			memcpy(out, row, size);
			out += size;
			continue;
		}
		for (unsigned int y = 0; y < rows; y++) {
			memcpy(out, row + (gssize)y * frame->stride[i], width);
			out += width;
		}
	}
}

/* Picks the best mode within the size and frame-rate limits. */
struct line_camera *line_camera_open(unsigned int max_width,
				     unsigned int max_height, unsigned int fps,
				     GError **error);
void line_camera_close(struct line_camera *camera);
int line_camera_fd(const struct line_camera *camera);

/* 1 for a new frame, 0 if no frame is ready, -1 on device failure. */
int line_camera_read(struct line_camera *camera,
		     struct line_camera_frame *frame, GError **error);

/* Convert a limited-range I420 frame to native-endian XRGB8888. */
void line_camera_xrgb(const struct line_camera_frame *frame, guint32 *pixels);

/*
 * Converts a decoded JPEG's full-range planes to tightly packed
 * limited-range I420. Each chroma sample covers sx × sy output chroma
 * pixels' worth of source columns and rows (1 or 2 each). The width and
 * height must be even.
 */
void line_camera_jpeg_to_i420(const guint8 *const planes[3], unsigned int width,
			      unsigned int height, unsigned int chroma_stride,
			      unsigned int sx, unsigned int sy, guint8 *out);
