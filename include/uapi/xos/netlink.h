/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_NETLINK_H
#define XOS_NETLINK_H

#include <stdint.h>

// AF_NETLINK comes from <xos/socket.h> (kernel) / musl <sys/socket.h> (user),
// both defining the Linux-standard value 16. Do not redefine here — musl's
// AF_NETLINK expands to PF_NETLINK, a different token sequence, which would
// trip -Werror on redefinition.

// ===================== Netlink protocol / group =====================
#define NETLINK_KOBJECT_UEVENT 1

// ===================== Netlink message types =====================
#define NLMSG_UEVENT_ADD 1
#define NLMSG_UEVENT_REMOVE 2
#define NLMSG_UEVENT_CHANGE 3

// ===================== nlmsghdr (Linux UAPI compatible) =====================
#define NLMSG_ALIGNTO 4
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN ((int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh) ((void *)((char *)(nlh) + NLMSG_HDRLEN))
#define NLMSG_NEXT(nlh, len)                                                   \
  ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len),                                     \
   (struct nlmsghdr *)((char *)(nlh) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(nlh, len)                                                     \
  ((len) >= (int)sizeof(struct nlmsghdr) &&                                    \
   (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && (nlh)->nlmsg_len <= (len))

typedef struct nlmsghdr {
  uint32_t nlmsg_len;   // total message length including header
  uint16_t nlmsg_type;  // message type
  uint16_t nlmsg_flags; // additional flags
  uint32_t nlmsg_seq;   // sequence number
  uint32_t nlmsg_pid;   // sending process port ID
} nlmsghdr;

// ===================== sockaddr_nl =====================
#include <xos/socket.h> // sa_family_t

typedef struct sockaddr_nl {
  sa_family_t nl_family; // AF_NETLINK = 16
  uint16_t nl_pad;       // padding
  uint32_t nl_pid;       // port ID (0 = auto-assign PID)
  uint32_t nl_groups;    // subscribed group bitmask
} sockaddr_nl;

#endif // XOS_NETLINK_H
