/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "chiptune.h"

/*
 * Plays the app's chiptune sounds (chiptune.c). One looping sound (the
 * ringtone or ringback) and one effect can sound at once. Nothing plays
 * while Sounds is off in settings. UI thread only.
 */
void ui_sound_play(enum chip_sound sound);
/* Starts a looping sound, replacing any other loop. */
void ui_sound_loop(enum chip_sound sound);
void ui_sound_stop_loop(void);
/* Closes the audio device; for exit and sign-out. */
void ui_sound_shutdown(void);
