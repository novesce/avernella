// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_sounds.h"
#include <SDL.h>

static GMutex lock;
static GCond wake;
static GThread *ui_thread;
static gboolean release_open, release_close;
static guint opened, closing, closed, started;
static SDL_AudioSpec requested;
static gint16 *played;
static gsize samples;

gboolean ui_sounds_enabled(void)
{
	return TRUE;
}

static void wait_for(guint *counter, guint target)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	g_mutex_lock(&lock);
	while (*counter < target)
		if (!g_cond_wait_until(&wake, &lock, deadline))
			break;
	g_assert_cmpuint(*counter, >=, target);
	g_mutex_unlock(&lock);
}

SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *name, int capture,
					     const SDL_AudioSpec *want,
					     SDL_AudioSpec *got, int flags);
SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *name, int capture,
					     const SDL_AudioSpec *want,
					     SDL_AudioSpec *got, int flags)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	g_assert_true(g_thread_self() != ui_thread);
	g_mutex_lock(&lock);
	opened++;
	g_cond_broadcast(&wake);
	while (!release_open)
		if (!g_cond_wait_until(&wake, &lock, deadline))
			break;
	g_assert_true(release_open);
	requested = *want;
	g_mutex_unlock(&lock);
	SDL_AudioDeviceID id =
		__real_SDL_OpenAudioDevice(name, capture, want, got, flags);

	g_assert_cmpuint(id, !=, 0);
	return id;
}

void __wrap_SDL_PauseAudioDevice(SDL_AudioDeviceID id, int pause)
{
	g_assert_true(g_thread_self() != ui_thread);
	g_assert_cmpint(pause, ==, 0);
	g_assert_nonnull(played);
	SDL_LockAudioDevice(id);
	requested.callback(requested.userdata, (Uint8 *)played,
			   samples * sizeof(*played));
	SDL_UnlockAudioDevice(id);
	g_mutex_lock(&lock);
	started++;
	g_cond_broadcast(&wake);
	g_mutex_unlock(&lock);
}

void __real_SDL_CloseAudioDevice(SDL_AudioDeviceID id);
void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID id)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	g_assert_true(g_thread_self() != ui_thread);
	g_mutex_lock(&lock);
	closing++;
	g_cond_broadcast(&wake);
	while (!release_close && g_cond_wait_until(&wake, &lock, deadline))
		;
	g_assert_true(release_close);
	g_mutex_unlock(&lock);
	__real_SDL_CloseAudioDevice(id);
	g_mutex_lock(&lock);
	closed++;
	g_cond_broadcast(&wake);
	g_mutex_unlock(&lock);
}

static void allow_open(void)
{
	g_mutex_lock(&lock);
	release_open = TRUE;
	g_cond_broadcast(&wake);
	g_mutex_unlock(&lock);
}

static void reset(void)
{
	opened = closing = closed = started = 0;
	release_open = FALSE;
	release_close = TRUE;
}

/* A stop and the latest one-shot take effect before the first sample. */
static void call_answered_during_open(void)
{
	g_autofree gint16 *expected = NULL;
	gsize n;

	expected = chip_render(CHIP_CONNECTED, &n);
	reset();
	ui_sound_init();
	g_assert_cmpuint(opened, ==, 0);
	g_assert_cmpuint(SDL_WasInit(SDL_INIT_AUDIO), ==, SDL_INIT_AUDIO);
	samples = n + 512;
	played = g_new0(gint16, samples);
	ui_sound_loop(CHIP_RINGBACK);
	wait_for(&opened, 1);
	ui_sound_play(CHIP_MUTE);
	ui_sound_stop_loop();
	ui_sound_play(CHIP_CONNECTED);
	allow_open();
	wait_for(&started, 1);
	g_assert_cmpmem(played, n * sizeof(*played), expected,
			n * sizeof(*expected));
	for (gsize i = n; i < samples; i++)
		g_assert_cmpint(played[i], ==, 0);
	wait_for(&closed, 1);
	ui_sound_shutdown();
	g_assert_cmpuint(SDL_WasInit(SDL_INIT_AUDIO), ==, 0);
	g_clear_pointer(&played, g_free);
}

/* Ringback which ended during open must never start the device. */
static void call_cancelled_during_open(void)
{
	reset();
	ui_sound_loop(CHIP_RINGBACK);
	wait_for(&opened, 1);
	ui_sound_stop_loop();
	allow_open();
	wait_for(&closed, 1);
	ui_sound_shutdown();
	g_assert_cmpuint(started, ==, 0);
}

/* An idle device may still be closing when a new call sound is needed. */
static void new_sound_during_close(void)
{
	g_autofree gint16 *expected = NULL;
	gsize n;

	expected = chip_render(CHIP_UNMUTE, &n);
	reset();
	release_close = FALSE;
	samples = n;
	played = g_new0(gint16, n);
	ui_sound_loop(CHIP_RINGBACK);
	wait_for(&opened, 1);
	ui_sound_stop_loop();
	allow_open();
	wait_for(&closing, 1);
	ui_sound_play(CHIP_UNMUTE);
	g_mutex_lock(&lock);
	release_close = TRUE;
	g_cond_broadcast(&wake);
	g_mutex_unlock(&lock);
	wait_for(&started, 1);
	g_assert_cmpmem(played, n * sizeof(*played), expected,
			n * sizeof(*expected));
	wait_for(&closed, 2);
	ui_sound_shutdown();
	g_assert_cmpuint(opened, ==, 2);
	g_clear_pointer(&played, g_free);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
	ui_thread = g_thread_self();
	g_test_add_func("/ui/sounds/answered-during-open",
			call_answered_during_open);
	g_test_add_func("/ui/sounds/cancelled-during-open",
			call_cancelled_during_open);
	g_test_add_func("/ui/sounds/new-sound-during-close",
			new_sound_during_close);
	return g_test_run();
}
