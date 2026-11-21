// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NES APU (2A03) sound for the app's ringtone and effects.
 *
 * The channel behaviour is ported to C from Mesen (MesenCE, commit
 * a8e5d02, Core/NES/APU and Core/NES/NesSoundMixer.cpp), copyright
 * Sour and the Mesen contributors, GPL-3.0-or-later:
 * https://github.com/nesdev-org/MesenCE
 * That covers the duty sequences, triangle sequence, noise LFSR and period
 * table, length counter table, envelope, sweep unit, frame counter steps
 * and the non-linear mixer. Mesen clocks every CPU cycle and resamples
 * with blip_buf; here the timers run event by event and each output
 * sample is the exact average of the mixer over its CPU cycles.
 */
#include "chiptune.h"
#include <math.h>
#include <string.h>

#define CPU_HZ 1789773.0 /* NTSC 2A03 */
#define FRAME_CYCLES 29781 /* one 60 Hz sequencer frame */
#define NEVER G_MAXUINT64

static const guint8 duty_sequences[4][8] = {
	{ 0, 0, 0, 0, 0, 0, 0, 1 },
	{ 0, 0, 0, 0, 0, 0, 1, 1 },
	{ 0, 0, 0, 0, 1, 1, 1, 1 },
	{ 1, 1, 1, 1, 1, 1, 0, 0 },
};

static const guint8 triangle_sequence[32] = {
	15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5,	4,  3,	2,  1,	0,
	0,  1,	2,  3,	4,  5,	6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

static const guint16 noise_periods[16] = {
	4,   8,	  16,  32,  64,	 96,   128,  160,
	202, 254, 380, 508, 762, 1016, 2034, 4068,
};

static const guint8 length_table[32] = {
	10, 254, 20, 2,	 40, 4,	 80, 6,	 160, 8,  60, 10, 14, 12, 26, 14,
	12, 16,	 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

/* Frame counter, 4-step mode: quarter frames, and half frames on 2 and 4. */
static const guint32 frame_steps[4] = { 7457, 14913, 22371, 29829 };
#define FRAME_PERIOD 29830

struct length {
	gboolean enabled, halt, new_halt;
	guint8 counter, reload, previous;
};

struct envelope {
	gboolean constant, start;
	guint8 volume, counter;
	gint8 divider;
	struct length length;
};

struct timer {
	guint16 period;
	guint64 next; /* CPU cycle of the next step, or NEVER */
};

struct pulse {
	struct envelope env;
	struct timer timer;
	gboolean first, sweep_enabled, sweep_negate, reload_sweep;
	guint8 duty, pos, sweep_period, sweep_shift, sweep_divider;
	guint16 real_period;
	guint32 sweep_target;
	guint8 out;
};

struct triangle {
	struct length length;
	struct timer timer;
	gboolean linear_reload, linear_control;
	guint8 linear, linear_load, pos, out;
};

struct noise {
	struct envelope env;
	struct timer timer;
	guint16 shift;
	gboolean mode;
	guint8 out;
};

struct apu {
	struct pulse pulse[2];
	struct triangle triangle;
	struct noise noise;
	guint64 now;
	guint64 frame_base;
	guint step;
};

/* Length counter (ApuLengthCounter). */
static void length_load(struct length *l, guint8 index)
{
	if (l->enabled) {
		l->reload = length_table[index & 31];
		l->previous = l->counter;
	}
}

static void length_commit(struct length *l)
{
	if (l->reload) {
		if (l->counter == l->previous)
			l->counter = l->reload;
		l->reload = 0;
	}
	l->halt = l->new_halt;
}

static void length_tick(struct length *l)
{
	if (l->counter > 0 && !l->halt)
		l->counter--;
}

static void length_enable(struct length *l, gboolean on)
{
	if (!on)
		l->counter = 0;
	l->enabled = on;
}

/* Envelope (ApuEnvelope). */
static void envelope_init(struct envelope *e, guint8 value)
{
	e->length.new_halt = (value & 0x20) != 0;
	e->constant = (value & 0x10) != 0;
	e->volume = value & 0x0f;
}

static guint8 envelope_volume(const struct envelope *e)
{
	if (!e->length.counter)
		return 0;
	return e->constant ? e->volume : e->counter;
}

static void envelope_tick(struct envelope *e)
{
	if (e->start) {
		e->start = FALSE;
		e->counter = 15;
		e->divider = (gint8)e->volume;
		return;
	}
	if (--e->divider < 0) {
		e->divider = (gint8)e->volume;
		if (e->counter > 0)
			e->counter--;
		else if (e->length.halt)
			e->counter = 15;
	}
}

/* Pulse channel (SquareChannel). */
static gboolean pulse_muted(const struct pulse *p)
{
	return p->real_period < 8 ||
	       (!p->sweep_negate && p->sweep_target > 0x7ff);
}

static void pulse_target(struct pulse *p)
{
	guint16 shifted = p->real_period >> p->sweep_shift;

	if (p->sweep_negate)
		p->sweep_target = p->real_period - shifted - (p->first ? 1 : 0);
	else
		p->sweep_target = p->real_period + shifted;
}

static void pulse_period(struct pulse *p, guint16 period)
{
	p->real_period = period;
	p->timer.period = period * 2 + 1;
	pulse_target(p);
}

static void pulse_output(struct pulse *p)
{
	p->out = pulse_muted(p) ? 0 :
				  duty_sequences[p->duty][p->pos] *
					  envelope_volume(&p->env);
}

static void pulse_write(struct pulse *p, guint reg, guint8 value)
{
	switch (reg) {
	case 0:
		envelope_init(&p->env, value);
		p->duty = value >> 6;
		break;
	case 1:
		p->sweep_enabled = (value & 0x80) != 0;
		p->sweep_negate = (value & 0x08) != 0;
		p->sweep_period = ((value & 0x70) >> 4) + 1;
		p->sweep_shift = value & 0x07;
		pulse_target(p);
		p->reload_sweep = TRUE;
		break;
	case 2:
		pulse_period(p, (p->real_period & 0x700) | value);
		break;
	case 3:
		length_load(&p->env.length, value >> 3);
		pulse_period(p, (p->real_period & 0xff) | ((value & 7) << 8));
		p->pos = 0;
		p->env.start = TRUE;
		break;
	}
	pulse_output(p);
}

static void pulse_sweep(struct pulse *p)
{
	if (--p->sweep_divider == 0) {
		if (p->sweep_shift > 0 && p->sweep_enabled &&
		    p->real_period >= 8 && p->sweep_target <= 0x7ff)
			pulse_period(p, (guint16)p->sweep_target);
		p->sweep_divider = p->sweep_period;
	}
	if (p->reload_sweep) {
		p->sweep_divider = p->sweep_period;
		p->reload_sweep = FALSE;
	}
}

static void pulse_step(struct pulse *p)
{
	p->pos = (p->pos - 1) & 7;
	pulse_output(p);
}

/* Triangle channel (TriangleChannel). */
static void triangle_write(struct triangle *t, guint reg, guint8 value)
{
	switch (reg) {
	case 0:
		t->linear_control = (value & 0x80) != 0;
		t->linear_load = value & 0x7f;
		t->length.new_halt = t->linear_control;
		break;
	case 2:
		t->timer.period = (t->timer.period & 0xff00) | value;
		break;
	case 3:
		length_load(&t->length, value >> 3);
		t->timer.period = (t->timer.period & 0xff) | ((value & 7) << 8);
		t->linear_reload = TRUE;
		break;
	}
}

static void triangle_linear(struct triangle *t)
{
	if (t->linear_reload)
		t->linear = t->linear_load;
	else if (t->linear > 0)
		t->linear--;
	if (!t->linear_control)
		t->linear_reload = FALSE;
}

static void triangle_step(struct triangle *t)
{
	if (t->length.counter && t->linear > 0) {
		t->pos = (t->pos + 1) & 31;
		t->out = triangle_sequence[t->pos];
	}
}

/* Noise channel (NoiseChannel). */
static void noise_write(struct noise *n, guint reg, guint8 value)
{
	switch (reg) {
	case 0:
		envelope_init(&n->env, value);
		break;
	case 2:
		n->timer.period = noise_periods[value & 15] - 1;
		n->mode = (value & 0x80) != 0;
		break;
	case 3:
		length_load(&n->env.length, value >> 3);
		n->env.start = TRUE;
		break;
	}
}

static void noise_step(struct noise *n)
{
	guint16 feedback = (n->shift & 1) ^
			   ((n->shift >> (n->mode ? 6 : 1)) & 1);

	n->shift = (n->shift >> 1) | (feedback << 14);
	n->out = (n->shift & 1) ? 0 : envelope_volume(&n->env);
}

/*
 * A channel's timer only matters while the channel can sound. Parking it
 * otherwise keeps a silent pulse with period 0 from costing a step per
 * CPU cycle.
 */
static void schedule(struct apu *a)
{
	struct timer *timers[4] = { &a->pulse[0].timer, &a->pulse[1].timer,
				    &a->triangle.timer, &a->noise.timer };
	gboolean live[4] = {
		a->pulse[0].env.length.counter && !pulse_muted(&a->pulse[0]),
		a->pulse[1].env.length.counter && !pulse_muted(&a->pulse[1]),
		a->triangle.length.counter && a->triangle.linear &&
			a->triangle.timer.period >= 2,
		a->noise.env.length.counter != 0,
	};

	for (int i = 0; i < 4; i++) {
		if (!live[i])
			timers[i]->next = NEVER;
		else if (timers[i]->next == NEVER)
			timers[i]->next = a->now + timers[i]->period + 1;
	}
	pulse_output(&a->pulse[0]);
	pulse_output(&a->pulse[1]);
	if (a->noise.env.length.counter == 0)
		a->noise.out = 0;
}

static void apu_write(struct apu *a, guint16 addr, guint8 value)
{
	if (addr >= 0x4000 && addr <= 0x4007) {
		struct pulse *p = &a->pulse[(addr >> 2) & 1];

		pulse_write(p, addr & 3, value);
		length_commit(&p->env.length);
	} else if (addr >= 0x4008 && addr <= 0x400b) {
		triangle_write(&a->triangle, addr & 3, value);
		length_commit(&a->triangle.length);
	} else if (addr >= 0x400c && addr <= 0x400f) {
		noise_write(&a->noise, addr & 3, value);
		length_commit(&a->noise.env.length);
	} else if (addr == 0x4015) {
		length_enable(&a->pulse[0].env.length, value & 1);
		length_enable(&a->pulse[1].env.length, value & 2);
		length_enable(&a->triangle.length, value & 4);
		length_enable(&a->noise.env.length, value & 8);
	}
	schedule(a);
}

static void quarter_frame(struct apu *a)
{
	envelope_tick(&a->pulse[0].env);
	envelope_tick(&a->pulse[1].env);
	envelope_tick(&a->noise.env);
	triangle_linear(&a->triangle);
}

static void half_frame(struct apu *a)
{
	for (int i = 0; i < 2; i++) {
		length_tick(&a->pulse[i].env.length);
		pulse_sweep(&a->pulse[i]);
	}
	length_tick(&a->triangle.length);
	length_tick(&a->noise.env.length);
}

static guint64 next_frame_step(const struct apu *a)
{
	return a->frame_base + frame_steps[a->step];
}

static void frame_counter(struct apu *a)
{
	quarter_frame(a);
	if (a->step & 1)
		half_frame(a);
	if (++a->step == 4) {
		a->step = 0;
		a->frame_base += FRAME_PERIOD;
	}
	schedule(a);
}

/* The 2A03's non-linear DAC, as Mesen's NesSoundMixer approximates it. */
static double mix(const struct apu *a)
{
	double square = a->pulse[0].out + a->pulse[1].out;
	double tnd =
		2.7516713261 * a->triangle.out + 1.8493587125 * a->noise.out;
	double out = 0;

	if (square > 0)
		out += 95.88 * 5000.0 / (8128.0 / square + 100.0);
	if (tnd > 0)
		out += 159.79 * 5000.0 / (22638.0 / tnd + 100.0);
	return out;
}

/* Runs every timer and frame-counter event up to and including 'until'. */
static void apu_run(struct apu *a, guint64 until)
{
	for (;;) {
		guint64 at = next_frame_step(a);

		at = MIN(at, a->pulse[0].timer.next);
		at = MIN(at, a->pulse[1].timer.next);
		at = MIN(at, a->triangle.timer.next);
		at = MIN(at, a->noise.timer.next);
		if (at > until)
			return;
		a->now = at;
		if (a->pulse[0].timer.next == at) {
			pulse_step(&a->pulse[0]);
			a->pulse[0].timer.next += a->pulse[0].timer.period + 1;
		}
		if (a->pulse[1].timer.next == at) {
			pulse_step(&a->pulse[1]);
			a->pulse[1].timer.next += a->pulse[1].timer.period + 1;
		}
		if (a->triangle.timer.next == at) {
			triangle_step(&a->triangle);
			a->triangle.timer.next += a->triangle.timer.period + 1;
		}
		if (a->noise.timer.next == at) {
			noise_step(&a->noise);
			a->noise.timer.next += a->noise.timer.period + 1;
		}
		if (next_frame_step(a) == at)
			frame_counter(a);
	}
}

/*
 * The tunes. A note starts on a 60 Hz frame and, unless len is 0, is cut
 * len frames later; with len 0 a decaying envelope ends it.
 *   ch:    0 and 1 pulse, 2 triangle, 3 noise
 *   pitch: MIDI note; for noise the period index, | 0x80 for short mode
 *   ctrl:  $4000/$400C value: duty << 6, loop/halt, constant, volume/decay
 *   sweep: $4001 value; 0 means off
 */
struct note {
	guint8 ch, at, len, pitch, ctrl, sweep;
};

struct tune {
	const struct note *notes;
	guint n, frames;
	gboolean loop;
};

#define D12 0x00
#define D25 0x40
#define D50 0x80
#define CONST 0x10 /* constant volume */
#define DECAY 0x00 /* envelope decay; low nibble is its rate */

/* Incoming call: two bright NES arpeggio rings, then a breath. */
static const struct note ringtone[] = {
	/* First ring: E major arpeggio, 2 frames a note, with a delayed echo. */
	{ 0, 0, 2, 88, D25 | CONST | 10, 0 },
	{ 0, 2, 2, 92, D25 | CONST | 10, 0 },
	{ 0, 4, 2, 95, D25 | CONST | 10, 0 },
	{ 0, 6, 2, 100, D25 | CONST | 10, 0 },
	{ 0, 8, 2, 88, D25 | CONST | 10, 0 },
	{ 0, 10, 2, 92, D25 | CONST | 10, 0 },
	{ 0, 12, 2, 95, D25 | CONST | 10, 0 },
	{ 0, 14, 2, 100, D25 | CONST | 10, 0 },
	{ 0, 16, 2, 88, D25 | CONST | 9, 0 },
	{ 0, 18, 2, 92, D25 | CONST | 8, 0 },
	{ 0, 20, 2, 95, D25 | CONST | 7, 0 },
	{ 0, 22, 2, 100, D25 | CONST | 6, 0 },
	{ 1, 3, 2, 76, D50 | CONST | 4, 0 },
	{ 1, 5, 2, 80, D50 | CONST | 4, 0 },
	{ 1, 7, 2, 83, D50 | CONST | 4, 0 },
	{ 1, 9, 2, 88, D50 | CONST | 4, 0 },
	{ 1, 11, 2, 76, D50 | CONST | 3, 0 },
	{ 1, 13, 2, 80, D50 | CONST | 3, 0 },
	{ 1, 15, 2, 83, D50 | CONST | 3, 0 },
	{ 1, 17, 2, 88, D50 | CONST | 3, 0 },
	{ 2, 0, 10, 52, 0, 0 },
	{ 3, 0, 0, 3, DECAY | 1, 0 },
	/* Second ring, a fourth higher. */
	{ 0, 30, 2, 93, D25 | CONST | 10, 0 },
	{ 0, 32, 2, 97, D25 | CONST | 10, 0 },
	{ 0, 34, 2, 100, D25 | CONST | 10, 0 },
	{ 0, 36, 2, 105, D25 | CONST | 10, 0 },
	{ 0, 38, 2, 93, D25 | CONST | 10, 0 },
	{ 0, 40, 2, 97, D25 | CONST | 10, 0 },
	{ 0, 42, 2, 100, D25 | CONST | 10, 0 },
	{ 0, 44, 2, 105, D25 | CONST | 10, 0 },
	{ 0, 46, 2, 93, D25 | CONST | 9, 0 },
	{ 0, 48, 2, 97, D25 | CONST | 8, 0 },
	{ 0, 50, 2, 100, D25 | CONST | 7, 0 },
	{ 0, 52, 2, 105, D25 | CONST | 6, 0 },
	{ 1, 33, 2, 81, D50 | CONST | 4, 0 },
	{ 1, 35, 2, 85, D50 | CONST | 4, 0 },
	{ 1, 37, 2, 88, D50 | CONST | 4, 0 },
	{ 1, 39, 2, 93, D50 | CONST | 4, 0 },
	{ 1, 41, 2, 81, D50 | CONST | 3, 0 },
	{ 1, 43, 2, 85, D50 | CONST | 3, 0 },
	{ 1, 45, 2, 88, D50 | CONST | 3, 0 },
	{ 1, 47, 2, 93, D50 | CONST | 3, 0 },
	{ 2, 30, 10, 57, 0, 0 },
	{ 3, 30, 0, 3, DECAY | 1, 0 },
	/* A little tail that resolves back home. */
	{ 0, 62, 6, 88, D12 | CONST | 7, 0 },
	{ 0, 68, 6, 95, D12 | CONST | 6, 0 },
	{ 0, 74, 10, 92, D12 | CONST | 5, 0 },
	{ 2, 62, 22, 40, 0, 0 },
};

/* Outgoing call: a soft two-note trill while the other phone rings. */
static const struct note ringback[] = {
	{ 0, 0, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 3, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 6, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 9, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 12, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 15, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 18, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 21, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 24, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 27, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 30, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 33, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 36, 3, 69, D50 | CONST | 4, 0 },
	{ 0, 39, 3, 71, D50 | CONST | 4, 0 },
	{ 0, 42, 3, 69, D50 | CONST | 3, 0 },
	{ 0, 45, 3, 71, D50 | CONST | 3, 0 },
	{ 0, 48, 3, 69, D50 | CONST | 2, 0 },
	{ 0, 51, 3, 71, D50 | CONST | 2, 0 },
	{ 2, 0, 54, 45, 0, 0 },
};

/* Answered: a rising C major arpeggio with an echo. */
static const struct note connected[] = {
	{ 0, 0, 0, 72, D25 | DECAY | 2, 0 },
	{ 0, 3, 0, 76, D25 | DECAY | 2, 0 },
	{ 0, 6, 0, 79, D25 | DECAY | 2, 0 },
	{ 0, 9, 0, 84, D25 | DECAY | 4, 0 },
	{ 1, 5, 3, 72, D50 | CONST | 3, 0 },
	{ 1, 8, 3, 76, D50 | CONST | 3, 0 },
	{ 1, 11, 3, 79, D50 | CONST | 3, 0 },
	{ 1, 14, 8, 84, D50 | CONST | 2, 0 },
	{ 2, 9, 10, 60, 0, 0 },
};

/* Ended: the same chord falling to a low thump. */
static const struct note hangup[] = {
	{ 0, 0, 0, 79, D50 | DECAY | 2, 0 },
	{ 0, 4, 0, 76, D50 | DECAY | 2, 0 },
	{ 0, 8, 0, 72, D50 | DECAY | 3, 0 },
	{ 2, 8, 10, 48, 0, 0 },
	{ 3, 8, 0, 10, DECAY | 2, 0 },
};

/* Camera on: a shutter click and two rising blips. */
static const struct note camera_on[] = {
	{ 3, 0, 0, 0x80 | 4, DECAY | 0, 0 },
	{ 0, 2, 3, 93, D12 | CONST | 8, 0 },
	{ 0, 5, 4, 100, D12 | CONST | 8, 0 },
};

/* Camera off: the click and the blips falling. */
static const struct note camera_off[] = {
	{ 3, 0, 0, 0x80 | 4, DECAY | 0, 0 },
	{ 0, 2, 3, 100, D12 | CONST | 7, 0 },
	{ 0, 5, 4, 93, D12 | CONST | 7, 0 },
};

/* Sharing started: the sweep unit lifts a tone, then a sparkle. */
static const struct note share_start[] = {
	{ 0, 0, 16, 60, D50 | CONST | 8, 0x80 | 0x10 | 0x08 | 0x02 },
	{ 1, 16, 0, 84, D25 | DECAY | 1, 0 },
	{ 1, 18, 0, 88, D25 | DECAY | 1, 0 },
	{ 1, 20, 0, 91, D25 | DECAY | 1, 0 },
	{ 1, 22, 0, 96, D25 | DECAY | 3, 0 },
	{ 2, 16, 12, 67, 0, 0 },
};

/* Sharing stopped: the sweep unit lets a tone fall away. */
static const struct note share_stop[] = {
	{ 0, 0, 20, 84, D50 | CONST | 7, 0x80 | 0x10 | 0x02 },
	{ 2, 14, 8, 48, 0, 0 },
};

static const struct note mute[] = {
	{ 0, 0, 0, 76, D12 | DECAY | 1, 0 },
	{ 0, 3, 0, 71, D12 | DECAY | 1, 0 },
};

static const struct note unmute[] = {
	{ 0, 0, 0, 71, D12 | DECAY | 1, 0 },
	{ 0, 3, 0, 76, D12 | DECAY | 1, 0 },
};

/* New message: a two-note chime with its NES-style echo. */
static const struct note message[] = {
	{ 0, 0, 0, 83, D25 | DECAY | 3, 0 },
	{ 0, 4, 0, 88, D25 | DECAY | 4, 0 },
	{ 1, 6, 4, 83, D25 | CONST | 4, 0 },
	{ 1, 10, 8, 88, D25 | CONST | 3, 0 },
	{ 2, 4, 6, 64, 0, 0 },
};

#define TUNE(name, frames, loop) { name, G_N_ELEMENTS(name), frames, loop }

static const struct tune tunes[CHIP_SOUNDS] = {
	[CHIP_RINGTONE] = TUNE(ringtone, 150, TRUE),
	[CHIP_RINGBACK] = TUNE(ringback, 240, TRUE),
	[CHIP_CONNECTED] = TUNE(connected, 45, FALSE),
	[CHIP_HANGUP] = TUNE(hangup, 45, FALSE),
	[CHIP_CAMERA_ON] = TUNE(camera_on, 18, FALSE),
	[CHIP_CAMERA_OFF] = TUNE(camera_off, 18, FALSE),
	[CHIP_SHARE_START] = TUNE(share_start, 50, FALSE),
	[CHIP_SHARE_STOP] = TUNE(share_stop, 40, FALSE),
	[CHIP_MUTE] = TUNE(mute, 15, FALSE),
	[CHIP_UNMUTE] = TUNE(unmute, 15, FALSE),
	[CHIP_MESSAGE] = TUNE(message, 40, FALSE),
};

gboolean chip_loops(enum chip_sound sound)
{
	return sound < CHIP_SOUNDS && tunes[sound].loop;
}

/* Timer period for a MIDI note: pulse steps every 16 CPU cycles per
 * period unit, the triangle every 32.
 */
static guint16 note_period(guint8 pitch, double divider)
{
	double hz = 440.0 * pow(2.0, (pitch - 69) / 12.0);
	long period = lround(CPU_HZ / (divider * hz) - 1.0);

	return (guint16)CLAMP(period, 8, 0x7ff);
}

static guint8 enabled_bits(const struct apu *a)
{
	return (a->pulse[0].env.length.enabled ? 1 : 0) |
	       (a->pulse[1].env.length.enabled ? 2 : 0) |
	       (a->triangle.length.enabled ? 4 : 0) |
	       (a->noise.env.length.enabled ? 8 : 0);
}

static void note_on(struct apu *a, const struct note *n)
{
	guint8 bit = 1 << n->ch;

	/* Length index 1 holds a note for 254 half frames: about 2 s. */
	apu_write(a, 0x4015, enabled_bits(a) | bit);
	if (n->ch < 2) {
		guint16 base = 0x4000 + 4 * n->ch;
		guint16 period = note_period(n->pitch, 16.0);

		apu_write(a, base, n->ctrl);
		/* Negate with shift 0 keeps low notes from being muted. */
		apu_write(a, base + 1, n->sweep ? n->sweep : 0x08);
		apu_write(a, base + 2, period & 0xff);
		apu_write(a, base + 3, (1 << 3) | (period >> 8));
	} else if (n->ch == 2) {
		guint16 period = note_period(n->pitch, 32.0);

		apu_write(a, 0x4008, 0xff);
		apu_write(a, 0x400a, period & 0xff);
		apu_write(a, 0x400b, (1 << 3) | (period >> 8));
	} else {
		apu_write(a, 0x400c, n->ctrl);
		apu_write(a, 0x400e, n->pitch & 0x8f);
		apu_write(a, 0x400f, 1 << 3);
	}
}

static void note_off(struct apu *a, const struct note *n)
{
	if (n->ch < 2)
		apu_write(a, 0x4000 + 4 * n->ch, (n->ctrl & 0xc0) | CONST);
	else if (n->ch == 2)
		apu_write(a, 0x4015, enabled_bits(a) & ~4);
	else
		apu_write(a, 0x400c, CONST);
}

/* Plays the notes that start or end on sequencer frame f. */
static void sequence(struct apu *a, const struct tune *t, guint f)
{
	for (guint i = 0; i < t->n; i++) {
		const struct note *n = &t->notes[i];

		if (n->len && n->at + n->len == f)
			note_off(a, n);
	}
	for (guint i = 0; i < t->n; i++)
		if (t->notes[i].at == f)
			note_on(a, &t->notes[i]);
}

static guint64 next_event(const struct apu *a)
{
	guint64 at = next_frame_step(a);

	at = MIN(at, a->pulse[0].timer.next);
	at = MIN(at, a->pulse[1].timer.next);
	at = MIN(at, a->triangle.timer.next);
	return MIN(at, a->noise.timer.next);
}

/*
 * NES output stage: a first-order high-pass at 90 Hz removes the DAC's DC
 * offset and a first-order low-pass at 14 kHz softens the edges, as on
 * the console. A loop is rendered twice and the second period kept, so
 * the tail of each period carries into the next and the loop is seamless.
 */
gint16 *chip_render(enum chip_sound sound, gsize *samples)
{
	g_return_val_if_fail(sound < CHIP_SOUNDS && samples, NULL);
	const struct tune *t = &tunes[sound];
	double per_sample = CPU_HZ / CHIP_RATE;
	gsize n = (gsize)((double)t->frames * FRAME_CYCLES / per_sample);
	gint16 *out = g_new0(gint16, n);
	struct apu a = { 0 };
	double hp = 1.0 / (1.0 + 2 * G_PI * 90.0 / CHIP_RATE);
	double lp = 2 * G_PI * 14000.0 / CHIP_RATE;
	double hp_in = 0, hp_out = 0, lp_out = 0, pos = 0;

	lp /= 1.0 + lp;
	a.pulse[0].first = TRUE;
	a.noise.shift = 1;
	a.pulse[0].timer.next = a.pulse[1].timer.next = NEVER;
	a.triangle.timer.next = a.noise.timer.next = NEVER;
	for (guint pass = 0; pass < (t->loop ? 2u : 1u); pass++) {
		double start = pos;
		guint frame = 0;

		for (gsize i = 0; i < n; i++) {
			double end = start + (double)(i + 1) * per_sample;
			double area = 0;

			while (pos < end) {
				double seq =
					frame < t->frames ?
						start + (double)frame *
								FRAME_CYCLES :
						G_MAXDOUBLE;
				guint64 ev = next_event(&a);
				double stop = MIN(end, MIN(seq, (double)ev));

				area += mix(&a) * (stop - pos);
				pos = stop;
				if (pos >= seq) {
					guint64 at = (guint64)seq;

					apu_run(&a, at);
					a.now = at;
					sequence(&a, t, frame++);
				} else if (pos >= (double)ev) {
					apu_run(&a, ev);
				}
			}
			double x = area / per_sample * 8.0;

			hp_out = hp * (hp_out + x - hp_in);
			hp_in = x;
			lp_out += lp * (hp_out - lp_out);
			out[i] = (gint16)CLAMP(lrint(lp_out), -32767, 32767);
		}
	}
	*samples = n;
	return out;
}
