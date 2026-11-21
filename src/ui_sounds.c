// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Sound effects for calls and notifications. Each sound is rendered once,
 * on first use, from the emulated NES APU (under 10 ms) and kept as PCM.
 *
 * Playback opens an SDL audio device only when a sound starts. Its
 * callback mixes the looping voice and the effect voice, and once both
 * have finished an SDL timer closes the device: no audio thread waking
 * every few milliseconds to write silence, and no idle PipeWire stream.
 * (Pausing is not enough: SDL 3 keeps a paused stream running.)
 */
#include "ui_sounds.h"
#include "ui_chat_lists.h"
#include <SDL.h>

struct voice {
	const gint16 *pcm;
	gsize n, pos;
	gboolean loop;
};

static struct {
	GMutex lock; /* orders opening, starting and closing the device */
	SDL_AudioDeviceID device;
	SDL_TimerID idle_timer;
	gboolean audio_init, failed;
	struct voice loop, effect; /* guarded by the device lock */
	gint16 *pcm[CHIP_SOUNDS];
	gsize n[CHIP_SOUNDS];
} snd;

static void mix(void *data, Uint8 *stream, int len)
{
	gint16 *out = (gint16 *)stream;
	int samples = len / (int)sizeof(*out);
	struct voice *voices[2] = { &snd.loop, &snd.effect };

	(void)data;
	for (int i = 0; i < samples; i++) {
		int sum = 0;

		for (int v = 0; v < 2; v++) {
			struct voice *voice = voices[v];

			if (!voice->pcm)
				continue;
			sum += voice->pcm[voice->pos++];
			if (voice->pos < voice->n)
				continue;
			if (voice->loop)
				voice->pos = 0;
			else
				voice->pcm = NULL;
		}
		out[i] = (gint16)CLAMP(sum, -32767, 32767);
	}
}

/* Closes the device once both voices have finished. Timer thread. */
static Uint32 idle_check(Uint32 interval, void *data)
{
	(void)data;
	g_mutex_lock(&snd.lock);
	SDL_LockAudioDevice(snd.device);
	gboolean busy = snd.loop.pcm || snd.effect.pcm;

	SDL_UnlockAudioDevice(snd.device);
	if (!busy) {
		SDL_CloseAudioDevice(snd.device);
		snd.device = 0;
		snd.idle_timer = 0;
		interval = 0;
	}
	g_mutex_unlock(&snd.lock);
	return interval;
}

static gboolean open_device(void)
{
	if (snd.device)
		return TRUE;
	if (snd.failed)
		return FALSE;
	if (!snd.audio_init) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			snd.failed = TRUE;
			return FALSE;
		}
		snd.audio_init = TRUE;
	}
	/* PipeWire lists the stream by name and treats it as an event sound. */
	SDL_SetHint("SDL_AUDIO_DEVICE_STREAM_NAME", "Avernella sounds");
	SDL_SetHint("SDL_AUDIO_DEVICE_STREAM_ROLE", "Notification");
	SDL_AudioSpec want = {
		.freq = CHIP_RATE,
		.format = AUDIO_S16SYS,
		.channels = 1,
		.samples = 512, /* about 11 ms */
		.callback = mix,
	};

	snd.device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if (!snd.device) {
		g_debug("No audio for sounds: %s", SDL_GetError());
		snd.failed = TRUE;
		return FALSE;
	}
	return TRUE;
}

static const gint16 *sound_pcm(enum chip_sound sound, gsize *n)
{
	if (!snd.pcm[sound])
		snd.pcm[sound] = chip_render(sound, &snd.n[sound]);
	*n = snd.n[sound];
	return snd.pcm[sound];
}

static void start(struct voice *voice, enum chip_sound sound, gboolean loop)
{
	gsize n;
	const gint16 *pcm;

	if (sound >= CHIP_SOUNDS || !ui_sounds_enabled())
		return;
	pcm = sound_pcm(sound, &n);
	if (!pcm || !n)
		return;
	g_mutex_lock(&snd.lock);
	if (!open_device()) {
		g_mutex_unlock(&snd.lock);
		return;
	}
	SDL_LockAudioDevice(snd.device);
	*voice = (struct voice){ .pcm = pcm, .n = n, .loop = loop };
	SDL_UnlockAudioDevice(snd.device);
	SDL_PauseAudioDevice(snd.device, 0);
	if (!snd.idle_timer)
		snd.idle_timer = SDL_AddTimer(500, idle_check, NULL);
	g_mutex_unlock(&snd.lock);
}

void ui_sound_play(enum chip_sound sound)
{
	start(&snd.effect, sound, FALSE);
}

void ui_sound_loop(enum chip_sound sound)
{
	g_mutex_lock(&snd.lock);
	gboolean playing = sound < CHIP_SOUNDS && snd.device && snd.loop.pcm &&
			   snd.loop.pcm == snd.pcm[sound];

	g_mutex_unlock(&snd.lock);
	if (!playing)
		start(&snd.loop, sound, TRUE);
}

void ui_sound_stop_loop(void)
{
	g_mutex_lock(&snd.lock);
	if (snd.device) {
		SDL_LockAudioDevice(snd.device);
		snd.loop.pcm = NULL;
		SDL_UnlockAudioDevice(snd.device);
	}
	g_mutex_unlock(&snd.lock);
}

void ui_sound_shutdown(void)
{
	g_mutex_lock(&snd.lock);
	if (snd.idle_timer) {
		SDL_RemoveTimer(snd.idle_timer);
		snd.idle_timer = 0;
	}
	if (snd.device) {
		SDL_CloseAudioDevice(snd.device);
		snd.device = 0;
	}
	g_mutex_unlock(&snd.lock);
	snd.loop.pcm = snd.effect.pcm = NULL;
	for (int i = 0; i < CHIP_SOUNDS; i++)
		g_clear_pointer(&snd.pcm[i], g_free);
}
