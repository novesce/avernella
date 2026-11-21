// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux and glibc tuning. Each hint is advisory: a kernel or libc that
 * refuses it leaves the default behaviour.
 */
#include "line.h"
#include <malloc.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

void line_tune_process(void)
{
	/*
	 * Two malloc arenas are plenty for a handful of threads and keep the
	 * resident heap small. Buffers of 128 KiB and more (pictures, video
	 * frames) come from mmap, so freeing one returns it to the kernel.
	 */
	mallopt(M_ARENA_MAX, 2);
	mallopt(M_MMAP_THRESHOLD, 128 * 1024);
	mallopt(M_TRIM_THRESHOLD, 512 * 1024);
}

void line_tune_thread(enum line_thread_role role)
{
	switch (role) {
	case LINE_THREAD_MEDIA:
		/* Audio and video deadlines: wake exactly on time. */
		prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
		break;
	case LINE_THREAD_BACKGROUND: {
		/* Decoding and fetching may wait; let the kernel batch it. */
		struct sched_param param = { 0 };

		prctl(PR_SET_TIMERSLACK, 20UL * 1000 * 1000, 0, 0, 0);
		sched_setscheduler(0, SCHED_BATCH, &param);
		setpriority(PRIO_PROCESS, (id_t)gettid(), 5);
		break;
	}
	}
}

void line_trim_memory(void)
{
	malloc_trim(64 * 1024);
}
