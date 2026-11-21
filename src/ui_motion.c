// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_motion.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

void ui_frame_rate(UiFrameClock *clock, int hz)
{
	if (hz <= 0 || hz > 2000)
		hz = 60;
	if (clock->hz != hz)
		*clock = (UiFrameClock){ .hz = hz };
}

void ui_frame_started(UiFrameClock *clock, gint64 now_us)
{
	if (!clock->hz)
		ui_frame_rate(clock, 60);
	/*
	 * Start each burst at the first actual frame. Preserve phase through late
	 * wakes and slow presentations, skipping missed refreshes in one step.
	 */
	if (!clock->next_us ||
	    now_us - clock->next_us >= G_USEC_PER_SEC / clock->hz)
		clock->origin_us = now_us;
	gint64 slot =
		(now_us - clock->origin_us) * clock->hz / G_USEC_PER_SEC + 1;
	clock->next_us = clock->origin_us +
			 (slot * G_USEC_PER_SEC + clock->hz - 1) / clock->hz;
}

int ui_frame_wait_ms(const UiFrameClock *clock, gint64 now_us,
		     gboolean requested)
{
	if (!requested)
		return -1;
	return (int)CLAMP((clock->next_us - now_us + 999) / 1000, 0, G_MAXINT);
}

void ui_scroll_reset(UiScroll *scroll, int offset)
{
	*scroll = (UiScroll){ .from = offset,
			      .to = offset,
			      .position = offset,
			      .displayed = offset };
}

gboolean ui_scroll_sample(UiScroll *scroll, int *offset, int maximum,
			  gint64 now_us, gboolean reduced)
{
	maximum = MAX(0, maximum);
	if (scroll->displayed != *offset)
		ui_scroll_reset(scroll, *offset);
	scroll->to = CLAMP(scroll->to, 0, maximum);
	if (now_us >= scroll->end_us || reduced) {
		scroll->position = scroll->to;
		scroll->end_us = 0;
	} else {
		/*
		 * Time-based cubic ease-out gives an immediate response to a wheel
		 * impulse and reaches the exact destination after the final sample.
		 */
		double left = (double)(scroll->end_us - now_us) /
			      (scroll->end_us - scroll->start_us);
		scroll->position = scroll->to + (scroll->from - scroll->to) *
							left * left * left;
	}
	scroll->position = CLAMP(scroll->position, 0, maximum);
	*offset = scroll->displayed = (int)(scroll->position + 0.5);
	return scroll->end_us != 0;
}

void ui_scroll_by(UiScroll *scroll, int *offset, double distance, int maximum,
		  gint64 now_us, gboolean reduced)
{
	ui_scroll_sample(scroll, offset, maximum, now_us, reduced);
	if (!isfinite(distance))
		return;
	/*
	 * Same-direction notches accumulate. A reversal starts at the sampled
	 * position so the viewport immediately follows the changed direction.
	 */
	gboolean reversed = (distance < 0 && scroll->to > scroll->position) ||
			    (distance > 0 && scroll->to < scroll->position);
	double base = reversed ? scroll->position : scroll->to;
	double to = CLAMP(base + distance, 0, MAX(0, maximum));

	if (to == scroll->to)
		return;
	scroll->from = scroll->position;
	scroll->to = to;
	scroll->start_us = now_us;
	scroll->end_us = now_us + 120000;
	ui_scroll_sample(scroll, offset, maximum, now_us, reduced);
}

static GSettings *preferences;
static gboolean looked_up, reduced;
static int chosen = -1;

void ui_motion_choose(int reduce)
{
	chosen = reduce < 0 ? -1 : !!reduce;
	ui_motion_refresh_preferences();
}

int ui_motion_choice(void)
{
	return chosen;
}

gboolean ui_reduce_motion(void)
{
	return reduced;
}

void ui_motion_refresh_preferences(void)
{
	const char *override = g_getenv("LINE_REDUCE_MOTION");

	if (override && *override) {
		reduced = strcmp(override, "0") != 0 &&
			  g_ascii_strcasecmp(override, "false") != 0;
		return;
	}
	if (chosen >= 0) {
		reduced = chosen;
		return;
	}
	if (!looked_up) {
		looked_up = TRUE;
		GSettingsSchemaSource *source =
			g_settings_schema_source_get_default();
		GSettingsSchema *schema =
			source ? g_settings_schema_source_lookup(
					 source, "org.gnome.desktop.interface",
					 TRUE) :
				 NULL;
		if (schema) {
			if (g_settings_schema_has_key(schema,
						      "enable-animations"))
				preferences =
					g_settings_new_full(schema, NULL, NULL);
			g_settings_schema_unref(schema);
		}
	}
	reduced = preferences &&
		  !g_settings_get_boolean(preferences, "enable-animations");
}

void ui_motion_shutdown(void)
{
	g_clear_object(&preferences);
	looked_up = FALSE;
}
