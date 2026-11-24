/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The harnesses use libFuzzer's entry points, so they also build with other
 * engines. fuzz/driver.c is the engine for GCC, which has no libFuzzer.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

/* ---- Coverage feedback (fuzz/coverage.c), read by the driver ---- */

enum {
	FUZZ_EDGE_BITS = 16,
	FUZZ_EDGES = 1 << FUZZ_EDGE_BITS,
	FUZZ_CMPS = 1024,
	FUZZ_TOKENS = 256,
	FUZZ_TOKEN_MAX = 32,
};

/* An integer comparison the target made, operand 'a' against 'b'. */
struct fuzz_cmp {
	guint64 a, b;
	guint size;
};

/* A memcmp() or string comparison inside the program, not its libraries. */
struct fuzz_token {
	guint8 a[FUZZ_TOKEN_MAX], b[FUZZ_TOKEN_MAX];
	guint a_size, b_size;
};

extern guint8 fuzz_edges[FUZZ_EDGES];
extern struct fuzz_cmp fuzz_cmps[FUZZ_CMPS];
extern struct fuzz_token fuzz_tokens[FUZZ_TOKENS];
extern gboolean fuzz_running;

void fuzz_coverage_reset(void);

/* ---- Input reader shared by the harnesses ---- */

struct fuzz_input {
	const guint8 *p;
	gsize n;
};

static inline guint fuzz_byte(struct fuzz_input *in)
{
	if (!in->n)
		return 0;
	in->n--;
	return *in->p++;
}

static inline guint fuzz_u16(struct fuzz_input *in)
{
	guint high = fuzz_byte(in);

	return high << 8 | fuzz_byte(in);
}

static inline guint32 fuzz_u32(struct fuzz_input *in)
{
	guint32 high = fuzz_u16(in);

	return high << 16 | fuzz_u16(in);
}

/* A record: a two-byte big-endian length, then up to that many bytes. */
static inline gsize fuzz_chunk(struct fuzz_input *in, const guint8 **out)
{
	gsize n = fuzz_u16(in);

	n = MIN(n, in->n);
	*out = in->p;
	in->p += n;
	in->n -= n;
	return n;
}

/* Reads every byte so AddressSanitizer checks a borrowed buffer. */
static inline guint fuzz_touch(const guint8 *p, gsize n)
{
	guint sum = 0;

	for (gsize i = 0; i < n; i++)
		sum += p[i];
	return sum;
}

/* Library output stays quiet; sanitizer reports go straight to stderr. */
static inline void fuzz_quiet_print(const gchar *text)
{
	(void)text;
}

static inline void fuzz_quiet(void)
{
	g_set_printerr_handler(fuzz_quiet_print);
	g_set_print_handler(fuzz_quiet_print);
}
