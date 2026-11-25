/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL_events.h>
#include <SDL_version.h>
#include <glib.h>

typedef struct {
	int hz;
	gint64 origin_us, next_us;
} UiFrameClock;

void ui_frame_rate(UiFrameClock *clock, int hz);
void ui_frame_started(UiFrameClock *clock, gint64 now_us);
/* -1 means wait for input indefinitely, 0 means a frame is due. */
int ui_frame_wait_ms(const UiFrameClock *clock, gint64 now_us,
		     gboolean requested);

typedef struct {
	double from, to, position;
	int displayed;
	gint64 start_us, end_us;
} UiScroll;

void ui_scroll_reset(UiScroll *scroll, int offset);
void ui_scroll_by(UiScroll *scroll, int *offset, double distance, int maximum,
		  gint64 now_us, gboolean reduced);
/* Ease toward an absolute offset, such as keeping a selected row in view. */
void ui_scroll_to(UiScroll *scroll, int *offset, double target, int maximum,
		  gint64 now_us, gboolean reduced);
gboolean ui_scroll_sample(UiScroll *scroll, int *offset, int maximum,
			  gint64 now_us, gboolean reduced);
gboolean ui_reduce_motion(void);
void ui_motion_refresh_preferences(void);
/* The app's own choice: 1 reduce, 0 animate, -1 follow the desktop. */
void ui_motion_choose(int reduce);
int ui_motion_choice(void);
void ui_motion_shutdown(void);

/* Remaining displacement for a short slide with one soft overshoot. */
double ui_settle(double progress);

/*
 * A displacement that decays to zero with the scroll curve. Adding while it
 * runs keeps what is left, so bursts never jump and never queue up.
 */
typedef struct {
	double from;
	gint64 start_us, end_us;
} UiGlide;

void ui_glide_add(UiGlide *glide, double distance, gint64 duration_us,
		  gint64 now_us, gboolean reduced);
double ui_glide_left(UiGlide *glide, gint64 now_us);

typedef struct {
	gint64 down, up;
} UiPress;

/* A short tap still reaches full depth; holding at rest needs no frames. */
double ui_press_depth(UiPress press, gint64 now, gint64 *deadline);

/*
 * Wheel and touchpad steps toward the start of the content, positive for a
 * traditional wheel turned away from the reader. The desktop has already
 * applied its natural-scrolling setting to SDL's values, and
 * SDL_MOUSEWHEEL_FLIPPED only says so, so the values are used as they come:
 * inverting a flipped event again would undo the desktop's choice. Precise
 * touchpad deltas are preferred over SDL's whole-notch counts.
 */
double ui_wheel_steps(int notches, double precise);

static inline double ui_wheel_y(const SDL_MouseWheelEvent *wheel)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
	return ui_wheel_steps(wheel->y, wheel->preciseY);
#else
	return ui_wheel_steps(wheel->y, 0.);
#endif
}

/* Whole list rows from fractional steps; the carry keeps the remainder. */
int ui_wheel_rows(double steps, double *carry);
