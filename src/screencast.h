/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "camera.h"

/*
 * Desktop capture through the XDG Desktop Portal ScreenCast interface and
 * PipeWire. The portal shows its own picker, which works on Wayland and on
 * X11 desktops that run xdg-desktop-portal. Frames arrive as fitted, packed I420, or as
 * leased NV12 DMA-BUFs at the exact size and modifier accepted by the encoder.
 */
struct line_screencast;
struct spa_buffer;
GBytes *line_screencast_planar(const struct spa_buffer *buf, guint w, guint h);

enum line_screencast_state {
	LINE_SCREENCAST_STARTING,
	LINE_SCREENCAST_RUNNING,
	LINE_SCREENCAST_STOPPED,
	LINE_SCREENCAST_FAILED,
};

typedef gboolean (*line_cast_probe_fn)(guint width, guint height, guint fps,
				       guint kbps, guint64 *modifier);

/* Starts the portal request on a worker thread; returns immediately. */
struct line_screencast *line_screencast_start(unsigned int max_width,
					      unsigned int max_height,
					      unsigned int fps, guint kbps,
					      line_cast_probe_fn probe,
					      GError **error);
void line_screencast_stop(struct line_screencast *cast);
/* Readable when a frame or a state change is waiting. */
int line_screencast_fd(const struct line_screencast *cast);
/* Clears the wakeup and reports the state; 'message' explains a failure. */
enum line_screencast_state line_screencast_poll(struct line_screencast *cast,
						char **message);
/* Takes the newest captured frame, or NULL; 'time_ns' is its capture time. */
GBytes *line_screencast_take(struct line_screencast *cast, unsigned int *width,
			     unsigned int *height, guint64 *time_ns,
			     gboolean *dma);

/*
 * Converts packed 32-bit pixels to I420 of the given size with bilinear
 * filtering. 'bgr' is TRUE for BGRx/BGRA byte order and FALSE for RGBx/RGBA.
 */
void line_screencast_to_i420(const guint8 *src, unsigned int width,
			     unsigned int height, unsigned int stride,
			     gboolean bgr, guint8 *dst, unsigned int out_width,
			     unsigned int out_height);
/* Fits width x height within the limits, keeping even dimensions. */
void line_screencast_fit(unsigned int width, unsigned int height,
			 unsigned int max_width, unsigned int max_height,
			 unsigned int *out_width, unsigned int *out_height);
