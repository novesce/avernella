/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Batched UDP for call media: recvmmsg() receive batches and sendmmsg()
 * send batches with UDP generic segmentation offload (UDP_SEGMENT). Each
 * step falls back to plain recvfrom() and sendto() for the life of the
 * socket when the kernel or a seccomp filter refuses it.
 */
#ifndef LINE_UDP_BATCH_H
#define LINE_UDP_BATCH_H

#include <glib.h>
#include <sys/socket.h>
#include <sys/types.h>

enum line_udp_mode {
	LINE_UDP_PLAIN, /* one recvfrom() or sendto() a datagram */
	LINE_UDP_MMSG, /* recvmmsg() and sendmmsg() */
	LINE_UDP_GSO, /* sendmmsg() with UDP_SEGMENT for equal-size runs */
};

/* Datagrams kept from one recvmmsg() and the largest one accepted. */
#define LINE_UDP_RX_BATCH 16
#define LINE_UDP_RX_SIZE 4096
/* Datagrams queued before a send; the kernel's UDP_MAX_SEGMENTS is 64. */
#define LINE_UDP_TX_BATCH 64
#define LINE_UDP_TX_SIZE 1536

struct line_udp_stats {
	guint64 syscalls, datagrams;
};

struct line_udp_rx;
struct line_udp_tx;

/*
 * The best mode the environment allows: LINE_DISABLE_UDP_BATCH=1 selects
 * LINE_UDP_PLAIN. A socket still drops to a simpler mode at run time.
 */
enum line_udp_mode line_udp_default_mode(void);

struct line_udp_rx *line_udp_rx_new(int fd, enum line_udp_mode mode);
void line_udp_rx_free(struct line_udp_rx *r);
/*
 * Copies the next datagram into data, at most size bytes, and its source
 * into from. Returns the datagram length, or -1 with errno set (EAGAIN
 * when nothing is waiting). The socket must be non-blocking.
 */
ssize_t line_udp_rx_next(struct line_udp_rx *r, guint8 *data, gsize size,
			 struct sockaddr_storage *from, socklen_t *from_len);
/*
 * When the kernel received the datagram line_udp_rx_next() returned last,
 * on the g_get_monotonic_time() clock, from SO_TIMESTAMPNS. Without kernel
 * stamps it is the time of the receive call; 0 after a failed call.
 * LINE_DISABLE_RX_TIMESTAMPS=1 turns the stamps off.
 */
gint64 line_udp_rx_arrival(const struct line_udp_rx *r);
gboolean line_udp_rx_timestamps(const struct line_udp_rx *r);
gboolean line_udp_timestamps_enabled(void);
/* Datagrams already received and not yet returned. */
gboolean line_udp_rx_pending(const struct line_udp_rx *r);
enum line_udp_mode line_udp_rx_mode(const struct line_udp_rx *r);
struct line_udp_stats line_udp_rx_stats(const struct line_udp_rx *r);

struct line_udp_tx *line_udp_tx_new(int fd, enum line_udp_mode mode);
void line_udp_tx_free(struct line_udp_tx *t);
/*
 * Queues a copy of one datagram to the destination. A different
 * destination or a full queue sends what is queued first. Returns FALSE
 * when the datagram is too large or that earlier send failed.
 */
gboolean line_udp_tx_queue(struct line_udp_tx *t, const void *to,
			   socklen_t to_len, const guint8 *data, gsize size);
/* Sends everything queued, in order. FALSE if any datagram failed. */
gboolean line_udp_tx_flush(struct line_udp_tx *t);
gboolean line_udp_tx_pending(const struct line_udp_tx *t);
enum line_udp_mode line_udp_tx_mode(const struct line_udp_tx *t);
struct line_udp_stats line_udp_tx_stats(const struct line_udp_tx *t);

/*
 * DiffServ classes for call media (RFC 4594): Expedited Forwarding for
 * voice, AF41 for video and everything else on the call socket. Linux
 * maps them to the Wi-Fi WMM voice and video access categories.
 */
#define LINE_DSCP_VOICE 46
#define LINE_DSCP_VIDEO 34

/*
 * Prepares a call socket: 1 MiB send and receive buffers (the kernel caps
 * them at net.core.wmem_max and rmem_max) and AF41 as the default class.
 * LINE_CALL_DSCP=0 leaves packets unmarked. Returns the class in use.
 */
guint line_udp_tune(int fd, int family);
/*
 * Bytes of datagrams the kernel still holds for this socket: queued in
 * the qdisc, the Wi-Fi stack or the driver and not yet sent (SIOCOUTQ,
 * the socket's whole buffers). -1 if the kernel does not say.
 * LINE_DISABLE_SEND_QUEUE=1 always answers -1.
 */
gint64 line_udp_send_queue(int fd);
/*
 * sendto() with this datagram marked dscp, for voice on a socket whose
 * default class is video. Falls back to an unmarked send if refused.
 */
ssize_t line_udp_send_class(int fd, int family, const void *to,
			    socklen_t to_len, const guint8 *data, gsize size,
			    guint dscp);

#endif
