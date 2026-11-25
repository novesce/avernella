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
 */
#include "udp_batch.h"
#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdlib.h>
#include <string.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

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

struct line_udp_rx {
	int fd;
	enum line_udp_mode mode;
	guint count, next;
	struct line_udp_stats stats;
	struct mmsghdr msgs[LINE_UDP_RX_BATCH];
	struct iovec iov[LINE_UDP_RX_BATCH];
	struct sockaddr_storage from[LINE_UDP_RX_BATCH];
	guint8 buf[LINE_UDP_RX_BATCH][LINE_UDP_RX_SIZE];
};

struct line_udp_rx *line_udp_rx_new(int fd, enum line_udp_mode mode)
{
	struct line_udp_rx *r = g_malloc0(sizeof(*r));

	r->fd = fd;
	r->mode = mode == LINE_UDP_PLAIN ? LINE_UDP_PLAIN : LINE_UDP_MMSG;
	for (guint i = 0; i < LINE_UDP_RX_BATCH; i++) {
		r->iov[i] = (struct iovec){ r->buf[i], LINE_UDP_RX_SIZE };
		r->msgs[i].msg_hdr.msg_iov = &r->iov[i];
		r->msgs[i].msg_hdr.msg_iovlen = 1;
		r->msgs[i].msg_hdr.msg_name = &r->from[i];
	}
	return r;
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
	ssize_t n;

	*from_len = sizeof(*from);
	do {
		r->stats.syscalls++;
		n = recvfrom(r->fd, data, size, MSG_DONTWAIT, (void *)from,
			     from_len);
	} while (n < 0 && errno == EINTR);
	r->stats.datagrams += n >= 0;
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
		for (guint i = 0; i < LINE_UDP_RX_BATCH; i++)
			r->msgs[i].msg_hdr.msg_namelen = sizeof(r->from[i]);
		do {
			r->stats.syscalls++;
			n = recvmmsg(r->fd, r->msgs, LINE_UDP_RX_BATCH,
				     MSG_DONTWAIT, NULL);
		} while (n < 0 && errno == EINTR);
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
