/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/*
 * The app's sound effects, played on an emulated NES sound chip (2A03 APU:
 * two pulse channels, a triangle and a noise channel). The channel logic is
 * ported from Mesen (see chiptune.c); each sound is a short tune written as
 * register writes, rendered once to PCM.
 */
enum chip_sound {
	CHIP_RINGTONE, /* incoming call, loops */
	CHIP_RINGBACK, /* outgoing call ringing, loops */
	CHIP_CONNECTED, /* call answered */
	CHIP_HANGUP, /* call ended */
	CHIP_CAMERA_ON,
	CHIP_CAMERA_OFF,
	CHIP_SHARE_START, /* screen sharing started */
	CHIP_SHARE_STOP,
	CHIP_MUTE,
	CHIP_UNMUTE,
	CHIP_MESSAGE, /* new message notification */
	CHIP_SOUNDS
};

#define CHIP_RATE 48000

/* Whether the sound repeats; its rendering is then exactly one period. */
gboolean chip_loops(enum chip_sound sound);

/*
 * Mono signed 16-bit PCM at CHIP_RATE. *samples receives the length.
 * Free with g_free.
 */
gint16 *chip_render(enum chip_sound sound, gsize *samples);
