// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Batched UDP over loopback: every mode must deliver exactly the same
 * datagrams, in order, as one sendto() or recvfrom() each.
 */
#include "../src/udp_batch.h"
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int bound_socket(struct sockaddr_in *addr)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	socklen_t len = sizeof(*addr);
	int buffer = 4 << 20;

	g_assert_cmpint(fd, >=, 0);
	*addr = (struct sockaddr_in){
		.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
	};
	g_assert_cmpint(bind(fd, (void *)addr, sizeof(*addr)), ==, 0);
	g_assert_cmpint(getsockname(fd, (void *)addr, &len), ==, 0);
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
	return fd;
}

static void fill(guint8 *p, gsize n, guint tag)
{
	for (gsize i = 0; i < n; i++)
		p[i] = (guint8)(tag * 31 + i);
}

static gboolean check(const guint8 *p, gsize n, guint tag)
{
	for (gsize i = 0; i < n; i++)
		if (p[i] != (guint8)(tag * 31 + i))
			return FALSE;
	return TRUE;
}

/* A video picture: a longer first packet, equal middles, a short last. */
static const gsize picture[] = { 1180, 1160, 1160, 1160, 1160, 1160, 1160,
				 1160, 1160, 1160, 1160, 1160, 300 };

static void expect_picture(int fd, guint base)
{
	for (guint i = 0; i < G_N_ELEMENTS(picture); i++) {
		guint8 data[2048];
		ssize_t n = recv(fd, data, sizeof(data), 0);

		g_assert_cmpint(n, ==, (ssize_t)picture[i]);
		g_assert_true(check(data, n, base + i));
	}
	guint8 extra;

	g_assert_cmpint(recv(fd, &extra, 1, MSG_DONTWAIT), <, 0);
}

static void send_picture(struct line_udp_tx *t, const struct sockaddr_in *to,
			 guint base)
{
	for (guint i = 0; i < G_N_ELEMENTS(picture); i++) {
		guint8 data[LINE_UDP_TX_SIZE];

		fill(data, picture[i], base + i);
		g_assert_true(line_udp_tx_queue(t, to, sizeof(*to), data,
						picture[i]));
	}
	g_assert_true(line_udp_tx_pending(t));
	g_assert_true(line_udp_tx_flush(t));
	g_assert_false(line_udp_tx_pending(t));
}

static void send_modes(gconstpointer data)
{
	enum line_udp_mode mode = GPOINTER_TO_INT(data);
	struct sockaddr_in rx_addr, tx_addr;
	int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);
	struct line_udp_tx *t = line_udp_tx_new(tx, mode);

	send_picture(t, &rx_addr, 0);
	expect_picture(rx, 0);
	g_assert_cmpint(line_udp_tx_mode(t), ==, mode);
	struct line_udp_stats s = line_udp_tx_stats(t);

	g_assert_cmpuint(s.datagrams, ==, G_N_ELEMENTS(picture));
	/* The burst costs one system call unless batching is off. */
	g_assert_cmpuint(s.syscalls, ==,
			 mode == LINE_UDP_PLAIN ? G_N_ELEMENTS(picture) : 1);
	line_udp_tx_free(t);
	close(rx);
	close(tx);
}

/* SO_NO_CHECK makes the kernel refuse UDP_SEGMENT with EINVAL. */
static void gso_refused(void)
{
	struct sockaddr_in rx_addr, tx_addr;
	int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);
	int one = 1;

	g_assert_cmpint(setsockopt(tx, SOL_SOCKET, SO_NO_CHECK, &one,
				   sizeof(one)),
			==, 0);
	struct line_udp_tx *t = line_udp_tx_new(tx, LINE_UDP_GSO);

	send_picture(t, &rx_addr, 7);
	expect_picture(rx, 7);
	g_assert_cmpint(line_udp_tx_mode(t), ==, LINE_UDP_MMSG);
	send_picture(t, &rx_addr, 9);
	expect_picture(rx, 9);
	line_udp_tx_free(t);
	close(rx);
	close(tx);
}

static void destination_change(void)
{
	struct sockaddr_in a_addr, b_addr, tx_addr;
	int a = bound_socket(&a_addr), b = bound_socket(&b_addr),
	    tx = bound_socket(&tx_addr);
	struct line_udp_tx *t = line_udp_tx_new(tx, LINE_UDP_GSO);
	const struct sockaddr_in *to[] = { &a_addr, &a_addr, &b_addr, &a_addr,
					   &b_addr };
	guint8 data[1000];

	for (guint i = 0; i < G_N_ELEMENTS(to); i++) {
		fill(data, sizeof(data), i);
		g_assert_true(line_udp_tx_queue(t, to[i], sizeof(*to[i]), data,
						sizeof(data)));
	}
	g_assert_true(line_udp_tx_flush(t));
	static const guint at_a[] = { 0, 1, 3 }, at_b[] = { 2, 4 };

	for (guint i = 0; i < G_N_ELEMENTS(at_a); i++) {
		g_assert_cmpint(recv(a, data, sizeof(data), 0), ==, 1000);
		g_assert_true(check(data, sizeof(data), at_a[i]));
	}
	for (guint i = 0; i < G_N_ELEMENTS(at_b); i++) {
		g_assert_cmpint(recv(b, data, sizeof(data), 0), ==, 1000);
		g_assert_true(check(data, sizeof(data), at_b[i]));
	}
	/* Oversized datagrams are refused, never truncated. */
	g_assert_false(line_udp_tx_queue(t, &a_addr, sizeof(a_addr), data,
					 LINE_UDP_TX_SIZE + 1));
	line_udp_tx_free(t);
	close(a);
	close(b);
	close(tx);
}

enum {
	RX_COUNT = 40
};

static void receive_all(struct line_udp_rx *r, const struct sockaddr_in *src)
{
	for (guint i = 0; i < RX_COUNT; i++) {
		guint8 data[LINE_UDP_RX_SIZE];
		struct sockaddr_storage from;
		socklen_t len;
		ssize_t n =
			line_udp_rx_next(r, data, sizeof(data), &from, &len);

		g_assert_cmpint(n, ==, 100 + 20 * i);
		g_assert_true(check(data, n, i));
		g_assert_cmpuint(len, ==, sizeof(*src));
		g_assert_cmpmem(&from, len, src, sizeof(*src));
	}
	guint8 data[16];
	struct sockaddr_storage from;
	socklen_t len;

	g_assert_false(line_udp_rx_pending(r));
	g_assert_cmpint(line_udp_rx_next(r, data, sizeof(data), &from, &len),
			==, -1);
	g_assert_cmpint(errno, ==, EAGAIN);
}

static void queue_datagrams(int tx, const struct sockaddr_in *to)
{
	for (guint i = 0; i < RX_COUNT; i++) {
		guint8 data[1000];

		fill(data, 100 + 20 * i, i);
		g_assert_cmpint(sendto(tx, data, 100 + 20 * i, 0, (void *)to,
				       sizeof(*to)),
				==, 100 + 20 * i);
	}
}

static void receive_modes(gconstpointer data)
{
	enum line_udp_mode mode = GPOINTER_TO_INT(data);
	struct sockaddr_in rx_addr, tx_addr;
	int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);
	struct line_udp_rx *r = line_udp_rx_new(rx, mode);

	queue_datagrams(tx, &rx_addr);
	receive_all(r, &tx_addr);
	struct line_udp_stats s = line_udp_rx_stats(r);

	g_assert_cmpuint(s.datagrams, ==, RX_COUNT);
	/* 40 datagrams and the final EAGAIN: 4 batches or 41 single reads. */
	g_assert_cmpuint(s.syscalls, ==,
			 mode == LINE_UDP_PLAIN ?
				 RX_COUNT + 1 :
				 (RX_COUNT + LINE_UDP_RX_BATCH - 1) /
						 LINE_UDP_RX_BATCH +
					 1);
	/* A truncated copy still consumes the datagram. */
	queue_datagrams(tx, &rx_addr);
	guint8 small[8];
	struct sockaddr_storage from;
	socklen_t len;

	g_assert_cmpint(line_udp_rx_next(r, small, sizeof(small), &from, &len),
			==, sizeof(small));
	g_assert_true(check(small, sizeof(small), 0));
	line_udp_rx_free(r);
	close(rx);
	close(tx);
}

/*
 * A sandbox that refuses recvmmsg() and sendmmsg() with ENOSYS, as an old
 * kernel or a strict seccomp profile would. Runs in a child process.
 */
static void refuse_mmsg(void)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendmmsg, 2, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_recvmmsg, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
	};
	struct sock_fprog program = { G_N_ELEMENTS(filter), filter };

	g_assert_cmpint(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), ==, 0);
	g_assert_cmpint(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program),
			==, 0);
}

static void sandbox_fallback(void)
{
	if (g_test_subprocess()) {
		struct sockaddr_in rx_addr, tx_addr;
		int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);

		refuse_mmsg();
		struct line_udp_tx *t = line_udp_tx_new(tx, LINE_UDP_GSO);
		struct line_udp_rx *r = line_udp_rx_new(rx, LINE_UDP_MMSG);

		send_picture(t, &rx_addr, 3);
		expect_picture(rx, 3);
		g_assert_cmpint(line_udp_tx_mode(t), ==, LINE_UDP_PLAIN);
		queue_datagrams(tx, &rx_addr);
		receive_all(r, &tx_addr);
		g_assert_cmpint(line_udp_rx_mode(r), ==, LINE_UDP_PLAIN);
		line_udp_tx_free(t);
		line_udp_rx_free(r);
		return;
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
}

static void default_mode(void)
{
	g_setenv("LINE_DISABLE_UDP_BATCH", "1", TRUE);
	g_assert_cmpint(line_udp_default_mode(), ==, LINE_UDP_PLAIN);
	g_setenv("LINE_DISABLE_UDP_BATCH", "0", TRUE);
	g_assert_cmpint(line_udp_default_mode(), ==, LINE_UDP_GSO);
	g_unsetenv("LINE_DISABLE_UDP_BATCH");
	g_assert_cmpint(line_udp_default_mode(), ==, LINE_UDP_GSO);
}

/* The TOS byte a datagram arrived with, from IP_RECVTOS. */
static int received_tos(int fd)
{
	guint8 data[64];
	union {
		struct cmsghdr header;
		guint8 space[CMSG_SPACE(sizeof(int))];
	} control;
	struct iovec iov = { data, sizeof(data) };
	struct msghdr msg = { .msg_iov = &iov,
			      .msg_iovlen = 1,
			      .msg_control = control.space,
			      .msg_controllen = sizeof(control.space) };

	g_assert_cmpint(recvmsg(fd, &msg, 0), >, 0);
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
	     c = CMSG_NXTHDR(&msg, c))
		if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS)
			return *CMSG_DATA(c);
	return -1;
}

/* Voice is marked EF per datagram, everything else AF41 by default. */
static void traffic_class(void)
{
	struct sockaddr_in to, from;
	int rx = bound_socket(&to), tx = bound_socket(&from), on = 1;
	guint8 data[32] = { 1 };

	g_assert_cmpint(setsockopt(rx, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)),
			==, 0);
	g_unsetenv("LINE_CALL_DSCP");
	g_assert_cmpuint(line_udp_tune(tx, AF_INET), ==, LINE_DSCP_VIDEO);
	g_assert_cmpint(sendto(tx, data, sizeof(data), 0, (void *)&to,
			       sizeof(to)),
			==, sizeof(data));
	g_assert_cmpint(received_tos(rx), ==, LINE_DSCP_VIDEO << 2);
	g_assert_cmpint(line_udp_send_class(tx, AF_INET, &to, sizeof(to), data,
					    sizeof(data), LINE_DSCP_VOICE),
			==, sizeof(data));
	g_assert_cmpint(received_tos(rx), ==, LINE_DSCP_VOICE << 2);
	close(tx);
	/* LINE_CALL_DSCP=0 leaves call packets unmarked. */
	tx = bound_socket(&from);
	g_setenv("LINE_CALL_DSCP", "0", TRUE);
	g_assert_cmpuint(line_udp_tune(tx, AF_INET), ==, 0);
	g_assert_cmpint(line_udp_send_class(tx, AF_INET, &to, sizeof(to), data,
					    sizeof(data), LINE_DSCP_VOICE),
			==, sizeof(data));
	g_assert_cmpint(received_tos(rx), ==, 0);
	g_unsetenv("LINE_CALL_DSCP");
	close(tx);
	close(rx);
}

static gdouble cpu_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *const mode_names[] = { "sendto", "sendmmsg", "UDP_SEGMENT" };

/* With -m perf: CPU time a datagram for a 720p keyframe-sized burst. */
static void perf_send(gconstpointer data)
{
	enum line_udp_mode mode = GPOINTER_TO_INT(data);
	enum {
		BURST = 48,
		ROUNDS = 400
	};

	if (!g_test_perf()) {
		g_test_skip("Run with -m perf to measure.");
		return;
	}
	struct sockaddr_in rx_addr, tx_addr;
	int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);
	struct line_udp_tx *t = line_udp_tx_new(tx, mode);
	guint8 packet[1160], sink[2048];
	gdouble spent = 0;

	fill(packet, sizeof(packet), 1);
	for (int round = 0; round < ROUNDS; round++) {
		gdouble start = cpu_seconds();

		for (int i = 0; i < BURST; i++)
			line_udp_tx_queue(t, &rx_addr, sizeof(rx_addr), packet,
					  i == BURST - 1 ? 400 :
							   sizeof(packet));
		g_assert_true(line_udp_tx_flush(t));
		spent += cpu_seconds() - start;
		for (int i = 0; i < BURST; i++)
			g_assert_cmpint(recv(rx, sink, sizeof(sink), 0), >, 0);
	}
	gdouble us = spent * 1e6 / (BURST * ROUNDS);

	g_test_minimized_result(us, "send %s: %.2f us CPU a datagram",
				mode_names[mode], us);
	line_udp_tx_free(t);
	close(rx);
	close(tx);
}

static void perf_receive(gconstpointer data)
{
	enum line_udp_mode mode = GPOINTER_TO_INT(data);
	enum {
		BURST = 48,
		ROUNDS = 400
	};

	if (!g_test_perf()) {
		g_test_skip("Run with -m perf to measure.");
		return;
	}
	struct sockaddr_in rx_addr, tx_addr;
	int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);
	struct line_udp_rx *r = line_udp_rx_new(rx, mode);
	guint8 packet[1160], sink[LINE_UDP_RX_SIZE];
	gdouble spent = 0;

	fill(packet, sizeof(packet), 1);
	for (int round = 0; round < ROUNDS; round++) {
		for (int i = 0; i < BURST; i++)
			sendto(tx, packet, sizeof(packet), 0, (void *)&rx_addr,
			       sizeof(rx_addr));
		gdouble start = cpu_seconds();
		struct sockaddr_storage from;
		socklen_t len;

		for (int i = 0; i < BURST; i++)
			g_assert_cmpint(line_udp_rx_next(r, sink, sizeof(sink),
							 &from, &len),
					==, sizeof(packet));
		spent += cpu_seconds() - start;
	}
	gdouble us = spent * 1e6 / (BURST * ROUNDS);

	g_test_minimized_result(
		us, "receive %s: %.2f us CPU a datagram",
		mode == LINE_UDP_PLAIN ? "recvfrom" : "recvmmsg", us);
	line_udp_rx_free(r);
	close(rx);
	close(tx);
}

/*
 * A datagram that waited in the socket while the thread was busy keeps
 * the time the kernel received it, in both receive modes.
 */
static void receive_timestamps(gconstpointer data)
{
	enum line_udp_mode mode = GPOINTER_TO_INT(data);
	struct sockaddr_in rx_addr, tx_addr;
	int rx = bound_socket(&rx_addr), tx = bound_socket(&tx_addr);
	struct line_udp_rx *r = line_udp_rx_new(rx, mode);
	guint8 packet[200] = { 1 }, got[LINE_UDP_RX_SIZE];
	struct sockaddr_storage from;
	socklen_t len;

	g_assert_true(line_udp_rx_timestamps(r));
	gint64 sent = g_get_monotonic_time();

	for (guint i = 0; i < 3; i++)
		g_assert_cmpint(sendto(tx, packet, sizeof(packet), 0,
				       (void *)&rx_addr, sizeof(rx_addr)),
				==, sizeof(packet));
	/* The call thread encoding a picture. */
	g_usleep(30000);
	for (guint i = 0; i < 3; i++) {
		g_assert_cmpint(line_udp_rx_next(r, got, sizeof(got), &from,
						 &len),
				==, sizeof(packet));
		gint64 arrival = line_udp_rx_arrival(r);

		g_assert_cmpint(arrival, >=, sent - 1000);
		g_assert_cmpint(arrival, <, sent + 20000);
	}
	g_assert_cmpint(g_get_monotonic_time() - sent, >=, 30000);
	line_udp_rx_free(r);
	/* Switched off, the arrival is when the thread read the socket. */
	g_setenv("LINE_DISABLE_RX_TIMESTAMPS", "1", TRUE);
	r = line_udp_rx_new(rx, mode);
	g_assert_false(line_udp_rx_timestamps(r));
	g_unsetenv("LINE_DISABLE_RX_TIMESTAMPS");
	sent = g_get_monotonic_time();
	g_assert_cmpint(sendto(tx, packet, sizeof(packet), 0, (void *)&rx_addr,
			       sizeof(rx_addr)),
			==, sizeof(packet));
	g_usleep(30000);
	g_assert_cmpint(line_udp_rx_next(r, got, sizeof(got), &from, &len), ==,
			sizeof(packet));
	g_assert_cmpint(line_udp_rx_arrival(r), >=, sent + 30000);
	line_udp_rx_free(r);
	close(rx);
	close(tx);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/udp-batch/send/sendto",
			     GINT_TO_POINTER(LINE_UDP_PLAIN), send_modes);
	g_test_add_data_func("/udp-batch/send/sendmmsg",
			     GINT_TO_POINTER(LINE_UDP_MMSG), send_modes);
	g_test_add_data_func("/udp-batch/send/segment",
			     GINT_TO_POINTER(LINE_UDP_GSO), send_modes);
	g_test_add_func("/udp-batch/send/segment-refused", gso_refused);
	g_test_add_func("/udp-batch/send/destination-change",
			destination_change);
	g_test_add_data_func("/udp-batch/receive/recvfrom",
			     GINT_TO_POINTER(LINE_UDP_PLAIN), receive_modes);
	g_test_add_data_func("/udp-batch/receive/recvmmsg",
			     GINT_TO_POINTER(LINE_UDP_MMSG), receive_modes);
	g_test_add_data_func("/udp-batch/receive/timestamps/recvfrom",
			     GINT_TO_POINTER(LINE_UDP_PLAIN),
			     receive_timestamps);
	g_test_add_data_func("/udp-batch/receive/timestamps/recvmmsg",
			     GINT_TO_POINTER(LINE_UDP_MMSG),
			     receive_timestamps);
	g_test_add_func("/udp-batch/sandbox-fallback", sandbox_fallback);
	g_test_add_func("/udp-batch/default-mode", default_mode);
	g_test_add_func("/udp-batch/traffic-class", traffic_class);
	for (int mode = LINE_UDP_PLAIN; mode <= LINE_UDP_GSO; mode++) {
		g_autofree char *send = g_strdup_printf(
			"/udp-batch/perf/send/%s", mode_names[mode]);

		g_test_add_data_func(send, GINT_TO_POINTER(mode), perf_send);
	}
	g_test_add_data_func("/udp-batch/perf/receive/recvfrom",
			     GINT_TO_POINTER(LINE_UDP_PLAIN), perf_receive);
	g_test_add_data_func("/udp-batch/perf/receive/recvmmsg",
			     GINT_TO_POINTER(LINE_UDP_MMSG), perf_receive);
	return g_test_run();
}
