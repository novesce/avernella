// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Batched UDP for call media.
 *
 * The call thread used to pay one poll() and one recvfrom() for every
 * datagram, and one sendto() for every RTP packet of a video picture. A
 * picture is sent as a back-to-back burst of equal-size packets, so the
 * whole burst can leave in one sendmmsg(), and each run of equal sizes in
 * one UDP_SEGMENT message that the kernel splits into the same datagrams
 * after a single trip through the UDP stack. On receive, recvmmsg() drains
 * what is waiting at once.
 *
 * The datagrams on the wire are unchanged. A kernel without a feature, or
 * a seccomp filter that refuses it, moves that socket to the next simpler
 * mode for good: UDP_SEGMENT -> sendmmsg() -> sendto().
 *
 * Each datagram also carries the time the kernel received it
 * (SO_TIMESTAMPNS), so jitter buffers and congestion estimates measure
 * the network, not how long the call thread was busy before it read the
 * socket.
 */
#include "udp_batch.h"
#include <errno.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

/*
 * Receive timestamps: SO_TIMESTAMPNS_NEW (Linux 5.1) carries a 64-bit
 * struct __kernel_timespec on every architecture; the old option carries
 * the native struct timespec.
 */
#define LINE_SO_TIMESTAMPNS_OLD 35
#define LINE_SO_TIMESTAMPNS_NEW 64

struct line_kernel_timespec {
	gint64 sec, nsec;
};

/* Payload limit for one segmented message (IPv4 and IPv6 alike). */
#define GSO_PAYLOAD_MAX 65000

enum line_udp_mode line_udp_default_mode(void)
{
	const char *plain = getenv("LINE_DISABLE_UDP_BATCH");

	return plain && *plain && strcmp(plain, "0") ? LINE_UDP_PLAIN :
						       LINE_UDP_GSO;
}

/* Refusals that mean "this kernel or sandbox does not offer it". */
static gboolean unsupported(int err)
{
	return err == ENOSYS || err == EPERM || err == EOPNOTSUPP;
}

#define RX_CONTROL CMSG_SPACE(sizeof(struct line_kernel_timespec))

struct line_udp_rx {
	int fd;
	enum line_udp_mode mode;
	guint count, next;
	int stamp_type;
	/* CLOCK_MONOTONIC minus CLOCK_REALTIME at the last receive call. */
	gint64 clock_offset, received_at, arrival;
	struct line_udp_stats stats;
	struct mmsghdr msgs[LINE_UDP_RX_BATCH];
	struct iovec iov[LINE_UDP_RX_BATCH];
	struct sockaddr_storage from[LINE_UDP_RX_BATCH];
	/* size_t gives struct cmsghdr its alignment. */
	size_t control[LINE_UDP_RX_BATCH][RX_CONTROL / sizeof(size_t) + 1];
	guint8 buf[LINE_UDP_RX_BATCH][LINE_UDP_RX_SIZE];
};

static int enable_timestamps(int fd)
{
	int on = 1;

	if (!setsockopt(fd, SOL_SOCKET, LINE_SO_TIMESTAMPNS_NEW, &on,
			sizeof(on)))
		return LINE_SO_TIMESTAMPNS_NEW;
	if (!setsockopt(fd, SOL_SOCKET, LINE_SO_TIMESTAMPNS_OLD, &on,
			sizeof(on)))
		return LINE_SO_TIMESTAMPNS_OLD;
	return 0;
}

struct line_udp_rx *line_udp_rx_new(int fd, enum line_udp_mode mode)
{
	struct line_udp_rx *r = g_malloc0(sizeof(*r));

	r->fd = fd;
	r->mode = mode == LINE_UDP_PLAIN ? LINE_UDP_PLAIN : LINE_UDP_MMSG;
	r->stamp_type = line_udp_timestamps_enabled() ? enable_timestamps(fd) :
							0;
	for (guint i = 0; i < LINE_UDP_RX_BATCH; i++) {
		r->iov[i] = (struct iovec){ r->buf[i], LINE_UDP_RX_SIZE };
		r->msgs[i].msg_hdr.msg_iov = &r->iov[i];
		r->msgs[i].msg_hdr.msg_iovlen = 1;
		r->msgs[i].msg_hdr.msg_name = &r->from[i];
	}
	return r;
}

gboolean line_udp_timestamps_enabled(void)
{
	const char *off = getenv("LINE_DISABLE_RX_TIMESTAMPS");

	return !off || !*off || !strcmp(off, "0");
}

gboolean line_udp_rx_timestamps(const struct line_udp_rx *r)
{
	return r && r->stamp_type;
}

gint64 line_udp_rx_arrival(const struct line_udp_rx *r)
{
	return r ? r->arrival : 0;
}

/* Both clocks around the receive call, to turn kernel stamps monotonic. */
static void sample_clocks(struct line_udp_rx *r)
{
	struct timespec real;

	r->received_at = g_get_monotonic_time();
	if (!r->stamp_type)
		return;
	clock_gettime(CLOCK_REALTIME, &real);
	r->clock_offset = r->received_at -
			  (gint64)real.tv_sec * G_USEC_PER_SEC -
			  real.tv_nsec / 1000;
}

/*
 * The kernel's receive time of one message, on the monotonic clock. A
 * missing stamp, or one outside the last two seconds (a clock step),
 * reads as now.
 */
static gint64 stamp_of(const struct line_udp_rx *r, struct msghdr *m)
{
	if (!r->stamp_type)
		return r->received_at;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
		gint64 sec, nsec;

		if (c->cmsg_level != SOL_SOCKET ||
		    c->cmsg_type != r->stamp_type)
			continue;
		if (r->stamp_type == LINE_SO_TIMESTAMPNS_NEW &&
		    c->cmsg_len >=
			    CMSG_LEN(sizeof(struct line_kernel_timespec))) {
			struct line_kernel_timespec t;

			memcpy(&t, CMSG_DATA(c), sizeof(t));
			sec = t.sec;
			nsec = t.nsec;
		} else if (r->stamp_type == LINE_SO_TIMESTAMPNS_OLD &&
			   c->cmsg_len >= CMSG_LEN(sizeof(struct timespec))) {
			struct timespec t;

			memcpy(&t, CMSG_DATA(c), sizeof(t));
			sec = t.tv_sec;
			nsec = t.tv_nsec;
		} else {
			continue;
		}
		gint64 at =
			sec * G_USEC_PER_SEC + nsec / 1000 + r->clock_offset;

		if (at <= r->received_at &&
		    at > r->received_at - 2 * G_USEC_PER_SEC)
			return at;
	}
	return r->received_at;
}

void line_udp_rx_free(struct line_udp_rx *r)
{
	g_free(r);
}

gboolean line_udp_rx_pending(const struct line_udp_rx *r)
{
	return r && r->next < r->count;
}

enum line_udp_mode line_udp_rx_mode(const struct line_udp_rx *r)
{
	return r->mode;
}

struct line_udp_stats line_udp_rx_stats(const struct line_udp_rx *r)
{
	return r->stats;
}

static ssize_t rx_plain(struct line_udp_rx *r, guint8 *data, gsize size,
			struct sockaddr_storage *from, socklen_t *from_len)
{
	struct iovec iov = { data, size };
	struct msghdr m = { .msg_name = from,
			    .msg_namelen = sizeof(*from),
			    .msg_iov = &iov,
			    .msg_iovlen = 1,
			    .msg_control = r->control[0],
			    .msg_controllen = r->stamp_type ? RX_CONTROL : 0 };
	ssize_t n;

	do {
		r->stats.syscalls++;
		n = recvmsg(r->fd, &m, MSG_DONTWAIT);
	} while (n < 0 && errno == EINTR);
	sample_clocks(r);
	*from_len = m.msg_namelen;
	r->stats.datagrams += n >= 0;
	r->arrival = n >= 0 ? stamp_of(r, &m) : 0;
	return n;
}

ssize_t line_udp_rx_next(struct line_udp_rx *r, guint8 *data, gsize size,
			 struct sockaddr_storage *from, socklen_t *from_len)
{
	if (r->next >= r->count) {
		int n;

		r->count = r->next = 0;
		if (r->mode == LINE_UDP_PLAIN)
			return rx_plain(r, data, size, from, from_len);
		for (guint i = 0; i < LINE_UDP_RX_BATCH; i++) {
			struct msghdr *h = &r->msgs[i].msg_hdr;

			h->msg_namelen = sizeof(r->from[i]);
			h->msg_control = r->stamp_type ? r->control[i] : NULL;
			h->msg_controllen = r->stamp_type ? RX_CONTROL : 0;
		}
		do {
			r->stats.syscalls++;
			n = recvmmsg(r->fd, r->msgs, LINE_UDP_RX_BATCH,
				     MSG_DONTWAIT, NULL);
		} while (n < 0 && errno == EINTR);
		sample_clocks(r);
		if (n < 0 && unsupported(errno)) {
			r->mode = LINE_UDP_PLAIN;
			return rx_plain(r, data, size, from, from_len);
		}
		if (n <= 0) {
			if (!n)
				errno = EAGAIN;
			return -1;
		}
		r->count = (guint)n;
		r->stats.datagrams += (guint)n;
	}
	struct mmsghdr *m = &r->msgs[r->next];
	gsize len = MIN((gsize)m->msg_len, size);
	socklen_t name = MIN(m->msg_hdr.msg_namelen, (socklen_t)sizeof(*from));

	memcpy(data, r->buf[r->next], len);
	memcpy(from, &r->from[r->next], name);
	*from_len = name;
	r->arrival = stamp_of(r, &m->msg_hdr);
	r->next++;
	return (ssize_t)len;
}

struct line_udp_tx {
	int fd;
	enum line_udp_mode mode;
	guint count;
	struct line_udp_stats stats;
	struct sockaddr_storage to;
	socklen_t to_len;
	gsize sizes[LINE_UDP_TX_BATCH];
	struct iovec iov[LINE_UDP_TX_BATCH];
	struct mmsghdr msgs[LINE_UDP_TX_BATCH];
	/* size_t gives struct cmsghdr its alignment. */
	size_t control[LINE_UDP_TX_BATCH]
		      [CMSG_SPACE(sizeof(guint16)) / sizeof(size_t)];
	guint8 buf[LINE_UDP_TX_BATCH][LINE_UDP_TX_SIZE];
};

struct line_udp_tx *line_udp_tx_new(int fd, enum line_udp_mode mode)
{
	struct line_udp_tx *t = g_malloc0(sizeof(*t));

	t->fd = fd;
	t->mode = mode;
	return t;
}

void line_udp_tx_free(struct line_udp_tx *t)
{
	g_free(t);
}

gboolean line_udp_tx_pending(const struct line_udp_tx *t)
{
	return t && t->count;
}

enum line_udp_mode line_udp_tx_mode(const struct line_udp_tx *t)
{
	return t->mode;
}

struct line_udp_stats line_udp_tx_stats(const struct line_udp_tx *t)
{
	return t->stats;
}

static gboolean tx_plain(struct line_udp_tx *t, guint first)
{
	for (guint i = first; i < t->count; i++) {
		ssize_t n;

		do {
			t->stats.syscalls++;
			n = sendto(t->fd, t->buf[i], t->sizes[i], MSG_NOSIGNAL,
				   (const void *)&t->to, t->to_len);
		} while (n < 0 && errno == EINTR);
		if (n != (ssize_t)t->sizes[i])
			return FALSE;
		t->stats.datagrams++;
	}
	return TRUE;
}

/*
 * Packets from first in up to LINE_UDP_TX_BATCH messages. With UDP_SEGMENT
 * a message carries a run of equal sizes plus, optionally, one shorter
 * final datagram, which is what the kernel accepts. Returns the number of
 * messages and stores each message's packet count in counts.
 */
static guint tx_build(struct line_udp_tx *t, guint first, guint *counts)
{
	guint messages = 0;

	for (guint i = first; i < t->count; messages++) {
		struct msghdr *h = &t->msgs[messages].msg_hdr;
		gsize segment = t->sizes[i], total = 0;
		guint n = 0;

		*h = (struct msghdr){ .msg_name = &t->to,
				      .msg_namelen = t->to_len,
				      .msg_iov = &t->iov[i] };
		do {
			t->iov[i + n] = (struct iovec){ t->buf[i + n],
							t->sizes[i + n] };
			total += t->sizes[i + n];
			n++;
		} while (t->mode == LINE_UDP_GSO && i + n < t->count &&
			 t->sizes[i + n - 1] == segment &&
			 t->sizes[i + n] <= segment &&
			 total + t->sizes[i + n] <= GSO_PAYLOAD_MAX);
		h->msg_iovlen = n;
		if (n > 1) {
			struct cmsghdr *cm = (void *)t->control[messages];
			guint16 size = (guint16)segment;

			h->msg_control = cm;
			h->msg_controllen = sizeof(t->control[messages]);
			cm->cmsg_level = SOL_UDP;
			cm->cmsg_type = UDP_SEGMENT;
			cm->cmsg_len = CMSG_LEN(sizeof(size));
			memcpy(CMSG_DATA(cm), &size, sizeof(size));
		}
		counts[messages] = n;
		i += n;
	}
	return messages;
}

gboolean line_udp_tx_flush(struct line_udp_tx *t)
{
	guint counts[LINE_UDP_TX_BATCH];
	guint first = 0;
	gboolean ok = TRUE;

	while (ok && first < t->count) {
		if (t->mode == LINE_UDP_PLAIN) {
			ok = tx_plain(t, first);
			break;
		}
		guint messages = tx_build(t, first, counts);
		int sent;

		do {
			t->stats.syscalls++;
			sent = sendmmsg(t->fd, t->msgs, messages, MSG_NOSIGNAL);
		} while (sent < 0 && errno == EINTR);
		if (sent < 0) {
			int err = errno;

			/* Routes through IPsec give EIO; old kernels EINVAL. */
			if (t->mode == LINE_UDP_GSO &&
			    (err == EINVAL || err == EIO ||
			     err == ENOPROTOOPT || unsupported(err))) {
				t->mode = LINE_UDP_MMSG;
				continue;
			}
			if (unsupported(err)) {
				t->mode = LINE_UDP_PLAIN;
				continue;
			}
			ok = FALSE;
			break;
		}
		for (int m = 0; m < sent; m++) {
			first += counts[m];
			t->stats.datagrams += counts[m];
		}
	}
	t->count = 0;
	return ok;
}

gboolean line_udp_tx_queue(struct line_udp_tx *t, const void *to,
			   socklen_t to_len, const guint8 *data, gsize size)
{
	gboolean ok = TRUE;

	if (size > LINE_UDP_TX_SIZE || to_len > sizeof(t->to))
		return FALSE;
	if (t->count && (to_len != t->to_len || memcmp(to, &t->to, to_len) ||
			 t->count == LINE_UDP_TX_BATCH))
		ok = line_udp_tx_flush(t);
	if (!t->count) {
		memcpy(&t->to, to, to_len);
		t->to_len = to_len;
	}
	memcpy(t->buf[t->count], data, size);
	t->sizes[t->count++] = size;
	return ok;
}

gint64 line_udp_send_queue(int fd)
{
	const char *off = getenv("LINE_DISABLE_SEND_QUEUE");
	int held = 0;

	if ((off && *off && strcmp(off, "0")) || ioctl(fd, SIOCOUTQ, &held) ||
	    held < 0)
		return -1;
	return held;
}

static gboolean dscp_enabled(void)
{
	const char *mark = getenv("LINE_CALL_DSCP");

	return !mark || strcmp(mark, "0");
}

guint line_udp_tune(int fd, int family)
{
	int buffer = 1 << 20;
	int tos = LINE_DSCP_VIDEO << 2;

	/* Room for a keyframe burst and a Wi-Fi stall's backlog. */
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
	if (!dscp_enabled())
		return 0;
	if (family == AF_INET6 ?
		    setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos,
			       sizeof(tos)) :
		    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)))
		return 0;
	return LINE_DSCP_VIDEO;
}

ssize_t line_udp_send_class(int fd, int family, const void *to,
			    socklen_t to_len, const guint8 *data, gsize size,
			    guint dscp)
{
	union {
		struct cmsghdr header;
		guint8 space[CMSG_SPACE(sizeof(int))];
	} control = { 0 };
	struct iovec iov = { (void *)data, size };
	struct msghdr msg = { .msg_name = (void *)to,
			      .msg_namelen = to_len,
			      .msg_iov = &iov,
			      .msg_iovlen = 1 };
	ssize_t n;

	if (dscp && dscp_enabled()) {
		struct cmsghdr *c;
		int tos = dscp << 2;

		msg.msg_control = control.space;
		msg.msg_controllen = sizeof(control.space);
		c = CMSG_FIRSTHDR(&msg);
		c->cmsg_level = family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
		c->cmsg_type = family == AF_INET6 ? IPV6_TCLASS : IP_TOS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &tos, sizeof(tos));
	}
	do {
		n = sendmsg(fd, &msg, MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	if (n < 0 && msg.msg_control &&
	    (errno == EINVAL || unsupported(errno))) {
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		do {
			n = sendmsg(fd, &msg, MSG_NOSIGNAL);
		} while (n < 0 && errno == EINTR);
	}
	return n;
}
