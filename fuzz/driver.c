// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A coverage-guided fuzzer for GCC builds, which have no libFuzzer. It runs
 * libFuzzer's LLVMFuzzerTestOneInput() and accepts part of its command line:
 *
 *   fuzz-NAME [-flag=value...] CORPUS [SEEDS...]  mutate inputs, keeping
 *                                                 new coverage in CORPUS
 *   fuzz-NAME [-flag=value...] FILE...            run each file once
 *
 * Flags: -max_total_time=S -runs=N -max_len=N -timeout=S -rss_limit_mb=N
 * -seed=N -detect_leaks=0|1 -artifact_prefix=PATH. A crash, leak, timeout
 * or out-of-memory input is saved as PATH{crash,leak,timeout,oom}-HASH.
 *
 * Coverage comes from fuzz/coverage.c. Mutations follow libFuzzer's: bit
 * and byte edits, inserted and erased ranges, interesting integers, both
 * varint encodings the protocols use, operands of failed comparisons, and
 * splices with other corpus inputs.
 */
#include "fuzz.h"
#include <fcntl.h>
#include <pthread.h>
#include <sanitizer/common_interface_defs.h>
#include <sanitizer/lsan_interface.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

enum {
	EXIT_TIMEOUT = 70,
	EXIT_OOM = 71,
	EXIT_LEAK = 72,
	MUTATE_DEPTH = 8,
	LEAK_ATTEMPTS = 1000,
};

static struct {
	gint64 max_total_time, runs;
	guint64 seed;
	gsize max_len;
	guint timeout, rss_limit_mb;
	gboolean detect_leaks;
	const char *artifact_prefix;
} opt = {
	.runs = -1,
	.max_len = 4096,
	.timeout = 25,
	.rss_limit_mb = 2048,
	.detect_leaks = TRUE,
	.artifact_prefix = "",
};

/* The input being run, for crash reports from signal and death handlers. */
static const guint8 *current;
static gsize current_size;
static gint64 started;
static int alarm_kind;
static pthread_t main_thread;
static gboolean kept_memory;
static guint leak_attempts;

static guint8 seen[FUZZ_EDGES];
static guint seen_edges, seen_features;
static GPtrArray *corpus; /* GBytes */
static gsize corpus_bytes;
static const char *corpus_dir;
static guint64 execs, rng;
static gint64 begin_us;

const char *__asan_default_options(void);
const char *__ubsan_default_options(void);
/* From compiler-rt's allocator_interface.h, which GCC does not install. */
size_t __sanitizer_get_current_allocated_bytes(void);

/* UBSan aborts, and ASan turns the abort into a report and a death call. */
const char *__asan_default_options(void)
{
	return "handle_abort=1:handle_sigill=1:handle_sigfpe=1:"
	       "allocator_may_return_null=0:max_allocation_size_mb=2048:"
	       "malloc_context_size=30";
}

const char *__ubsan_default_options(void)
{
	return "print_stacktrace=1:halt_on_error=1:abort_on_error=1";
}

static guint64 rnd64(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 0x2545f4914f6cdd1dull;
}

static gsize rnd(gsize n)
{
	return n ? rnd64() % n : 0;
}

/* ---- Artifacts and failure handling ---- */

static void save_artifact(const char *kind, const guint8 *data, gsize size)
{
	guint64 hash = 0xcbf29ce484222325ull;
	char path[4096];
	int fd;

	for (gsize i = 0; i < size; i++)
		hash = (hash ^ data[i]) * 0x100000001b3ull;
	snprintf(path, sizeof(path), "%s%s-%016" G_GINT64_MODIFIER "x",
		 opt.artifact_prefix, kind, hash);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		fprintf(stderr, "fuzz: could not write %s\n", path);
		return;
	}
	for (gsize done = 0; done < size;) {
		ssize_t n = write(fd, data + done, size - done);

		if (n <= 0)
			break;
		done += n;
	}
	close(fd);
	fprintf(stderr, "fuzz: test unit written to %s\n", path);
}

static void on_death(void)
{
	const guint8 *data = __atomic_load_n(&current, __ATOMIC_ACQUIRE);

	if (data)
		save_artifact("crash", data, current_size);
}

/* Runs on the main thread, interrupting the stuck or growing input. */
static void on_alarm(int sig)
{
	const guint8 *data = __atomic_load_n(&current, __ATOMIC_ACQUIRE);
	int kind = __atomic_load_n(&alarm_kind, __ATOMIC_ACQUIRE);

	(void)sig;
	if (!data)
		return;
	__sanitizer_print_stack_trace();
	save_artifact(kind == EXIT_OOM ? "oom" : "timeout", data, current_size);
	_exit(kind);
}

static gsize peak_rss_mb(void)
{
	struct rusage usage;

	getrusage(RUSAGE_SELF, &usage);
	return usage.ru_maxrss / 1024;
}

static void *watchdog(void *data)
{
	(void)data;
	for (;;) {
		sleep(1);
		gint64 began = __atomic_load_n(&started, __ATOMIC_ACQUIRE);
		gint64 elapsed = g_get_monotonic_time() - began;
		gsize rss = peak_rss_mb();

		if (began && opt.timeout &&
		    elapsed > (gint64)opt.timeout * G_USEC_PER_SEC) {
			fprintf(stderr,
				"==%d== ERROR: fuzz: timeout after %u seconds\n",
				getpid(), opt.timeout);
			__atomic_store_n(&alarm_kind, EXIT_TIMEOUT,
					 __ATOMIC_RELEASE);
		} else if (opt.rss_limit_mb && rss > opt.rss_limit_mb) {
			fprintf(stderr,
				"==%d== ERROR: fuzz: out of memory (%zu MB, limit %u MB)\n",
				getpid(), rss, opt.rss_limit_mb);
			__atomic_store_n(&alarm_kind, EXIT_OOM,
					 __ATOMIC_RELEASE);
		} else {
			continue;
		}
		pthread_kill(main_thread, SIGUSR1);
		sleep(5);
		/* The input may have finished just before the signal. */
		if (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == began &&
		    began)
			_exit(__atomic_load_n(&alarm_kind, __ATOMIC_ACQUIRE));
	}
	return NULL;
}

/* ---- Running inputs ---- */

/* An exact-size copy lets AddressSanitizer catch reads past the end. */
static void execute(const guint8 *data, gsize size)
{
	guint8 *copy = malloc(size);
	gsize allocated;

	if (size)
		memcpy(copy, data, size);
	current_size = size;
	__atomic_store_n(&current, copy, __ATOMIC_RELEASE);
	fuzz_coverage_reset();
	allocated = __sanitizer_get_current_allocated_bytes();
	fuzz_running = TRUE;
	__atomic_store_n(&started, g_get_monotonic_time(), __ATOMIC_RELEASE);
	LLVMFuzzerTestOneInput(copy, size);
	__atomic_store_n(&started, 0, __ATOMIC_RELEASE);
	fuzz_running = FALSE;
	kept_memory = __sanitizer_get_current_allocated_bytes() > allocated;
	__atomic_store_n(&current, NULL, __ATOMIC_RELEASE);
	free(copy);
	execs++;
}

static guint bucket(guint hits)
{
	if (hits < 3)
		return hits;
	if (hits < 4)
		return 4;
	if (hits < 8)
		return 8;
	if (hits < 16)
		return 16;
	if (hits < 32)
		return 32;
	return hits < 128 ? 64 : 128;
}

/* Counts edge and hit-count buckets never seen before, then clears them. */
static guint collect(void)
{
	guint found = 0;

	for (gsize w = 0; w < FUZZ_EDGES; w += 8) {
		guint64 word;

		memcpy(&word, fuzz_edges + w, sizeof(word));
		if (!word)
			continue;
		for (gsize i = w; i < w + 8; i++) {
			guint b = bucket(fuzz_edges[i]);

			if (!b || (seen[i] & b))
				continue;
			seen_edges += !seen[i];
			seen[i] |= b;
			seen_features++;
			found++;
		}
		memset(fuzz_edges + w, 0, 8);
	}
	return found;
}

/*
 * As in libFuzzer: an input that allocated more than it freed runs again
 * with LeakSanitizer off, then a leak check names the first run's leaks.
 */
static void check_leak(const guint8 *data, gsize size)
{
	if (!opt.detect_leaks || !kept_memory)
		return;
	__lsan_disable();
	execute(data, size);
	__lsan_enable();
	memset(fuzz_edges, 0, sizeof(fuzz_edges));
	if (!kept_memory)
		return;
	if (leak_attempts++ >= LEAK_ATTEMPTS) {
		opt.detect_leaks = FALSE;
		fprintf(stderr,
			"INFO: fuzz: leak checks stopped after %u "
			"inputs that kept memory\n",
			LEAK_ATTEMPTS);
		return;
	}
	if (__lsan_do_recoverable_leak_check()) {
		fprintf(stderr, "==%d== ERROR: fuzz: memory leak\n", getpid());
		save_artifact("leak", data, size);
		_exit(EXIT_LEAK);
	}
}

static void status(const char *what)
{
	gint64 seconds = (g_get_monotonic_time() - begin_us) / G_USEC_PER_SEC;

	fprintf(stderr,
		"#%" G_GUINT64_FORMAT "\t%-6s cov: %u ft: %u corp: %u/%zub "
		"exec/s: %" G_GUINT64_FORMAT " rss: %zuMb\n",
		execs, what, seen_edges, seen_features, corpus->len,
		corpus_bytes, seconds ? execs / seconds : execs, peak_rss_mb());
}

static void add_unit(const guint8 *data, gsize size, gboolean save)
{
	g_ptr_array_add(corpus, g_bytes_new(data, size));
	corpus_bytes += size;
	if (!save || !corpus_dir)
		return;
	g_autofree char *name =
		g_compute_checksum_for_data(G_CHECKSUM_SHA1, data, size);
	g_autofree char *path = g_build_filename(corpus_dir, name, NULL);

	if (!g_file_test(path, G_FILE_TEST_EXISTS) &&
	    !g_file_set_contents(path, (const char *)data, size, NULL))
		fprintf(stderr, "fuzz: could not write %s\n", path);
}

/* ---- Mutations ---- */

enum {
	FORM_LITTLE,
	FORM_BIG,
	FORM_LEB128,
	FORM_QUIC,
	FORMS
};

static const guint64 interesting[] = {
	0,	     1,		  2,	      3,	  4,
	7,	     8,		  15,	      16,	  31,
	32,	     63,	  64,	      100,	  127,
	128,	     255,	  256,	      512,	  1000,
	1024,	     4095,	  4096,	      16383,	  16384,
	32767,	     32768,	  65535,      65536,	  0x7fffffff,
	0x80000000u, 0xffffffffu, 1ull << 32, 1ull << 62, G_MAXINT64,
	G_MAXUINT64,
};

static guint64 interesting_value(void)
{
	guint64 v = interesting[rnd(G_N_ELEMENTS(interesting))];

	switch (rnd(4)) {
	case 0:
		return v - 1;
	case 1:
		return v + 1;
	case 2:
		return -v;
	}
	return v;
}

/* Encodes v as 'size' fixed bytes, LEB128 or a QUIC-style varint. */
static gsize encode(guint8 *out, guint64 v, guint form, guint size)
{
	gsize n = 0;

	if (form == FORM_QUIC && v < 1ull << 62) {
		guint bits = v < 64 ? 0 : v < 16384 ? 1 : v < 1u << 30 ? 2 : 3;

		n = (gsize)1 << bits;
		for (gsize i = 0; i < n; i++)
			out[i] = v >> (8 * (n - 1 - i));
		out[0] |= bits << 6;
		return n;
	}
	if (form >= FORM_LEB128) {
		do {
			out[n++] = (v & 127) | (v > 127 ? 128 : 0);
			v >>= 7;
		} while (v);
		return n;
	}
	for (gsize i = 0; i < size; i++)
		out[form == FORM_BIG ? size - 1 - i : i] = v >> (8 * i);
	return size;
}

/* The length of the varint at p, or 0 if it runs past the end. */
static gsize varint_size(const guint8 *p, gsize n, guint form)
{
	if (form == FORM_QUIC) {
		gsize len = (gsize)1 << (p[0] >> 6);

		return len <= n ? len : 0;
	}
	for (gsize i = 0; i < n && i < 10; i++)
		if (!(p[i] & 128))
			return i + 1;
	return 0;
}

/* Replaces 'old' bytes at pos with 'len' bytes from src outside d. */
static gsize splice_in(guint8 *d, gsize n, gsize max, gsize pos, gsize old,
		       const guint8 *src, gsize len)
{
	len = MIN(len, max - (n - old));
	memmove(d + pos + len, d + pos + old, n - pos - old);
	if (len)
		memcpy(d + pos, src, len);
	return n - old + len;
}

static gsize put_value(guint8 *d, gsize n, gsize max, const guint8 *src,
		       gsize len)
{
	if (n >= len && rnd(2))
		return splice_in(d, n, max, rnd(n - len + 1), len, src, len);
	return splice_in(d, n, max, rnd(n + 1), 0, src, len);
}

/* Finds 'from' in d and replaces it with 'to', or else places 'to'. */
static gsize replace_value(guint8 *d, gsize n, gsize max, const guint8 *from,
			   gsize from_len, const guint8 *to, gsize to_len)
{
	if (n >= from_len) {
		gsize start = rnd(n);
		const guint8 *hit =
			memmem(d + start, n - start, from, from_len);

		if (!hit)
			hit = memmem(d, n, from, from_len);
		if (hit)
			return splice_in(d, n, max, hit - d, from_len, to,
					 to_len);
	}
	return put_value(d, n, max, to, to_len);
}

static gsize mutate_cmp(guint8 *d, gsize n, gsize max)
{
	const struct fuzz_cmp *c = &fuzz_cmps[rnd(FUZZ_CMPS)];
	guint8 from[10], to[10];
	guint64 a = c->a, b = c->b;
	guint form = rnd(FORMS);

	if (!c->size)
		return G_MAXSIZE;
	if (rnd(2)) {
		a = c->b;
		b = c->a;
	}
	if (!rnd(4))
		b += rnd(2) ? 1 : -1;
	return replace_value(d, n, max, from, encode(from, a, form, c->size),
			     to, encode(to, b, form, c->size));
}

static gsize mutate_token(guint8 *d, gsize n, gsize max)
{
	const struct fuzz_token *t = &fuzz_tokens[rnd(FUZZ_TOKENS)];

	if (!t->a_size)
		return G_MAXSIZE;
	if (rnd(2))
		return replace_value(d, n, max, t->b, t->b_size, t->a,
				     t->a_size);
	return replace_value(d, n, max, t->a, t->a_size, t->b, t->b_size);
}

static gsize mutate_varint(guint8 *d, gsize n, gsize max)
{
	guint form = rnd(2) ? FORM_LEB128 : FORM_QUIC;
	guint64 v = rnd(2) ? interesting_value() : rnd64() >> rnd(64);
	guint8 out[10];
	gsize len = encode(out, v, form, 8), pos = rnd(n + 1);
	gsize old = pos < n && rnd(2) ? varint_size(d + pos, n - pos, form) : 0;

	return splice_in(d, n, max, pos, old, out, len);
}

static gsize mutate_splice(guint8 *d, gsize n, gsize max)
{
	GBytes *other = corpus->pdata[rnd(corpus->len)];
	gsize size;
	const guint8 *p = g_bytes_get_data(other, &size);

	if (!size)
		return G_MAXSIZE;
	if (rnd(2)) {
		/* Crossover: our prefix, then the other input's tail. */
		gsize cut = rnd(n + 1), from = rnd(size);

		return splice_in(d, n, max, cut, n - cut, p + from,
				 size - from);
	}
	gsize len = 1 + rnd(MIN(size, 128));

	return splice_in(d, n, max, rnd(n + 1), 0, p + rnd(size - len + 1),
			 len);
}

static gsize mutate_once(guint8 *d, gsize n, gsize max, guint8 *scratch)
{
	gsize pos, len;

	switch (rnd(14)) {
	case 0:
		if (!n)
			return G_MAXSIZE;
		d[rnd(n)] ^= 1u << rnd(8);
		return n;
	case 1:
		if (!n)
			return G_MAXSIZE;
		d[rnd(n)] ^= 1 + rnd(255);
		return n;
	case 2:
		len = rnd(4) ? 1 + rnd(4) : 1 + rnd(32);
		if (rnd(2)) {
			memset(scratch, rnd(256), len);
		} else {
			for (gsize i = 0; i < len; i++)
				scratch[i] = rnd(256);
		}
		return splice_in(d, n, max, rnd(n + 1), 0, scratch, len);
	case 3:
		if (!n)
			return G_MAXSIZE;
		len = rnd(2) ? 4 : 64;
		len = 1 + rnd(MIN(n, len));
		return splice_in(d, n, max, rnd(n - len + 1), len, NULL, 0);
	case 4:
		if (n < 2)
			return G_MAXSIZE;
		len = 1 + rnd(MIN(n - 1, 64u));
		memmove(d + rnd(n - len + 1), d + rnd(n - len + 1), len);
		return n;
	case 5:
		if (!n)
			return G_MAXSIZE;
		len = 1 + rnd(MIN(n, 64u));
		memcpy(scratch, d + rnd(n - len + 1), len);
		return splice_in(d, n, max, rnd(n + 1), 0, scratch, len);
	case 6: {
		guint size = 1u << rnd(4);

		return put_value(d, n, max, scratch,
				 encode(scratch, interesting_value(), rnd(2),
					size));
	}
	case 7: {
		guint size = 1u << rnd(4), form = rnd(2);
		guint64 v = 0, delta = 1 + rnd(35);

		if (n < size)
			return G_MAXSIZE;
		pos = rnd(n - size + 1);
		for (guint i = 0; i < size; i++)
			v |= (guint64)d[pos + (form ? size - 1 - i : i)]
			     << (8 * i);
		encode(d + pos, rnd(2) ? v + delta : v - delta, form, size);
		return n;
	}
	case 8:
		return mutate_varint(d, n, max);
	case 9:
	case 10:
		return mutate_cmp(d, n, max);
	case 11:
		return mutate_token(d, n, max);
	case 12:
		return mutate_splice(d, n, max);
	default:
		if (n < 2)
			return G_MAXSIZE;
		len = 2 + rnd(MIN(n, 8u) - 1);
		pos = rnd(n - len + 1);
		for (gsize i = len - 1; i > 0; i--) {
			gsize j = rnd(i + 1);
			guint8 t = d[pos + i];

			d[pos + i] = d[pos + j];
			d[pos + j] = t;
		}
		return n;
	}
}

static gsize mutate(guint8 *d, gsize n, gsize max, guint8 *scratch)
{
	for (guint tries = 0; tries < 32; tries++) {
		gsize m = mutate_once(d, n, max, scratch);

		if (m != G_MAXSIZE)
			return m;
	}
	return n;
}

/* ---- Corpus and main loop ---- */

static gint by_size(gconstpointer a, gconstpointer b)
{
	gsize x = g_bytes_get_size(*(GBytes *const *)a);
	gsize y = g_bytes_get_size(*(GBytes *const *)b);

	return x < y ? -1 : x > y;
}

static void read_dir(const char *dir, GPtrArray *inputs)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	const char *name;

	while (d && (name = g_dir_read_name(d))) {
		g_autofree char *path = g_build_filename(dir, name, NULL);
		g_autofree char *data = NULL;
		gsize size;

		if (*name == '.' ||
		    !g_file_test(path, G_FILE_TEST_IS_REGULAR) ||
		    !g_file_get_contents(path, &data, &size, NULL))
			continue;
		g_ptr_array_add(inputs,
				g_bytes_new(data, MIN(size, opt.max_len)));
	}
}

static gboolean finished(void)
{
	gint64 elapsed = g_get_monotonic_time() - begin_us;

	return (opt.runs >= 0 && execs >= (guint64)opt.runs) ||
	       (opt.max_total_time &&
		elapsed >= opt.max_total_time * G_USEC_PER_SEC);
}

static int fuzz(GPtrArray *dirs)
{
	g_autoptr(GPtrArray) inputs =
		g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	g_autofree guint8 *work = g_malloc(opt.max_len);
	g_autofree guint8 *scratch = g_malloc(opt.max_len);
	gint64 last_status = 0;
	guint64 pulse = 1;

	if (dirs->len) {
		corpus_dir = dirs->pdata[0];
		g_mkdir_with_parents(corpus_dir, 0700);
	}
	for (guint i = 0; i < dirs->len; i++)
		read_dir(dirs->pdata[i], inputs);
	g_ptr_array_sort(inputs, by_size);
	fprintf(stderr, "INFO: fuzz: %u inputs in %u directories\n",
		inputs->len, dirs->len);
	if (!inputs->len)
		g_ptr_array_add(inputs, g_bytes_new(NULL, 0));
	for (guint i = 0; i < inputs->len; i++) {
		gsize size;
		const guint8 *data = g_bytes_get_data(inputs->pdata[i], &size);

		execute(data, size);
		if (collect() || !corpus->len)
			add_unit(data, size, TRUE);
		check_leak(data, size);
	}
	status("INITED");
	while (!finished()) {
		GBytes *parent =
			corpus->pdata[rnd(2) ? rnd(corpus->len) :
					       corpus->len - 1 -
						       rnd(MIN(corpus->len,
							       32u))];
		gsize n = MIN(g_bytes_get_size(parent), opt.max_len);
		guint depth = 1 + rnd(MUTATE_DEPTH);

		if (n)
			memcpy(work, g_bytes_get_data(parent, NULL), n);
		for (guint i = 0; i < depth; i++)
			n = mutate(work, n, opt.max_len, scratch);
		execute(work, n);
		if (collect()) {
			add_unit(work, n, TRUE);
			status("NEW");
		}
		check_leak(work, n);
		gint64 now = g_get_monotonic_time();

		if (execs >= pulse || now - last_status > 30 * G_USEC_PER_SEC) {
			if (execs >= pulse)
				pulse *= 2;
			last_status = now;
			status("pulse");
		}
	}
	status("DONE");
	fprintf(stderr,
		"Done %" G_GUINT64_FORMAT " runs in %" G_GINT64_FORMAT
		" second(s)\n",
		execs, (g_get_monotonic_time() - begin_us) / G_USEC_PER_SEC);
	return 0;
}

static int replay(GPtrArray *files)
{
	for (guint i = 0; i < files->len; i++) {
		g_autofree char *data = NULL;
		gsize size;
		gint64 t;

		if (!g_file_get_contents(files->pdata[i], &data, &size, NULL)) {
			fprintf(stderr, "fuzz: could not read %s\n",
				(const char *)files->pdata[i]);
			return 1;
		}
		fprintf(stderr, "Running: %s\n", (const char *)files->pdata[i]);
		t = g_get_monotonic_time();
		execute((const guint8 *)data, size);
		memset(fuzz_edges, 0, sizeof(fuzz_edges));
		fprintf(stderr, "Executed %s in %" G_GINT64_FORMAT " ms\n",
			(const char *)files->pdata[i],
			(g_get_monotonic_time() - t) / 1000);
	}
	return 0;
}

static gboolean parse_flag(const char *arg)
{
	const char *eq = strchr(arg, '=');

	if (!eq)
		return FALSE;
	g_autofree char *name = g_strndup(arg + 1, eq - arg - 1);
	const char *value = eq + 1;
	gint64 n = g_ascii_strtoll(value, NULL, 10);

	if (!strcmp(name, "max_total_time"))
		opt.max_total_time = MAX(n, 0);
	else if (!strcmp(name, "runs"))
		opt.runs = n;
	else if (!strcmp(name, "max_len"))
		opt.max_len = CLAMP(n, 1, 1 << 20);
	else if (!strcmp(name, "timeout"))
		opt.timeout = CLAMP(n, 0, G_MAXINT);
	else if (!strcmp(name, "rss_limit_mb"))
		opt.rss_limit_mb = CLAMP(n, 0, G_MAXINT);
	else if (!strcmp(name, "seed"))
		opt.seed = n;
	else if (!strcmp(name, "detect_leaks"))
		opt.detect_leaks = n != 0;
	else if (!strcmp(name, "artifact_prefix"))
		opt.artifact_prefix = value;
	else
		fprintf(stderr, "WARNING: fuzz: ignoring flag -%s\n", name);
	return TRUE;
}

int main(int argc, char **argv)
{
	g_autoptr(GPtrArray) paths = g_ptr_array_new();
	struct sigaction alarm = { .sa_handler = on_alarm };
	pthread_t thread;

	for (int i = 1; i < argc; i++)
		if (argv[i][0] != '-' || !parse_flag(argv[i]))
			g_ptr_array_add(paths, argv[i]);
	LLVMFuzzerInitialize(&argc, &argv);
	rng = opt.seed ? opt.seed :
			 (guint64)g_get_real_time() ^ (guint64)getpid() << 32;
	fprintf(stderr, "INFO: Seed: %" G_GUINT64_FORMAT "\n", rng);
	rng |= 1;
	corpus = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	begin_us = g_get_monotonic_time();
	main_thread = pthread_self();
	sigaction(SIGUSR1, &alarm, NULL);
	__sanitizer_set_death_callback(on_death);
	/* A critical warning from GLib in the target is a bug as well. */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
	if (pthread_create(&thread, NULL, watchdog, NULL)) {
		fprintf(stderr, "fuzz: could not start the watchdog\n");
		return 1;
	}
	pthread_detach(thread);
	if (paths->len && g_file_test(paths->pdata[0], G_FILE_TEST_IS_REGULAR))
		return replay(paths);
	return fuzz(paths);
}
