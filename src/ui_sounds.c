// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Sound effects for calls and notifications. Each sound is rendered once,
 * on first use, from the emulated NES APU (under 10 ms) and kept as PCM.
 *
 * The UI publishes the desired loop and latest one-shot. A sound thread
 * owns the device, including backend operations that may block. It sleeps
 * until a request, or until it can close the device after the last effect.
 */
#include "ui_sounds.h"
#include "ui_chat_lists.h"
#include <SDL.h>
#include <string.h>

struct voice {
	const gint16 *pcm;
	gsize n, pos;
	gboolean loop;
};

static struct {
	SDL_AudioDeviceID device;
	gboolean failed, playing;
	struct voice loop, effect; /* guarded by the device lock */
	gint16 *pcm[CHIP_SOUNDS];
	gsize n[CHIP_SOUNDS];
} snd;

static struct {
	GMutex lock;
	GCond wake;
	GThread *thread;
	enum chip_sound loop, effect; /* CHIP_SOUNDS means none */
	gboolean changed, stop;
	gboolean audio_init, init_checked; /* main-thread subsystem lifecycle */
} control = { .loop = CHIP_SOUNDS, .effect = CHIP_SOUNDS };

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

static void open_device(void)
{
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
	}
}

static const gint16 *sound_pcm(enum chip_sound sound, gsize *n)
{
	if (!snd.pcm[sound])
		snd.pcm[sound] = chip_render(sound, &snd.n[sound]);
	*n = snd.n[sound];
	return snd.pcm[sound];
}

static gint64 apply_sounds(enum chip_sound loop, enum chip_sound effect)
{
	struct voice next = { .loop = TRUE }, shot = { 0 };

	if (loop < CHIP_SOUNDS)
		next.pcm = sound_pcm(loop, &next.n);
	if (effect < CHIP_SOUNDS)
		shot.pcm = sound_pcm(effect, &shot.n);
	SDL_LockAudioDevice(snd.device);
	if (next.pcm != snd.loop.pcm)
		snd.loop = next;
	if (shot.pcm)
		snd.effect = shot;
	gboolean looping = snd.loop.pcm != NULL;
	gboolean busy = looping || snd.effect.pcm;

	SDL_UnlockAudioDevice(snd.device);
	if (!busy) {
		SDL_CloseAudioDevice(snd.device);
		snd.device = 0;
		snd.playing = FALSE;
	} else if (!snd.playing) {
		SDL_PauseAudioDevice(snd.device, 0);
		snd.playing = TRUE;
	}
	return busy && !looping ? g_get_monotonic_time() + 500000 : 0;
}

static gpointer playback(gpointer unused)
{
	gint64 idle_check = 0;

	(void)unused;
	g_mutex_lock(&control.lock);
	for (;;) {
		while (!control.changed && !control.stop) {
			if (!idle_check)
				g_cond_wait(&control.wake, &control.lock);
			else if (!g_cond_wait_until(&control.wake,
						    &control.lock, idle_check))
				break;
		}
		if (control.stop)
			break;
		if (!snd.device && !snd.failed &&
		    (control.loop < CHIP_SOUNDS ||
		     control.effect < CHIP_SOUNDS)) {
			g_mutex_unlock(&control.lock);
			open_device();
			g_mutex_lock(&control.lock);
			/* Ringing may have ended while the backend was opening. */
			continue;
		}
		enum chip_sound loop = control.loop;
		enum chip_sound effect = control.effect;

		control.changed = FALSE;
		control.effect = CHIP_SOUNDS;
		g_mutex_unlock(&control.lock);
		idle_check = snd.device ? apply_sounds(loop, effect) : 0;
		g_mutex_lock(&control.lock);
	}
	g_mutex_unlock(&control.lock);
	if (snd.device)
		SDL_CloseAudioDevice(snd.device);
	for (int i = 0; i < CHIP_SOUNDS; i++)
		g_free(snd.pcm[i]);
	memset(&snd, 0, sizeof(snd));
	return NULL;
}

void ui_sound_init(void)
{
	if (!control.init_checked) {
		control.audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
		control.init_checked = TRUE;
	}
}

static void request(enum chip_sound sound, gboolean loop)
{
	g_mutex_lock(&control.lock);
	if (loop && control.loop == sound)
		goto out;
	if (!control.thread) {
		ui_sound_init();
		if (!control.audio_init)
			goto out;
		control.thread = g_thread_try_new("avernella-sound", playback,
						  NULL, NULL);
		if (!control.thread)
			goto out;
	}
	if (loop)
		control.loop = sound;
	else
		control.effect = sound;
	control.changed = TRUE;
	g_cond_signal(&control.wake);
out:
	g_mutex_unlock(&control.lock);
}

void ui_sound_play(enum chip_sound sound)
{
	if (sound < CHIP_SOUNDS && ui_sounds_enabled())
		request(sound, FALSE);
}

void ui_sound_loop(enum chip_sound sound)
{
	if (sound < CHIP_SOUNDS && ui_sounds_enabled())
		request(sound, TRUE);
}

void ui_sound_stop_loop(void)
{
	request(CHIP_SOUNDS, TRUE);
}

void ui_sound_shutdown(void)
{
	if (control.thread) {
		g_mutex_lock(&control.lock);
		control.stop = TRUE;
		g_cond_signal(&control.wake);
		g_mutex_unlock(&control.lock);
		g_thread_join(control.thread);
		control.thread = NULL;
	}
	if (control.audio_init)
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	control.audio_init = control.init_checked = FALSE;
	control.stop = control.changed = FALSE;
	control.loop = control.effect = CHIP_SOUNDS;
}
