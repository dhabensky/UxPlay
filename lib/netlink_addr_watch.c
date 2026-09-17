/**
 * UxPlay - An open-source AirPlay mirroring server
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "netlink_addr_watch.h"

#include <string.h>

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

int netlink_addr_watch_open(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool netlink_addr_watch_is_addr_change(const void *buf, size_t len) {
    const struct nlmsghdr *nlh = (const struct nlmsghdr *) buf;
    for (; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
        if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
            return true;
        }
        if (nlh->nlmsg_type == NLMSG_DONE || nlh->nlmsg_type == NLMSG_ERROR) {
            break;
        }
    }
    return false;
}

#else /* non-Linux: no netlink, caller falls back to polling */

int netlink_addr_watch_open(void) {
    return -1;
}

bool netlink_addr_watch_is_addr_change(const void *buf, size_t len) {
    (void) buf;
    (void) len;
    return false;
}

#endif
