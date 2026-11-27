// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux and glibc tuning. Each hint is advisory: a kernel or libc that
 * refuses it leaves the default behaviour.
 */
#include "line.h"
#include "codec_affinity.h"
#include <gio/gio.h>
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

/* RTKit requires RESET_ON_FORK and a worker-wide finite RLIMIT_RTTIME.
 * Query the service's own budget and keep any tighter inherited limit.
 */
static gboolean realtime_service(GBusType bus, const char *service,
				 const char *path, const char *interface)
{
	g_autoptr(GDBusConnection) conn = g_bus_get_sync(bus, NULL, NULL);
	if (!conn)
		return FALSE;
	g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
		conn, service, path, "org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", interface, "RTTimeUSecMax"),
		G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
	if (!reply)
		return FALSE;
	g_autoptr(GVariant) value = NULL;
	g_variant_get(reply, "(v)", &value);
	if (!g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
		return FALSE;
	gint64 limit = g_variant_get_int64(value);
	struct rlimit budget;
	struct sched_param param = { 0 };

	if (limit <= 0 || getrlimit(RLIMIT_RTTIME, &budget) ||
	    sched_setscheduler(0, SCHED_OTHER | SCHED_RESET_ON_FORK, &param))
		return FALSE;
	budget.rlim_max = MIN((rlim_t)limit, budget.rlim_max);
	budget.rlim_cur = MIN(budget.rlim_cur, budget.rlim_max);
	if (setrlimit(RLIMIT_RTTIME, &budget))
		return FALSE;
	g_clear_pointer(&reply, g_variant_unref);
	reply = g_dbus_connection_call_sync(
		conn, service, path, interface, "MakeThreadRealtimeWithPID",
		g_variant_new("(ttu)", (guint64)getpid(), (guint64)gettid(),
			      1u),
		G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
	return reply != NULL;
}

static gboolean network_realtime(void)
{
	struct sched_param param = { .sched_priority = 1 };

	if (g_strcmp0(g_getenv("LINE_CALL_REALTIME"), "1"))
		return FALSE;
	if (!sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &param))
		return TRUE;
	if (!realtime_service(G_BUS_TYPE_SESSION,
			      "org.freedesktop.portal.Desktop",
			      "/org/freedesktop/portal/desktop",
			      "org.freedesktop.portal.Realtime") &&
	    !g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS))
		realtime_service(G_BUS_TYPE_SYSTEM,
				 "org.freedesktop.RealtimeKit1",
				 "/org/freedesktop/RealtimeKit1",
				 "org.freedesktop.RealtimeKit1");
	int policy = sched_getscheduler(0);

	return policy >= 0 && ((policy & ~SCHED_RESET_ON_FORK) == SCHED_FIFO ||
			       (policy & ~SCHED_RESET_ON_FORK) == SCHED_RR);
}

/* Restrict codec children to one NUMA node only when it has enough CPUs
 * already allowed by the user's cpuset. The caller restores its mask.
 */
gboolean line_codec_local_cpus(cpu_set_t *saved, guint threads)
{
	unsigned int node;
	cpu_set_t mask;

	g_autofree char *path = NULL, *list = NULL;

	if (g_strcmp0(g_getenv("LINE_CALL_CODEC_NUMA"), "1") ||
	    getcpu(NULL, &node) || sched_getaffinity(0, sizeof(*saved), saved))
		return FALSE;
	path = g_strdup_printf("/sys/devices/system/node/node%u/cpulist", node);
	if (!g_file_get_contents(path, &list, NULL, NULL))
		return FALSE;
	CPU_ZERO(&mask);
	char *p = list;

	for (;;) {
		char *end;
		guint64 first = g_ascii_strtoull(p, &end, 10), last = first;

		if (p == end || first >= CPU_SETSIZE)
			return FALSE;
		if (*end == '-') {
			p = end + 1;
			last = g_ascii_strtoull(p, &end, 10);
			if (p == end || last < first || last >= CPU_SETSIZE)
				return FALSE;
		}
		for (guint cpu = first; cpu <= last; cpu++)
			if (CPU_ISSET(cpu, saved))
				CPU_SET(cpu, &mask);
		if (*end != ',')
			break;
		p = end + 1;
	}
	if ((guint)CPU_COUNT(&mask) < threads || CPU_EQUAL(saved, &mask))
		return FALSE;
	return !sched_setaffinity(0, sizeof(mask), &mask);
}

void line_tune_thread(enum line_thread_role role)
{
	switch (role) {
	case LINE_THREAD_NETWORK:
		prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
		if (!network_realtime())
			media_schedule();
		break;
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
