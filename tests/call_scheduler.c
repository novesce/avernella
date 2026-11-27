// SPDX-License-Identifier: GPL-3.0-or-later
/* Capture-period wakeup lateness. Each policy runs in its own child so
 * RTKit limits and the caller's affinity never escape the measurement.
 */
#include "line.h"
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>

static int compare_us(const void *a, const void *b)
{
	gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;

	return (x > y) - (x < y);
}

static void wakeups(gconstpointer value)
{
	const char *policy = value;

	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC,
				       G_TEST_SUBPROCESS_INHERIT_STDOUT);
		g_test_trap_assert_passed();
		return;
	}
	g_setenv("LINE_CALL_REALTIME", policy, TRUE);
	line_tune_thread(LINE_THREAD_NETWORK);
	enum {
		N = 201
	};
	gint64 next = g_get_monotonic_time(), late[N];

	for (guint i = 0; i < N; i++) {
		next += 10000; /* one capture quantum at 48 kHz */
		struct timespec deadline = { next / G_USEC_PER_SEC,
					     (next % G_USEC_PER_SEC) * 1000 };
		int rc;

		do {
			rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
					     &deadline, NULL);
		} while (rc == EINTR);
		g_assert_cmpint(rc, ==, 0);
		late[i] = g_get_monotonic_time() - next;
	}
	qsort(late, N, sizeof(*late), compare_us);
	g_print("RR requested=%s actual-policy=%d wake p50/p95/p99 %" G_GINT64_FORMAT
		"/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " us\n",
		policy, sched_getscheduler(0) & ~SCHED_RESET_ON_FORK, late[100],
		late[190], late[198]);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/scheduler/fair", "0", wakeups);
	g_test_add_data_func("/scheduler/realtime", "1", wakeups);
	return g_test_run();
}
