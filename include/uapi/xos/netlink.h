/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_NETLINK_H
#define XOS_NETLINK_H

#include <stdint.h>

// ===================== Address family =====================
#define AF_NETLINK 2

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
  uint32_t nlmsg_len;   // 消息总长度（包括头部）
  uint16_t nlmsg_type;  // 消息类型
  uint16_t nlmsg_flags; // 附加标志
  uint32_t nlmsg_seq;   // 序列号
  uint32_t nlmsg_pid;   // 发送进程端口 ID
} nlmsghdr;

// ===================== sockaddr_nl =====================
#include <xos/socket.h> // sa_family_t

typedef struct sockaddr_nl {
  sa_family_t nl_family; // AF_NETLINK = 2
  uint16_t nl_pad;       // 填充
  uint32_t nl_pid;       // 端口 ID（0 = 自动分配 PID）
  uint32_t nl_groups;    // 订阅的 group bitmask
} sockaddr_nl;

#endif // XOS_NETLINK_H
