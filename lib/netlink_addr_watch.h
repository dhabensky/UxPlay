/**
 * UxPlay - An open-source AirPlay mirroring server
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef NETLINK_ADDR_WATCH_H
#define NETLINK_ADDR_WATCH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens a non-blocking NETLINK_ROUTE socket subscribed to IPv4/IPv6
 * address-change events (RTMGRP_IPV4_IFADDR/RTMGRP_IPV6_IFADDR), e.g. a
 * DHCP renewal. Returns the fd, or -1 if the kernel doesn't support it
 * (caller should fall back to polling). */
int netlink_addr_watch_open(void);

/* Parses one recv()'d netlink message batch and reports whether it
 * contains an address add/remove event (RTM_NEWADDR/RTM_DELADDR). */
bool netlink_addr_watch_is_addr_change(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif //NETLINK_ADDR_WATCH_H
