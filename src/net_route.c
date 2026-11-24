// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_route.h"
#include <errno.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

int line_route_open(void)
{
	const struct sockaddr_nl address = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE,
	};
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
			NETLINK_ROUTE);

	if (fd >= 0 &&
	    bind(fd, (const struct sockaddr *)&address, sizeof(address))) {
		close(fd);
		return -1;
	}
	return fd;
}

gboolean line_route_restored(int fd, gboolean *lost)
{
	gboolean restored = FALSE;
	union {
		struct nlmsghdr header;
		char bytes[8192];
	} buf;

	for (;;) {
		struct sockaddr_nl from = { 0 };
		socklen_t size = sizeof(from);
		ssize_t n = recvfrom(fd, &buf, sizeof(buf), MSG_DONTWAIT,
				     (struct sockaddr *)&from, &size);

		if (n < 0 && (errno == EINTR || errno == ENOBUFS))
			continue;
		if (n <= 0)
			break;
		if (size != sizeof(from) || from.nl_family != AF_NETLINK ||
		    from.nl_pid)
			continue;
		for (struct nlmsghdr *h = &buf.header; NLMSG_OK(h, n);
		     h = NLMSG_NEXT(h, n)) {
			if ((h->nlmsg_type != RTM_NEWROUTE &&
			     h->nlmsg_type != RTM_DELROUTE) ||
			    NLMSG_PAYLOAD(h, 0) < sizeof(struct rtmsg))
				continue;
			const struct rtmsg *route = NLMSG_DATA(h);

			if (route->rtm_table != RT_TABLE_MAIN ||
			    route->rtm_type != RTN_UNICAST ||
			    route->rtm_dst_len || route->rtm_src_len ||
			    (route->rtm_family != AF_INET &&
			     route->rtm_family != AF_INET6))
				continue;
			/* A lease renewal may announce an unchanged default. */
			if (h->nlmsg_type == RTM_DELROUTE) {
				*lost = TRUE;
			} else if (*lost) {
				*lost = FALSE;
				restored = TRUE;
			}
		}
	}
	return restored;
}
