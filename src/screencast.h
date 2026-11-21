/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "camera.h"

/*
 * Desktop capture through the XDG Desktop Portal ScreenCast interface and
 * PipeWire. The portal shows its own picker, which works on Wayland and on
 * X11 desktops that run xdg-desktop-portal. Frames arrive as tightly packed
 * I420, scaled to fit the requested limits.
 */
struct line_screencast;

enum line_screencast_state {
	LINE_SCREENCAST_STARTING,
	LINE_SCREENCAST_RUNNING,
	LINE_SCREENCAST_STOPPED,
	LINE_SCREENCAST_FAILED,
};

/* Starts the portal request on a worker thread; returns immediately. */
struct line_screencast *line_screencast_start(unsigned int max_width,
					      unsigned int max_height,
					      unsigned int fps, GError **error);
void line_screencast_stop(struct line_screencast *cast);
/* Readable when a frame or a state change is waiting. */
int line_screencast_fd(const struct line_screencast *cast);
/* Clears the wakeup and reports the state; 'message' explains a failure. */
enum line_screencast_state line_screencast_poll(struct line_screencast *cast,
						char **message);
/* Takes the newest captured frame, or NULL; 'time_ns' is its capture time. */
GBytes *line_screencast_take(struct line_screencast *cast, unsigned int *width,
			     unsigned int *height, guint64 *time_ns);

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
