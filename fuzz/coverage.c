// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Coverage feedback for the GCC fuzzing driver. The parsers and harnesses
 * are built with -fsanitize-coverage=trace-pc,trace-cmp, and GCC calls the
 * functions below. This file itself is built without sanitizers or coverage
 * because it runs on every basic block.
 *
 * Each block updates an AFL-style counter for the edge from the previous
 * block. Each comparison records its two operands so the mutator can find
 * one in the input and replace it with the other. String comparisons from
 * AddressSanitizer's interceptors do the same for byte strings.
 */
#include "fuzz.h"
#include <string.h>

guint8 fuzz_edges[FUZZ_EDGES];
struct fuzz_cmp fuzz_cmps[FUZZ_CMPS];
struct fuzz_token fuzz_tokens[FUZZ_TOKENS];
gboolean fuzz_running;

static guint64 previous;
static guint switch_case, token_next;

/* GNU ld defines the bounds of the program's own code. */
extern const char __executable_start[], etext[];

void __sanitizer_cov_trace_pc(void);
void __sanitizer_cov_trace_cmp1(guint8 a, guint8 b);
void __sanitizer_cov_trace_cmp2(guint16 a, guint16 b);
void __sanitizer_cov_trace_cmp4(guint32 a, guint32 b);
void __sanitizer_cov_trace_cmp8(guint64 a, guint64 b);
void __sanitizer_cov_trace_const_cmp1(guint8 a, guint8 b);
void __sanitizer_cov_trace_const_cmp2(guint16 a, guint16 b);
void __sanitizer_cov_trace_const_cmp4(guint32 a, guint32 b);
void __sanitizer_cov_trace_const_cmp8(guint64 a, guint64 b);
void __sanitizer_cov_trace_cmpf(float a, float b);
void __sanitizer_cov_trace_cmpd(double a, double b);
void __sanitizer_cov_trace_switch(guint64 value, guint64 *cases);
void __sanitizer_weak_hook_memcmp(void *pc, const void *a, const void *b,
				  size_t n, int result);
void __sanitizer_weak_hook_strcmp(void *pc, const char *a, const char *b,
				  int result);
void __sanitizer_weak_hook_strncmp(void *pc, const char *a, const char *b,
				   size_t n, int result);
void __sanitizer_weak_hook_strstr(void *pc, const char *haystack,
				  const char *needle, char *result);

static guint64 mix(guint64 value)
{
	return value * 0x9e3779b97f4a7c15ull;
}

void fuzz_coverage_reset(void)
{
	previous = 0;
}

void __sanitizer_cov_trace_pc(void)
{
	guint64 here = mix((guintptr)__builtin_return_address(0)) >>
		       (64 - FUZZ_EDGE_BITS);

	fuzz_edges[(here ^ previous) & (FUZZ_EDGES - 1)]++;
	previous = here >> 1;
}

/* One slot per comparison site, with a few for the values it has seen. */
static void compared(guintptr pc, guint64 a, guint64 b, guint size)
{
	if (a == b || !fuzz_running)
		return;
	struct fuzz_cmp *c = &fuzz_cmps[(mix(pc) >> 40 ^ (a & 3)) % FUZZ_CMPS];

	c->a = a;
	c->b = b;
	c->size = size;
}

#define CMP(name, type, size)                                                \
	void __sanitizer_cov_trace_##name(type a, type b)                    \
	{                                                                    \
		compared((guintptr)__builtin_return_address(0), a, b, size); \
	}

CMP(cmp1, guint8, 1)
CMP(cmp2, guint16, 2)
CMP(cmp4, guint32, 4)
CMP(cmp8, guint64, 8)
CMP(const_cmp1, guint8, 1)
CMP(const_cmp2, guint16, 2)
CMP(const_cmp4, guint32, 4)
CMP(const_cmp8, guint64, 8)

void __sanitizer_cov_trace_cmpf(float a, float b)
{
	(void)a;
	(void)b;
}

void __sanitizer_cov_trace_cmpd(double a, double b)
{
	(void)a;
	(void)b;
}

/* cases[0] is the number of cases, cases[1] their width in bits. */
void __sanitizer_cov_trace_switch(guint64 value, guint64 *cases)
{
	if (!cases[0])
		return;
	guint64 match = cases[2 + switch_case++ % cases[0]];

	compared((guintptr)__builtin_return_address(0), value, match,
		 MAX(cases[1] / 8, 1));
}

static gboolean own_code(const void *pc)
{
	const char *p = pc;

	return fuzz_running && p >= __executable_start && p < etext;
}

static void note_tokens(const void *a, gsize a_size, const void *b,
			gsize b_size)
{
	if (!a_size || !b_size || a_size > FUZZ_TOKEN_MAX ||
	    b_size > FUZZ_TOKEN_MAX)
		return;
	struct fuzz_token *t = &fuzz_tokens[token_next++ % FUZZ_TOKENS];

	memcpy(t->a, a, a_size);
	memcpy(t->b, b, b_size);
	t->a_size = a_size;
	t->b_size = b_size;
}

void __sanitizer_weak_hook_memcmp(void *pc, const void *a, const void *b,
				  size_t n, int result)
{
	if (result && own_code(pc))
		note_tokens(a, n, b, n);
}

void __sanitizer_weak_hook_strcmp(void *pc, const char *a, const char *b,
				  int result)
{
	if (result && own_code(pc))
		note_tokens(a, strnlen(a, FUZZ_TOKEN_MAX + 1), b,
			    strnlen(b, FUZZ_TOKEN_MAX + 1));
}

void __sanitizer_weak_hook_strncmp(void *pc, const char *a, const char *b,
				   size_t n, int result)
{
	if (result && own_code(pc))
		note_tokens(a, strnlen(a, MIN(n, FUZZ_TOKEN_MAX + 1)), b,
			    strnlen(b, MIN(n, FUZZ_TOKEN_MAX + 1)));
}

void __sanitizer_weak_hook_strstr(void *pc, const char *haystack,
				  const char *needle, char *result)
{
	gsize n = strnlen(needle, FUZZ_TOKEN_MAX + 1);

	if (!result && own_code(pc))
		note_tokens(needle, n, needle, n);
	(void)haystack;
}
