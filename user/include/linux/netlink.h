/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _LINUX_NETLINK_H
#define _LINUX_NETLINK_H

// Linux-compatible netlink header for udev and other ported programs.
// Redirects to our UAPI definitions which follow the same layout.

#include <xos/netlink.h>

// Linux-specific protocol constants not in our xos/netlink.h
#define NETLINK_ROUTE 0    // Routing/device notifications (future)
#define NETLINK_UNUSED 1   // Unused
#define NETLINK_USERSOCK 2 // User-mode socket (future)
#define NETLINK_FIREWALL 3 // Firewall (future)

#endif /* _LINUX_NETLINK_H */
