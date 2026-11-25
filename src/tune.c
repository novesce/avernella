// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux and glibc tuning. Each hint is advisory: a kernel or libc that
 * refuses it leaves the default behaviour.
 */
#include "line.h"
#include <errno.h>
#include <malloc.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* struct sched_attr, version 1; older libcs do not declare it. */
struct line_sched_attr {
	guint32 size, policy;
	guint64 flags;
	gint32 nice;
	guint32 priority;
	guint64 runtime, deadline, period;
	guint32 util_min, util_max;
};

#define LINE_SCHED_FLAG_UTIL_CLAMP_MIN 0x20
/* Fair-class slice for call media, and a floor of a quarter of a CPU. */
#define LINE_MEDIA_SLICE_NS 1000000
#define LINE_MEDIA_UTIL_MIN 256

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

/*
 * Ask EEVDF (Linux 6.12 and later) for a short slice, so a woken call thread
 * preempts long-running work at once, and a utilization floor (uclamp), so
 * schedutil and energy-aware placement do not park it on a slow core. Both
 * stay in the normal class and need no privilege. A kernel without uclamp
 * refuses the floor; one before 6.12 ignores the slice.
 */
static void media_schedule(void)
{
	struct line_sched_attr attr = {
		.size = sizeof(attr),
		.policy = SCHED_OTHER,
		.flags = LINE_SCHED_FLAG_UTIL_CLAMP_MIN,
		.runtime = LINE_MEDIA_SLICE_NS,
		.util_min = LINE_MEDIA_UTIL_MIN,
		.util_max = 1024,
	};

	errno = 0;
	attr.nice = getpriority(PRIO_PROCESS, (id_t)gettid());
	if (errno)
		return;
	if (!syscall(SYS_sched_setattr, 0, &attr, 0))
		return;
	attr.flags = 0;
	syscall(SYS_sched_setattr, 0, &attr, 0);
}

void line_tune_thread(enum line_thread_role role)
{
	switch (role) {
	case LINE_THREAD_MEDIA:
		/* Audio and video deadlines: wake exactly on time. */
		prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
		media_schedule();
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
