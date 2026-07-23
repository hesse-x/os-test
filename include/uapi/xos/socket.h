/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_SOCKET_H
#define COMMON_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Address family type =====================
typedef uint16_t sa_family_t;

// ===================== Address family =====================
#define AF_UNIX 1
#define AF_NETLINK 2
#define AF_LOCAL AF_UNIX

// ===================== Socket types =====================
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_SEQPACKET 5

// Socket type flags (ORed with type)
#define SOCK_CLOEXEC 02000000 /* 0x80000 — set FD_CLOEXEC on new fd */
#define SOCK_NONBLOCK 04000   /* 0x800  — set O_NONBLOCK on new fd */

// ===================== Protocol =====================
#define SOL_SOCKET 1
#define SCM_RIGHTS 1

// ===================== Shutdown how =====================
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

// ===================== Poll events =====================
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
#define POLLRDHUP 0x400

// ===================== sockaddr (generic) =====================
typedef struct sockaddr {
  sa_family_t sa_family; // address family (AF_UNIX = 1)
  char sa_data[14];      // address data
} sockaddr;

// ===================== sockaddr_un =====================
#define UNIX_PATH_MAX 108

typedef struct sockaddr_un {
  sa_family_t sun_family;       // AF_UNIX = 1
  char sun_path[UNIX_PATH_MAX]; // path or abstract (\0 prefix)
} sockaddr_un;

// ===================== iovec =====================
typedef struct iovec {
  void *iov_base; // buffer address
  size_t iov_len; // buffer length
} iovec;

// ===================== cmsghdr / CMSG macros =====================
// Linux UAPI compatible layout (cmsg_len is socklen_t = uint32_t on x86-64)
typedef struct cmsghdr {
  uint32_t cmsg_len; // data byte count including header (socklen_t)
  int cmsg_level;    // originating protocol
  int cmsg_type;     // protocol-specific type
                     // followed by unsigned char cmsg_data[];
} cmsghdr;

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) ((void *)(((char *)(cmsg)) + sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(msg, cmsg)                                                 \
  (((char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) >=                           \
    (char *)((msg)->msg_control) + (msg)->msg_controllen)                      \
       ? (struct cmsghdr *)NULL                                                \
       : (struct cmsghdr *)((char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_FIRSTHDR(msg)                                                     \
  ((msg)->msg_control && (msg)->msg_controllen >= sizeof(struct cmsghdr)       \
       ? (struct cmsghdr *)(msg)->msg_control                                  \
       : (struct cmsghdr *)NULL)
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))

// ===================== msghdr =====================
// Linux UAPI x86-64 exact layout
typedef struct msghdr {
  void *msg_name;        // optional address
  uint32_t msg_namelen;  // 4-byte
  unsigned int __pad0;   // padding 4-byte
  struct iovec *msg_iov; // scatter/gather array
  size_t msg_iovlen;     // # elements
  void *msg_control;     // ancillary data (SCM_RIGHTS)
  size_t msg_controllen; // ancillary data size
  int msg_flags;         // flags on received message
} msghdr;

// ===================== pollfd =====================
typedef unsigned long nfds_t;

typedef struct pollfd {
  int fd;        // fd to poll
  short events;  // requested events
  short revents; // returned events
} pollfd;

// ===================== Flags for sendmsg/recvmsg =====================
#define MSG_EOR 0x80        // end of record
#define MSG_TRUNC 0x20      // data truncated
#define MSG_CTRUNC 0x08     // control data truncated
#define MSG_OOB 0x01        // out-of-band data
#define MSG_DONTWAIT 0x40   // nonblocking
#define MSG_PEEK 0x02       // peek without consuming
#define MSG_WAITALL 0x100   // block until full request is satisfied
#define MSG_NOSIGNAL 0x4000 // don't raise SIGPIPE on EPIPE
// Defined for UAPI completeness; not yet implemented (see doc/design/todo.md).
#define MSG_ERRQUEUE 0x2000         // socket error queue (no infra)
#define MSG_PROBE 0x10              // probe connection without sending
#define MSG_CONFIRM 0x800           // confirm path validity
#define MSG_MORE 0x8000             // coalesce pending sends
#define MSG_CMSG_CLOEXEC 0x40000000 // set CLOEXEC on SCM_RIGHTS fds (needs S06)

// ===================== Socket options (SOL_SOCKET level) =====================
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_NO_CHECK 11
#define SO_PRIORITY 12
#define SO_LINGER 13
#define SO_BSDCOMPAT 14
#define SO_REUSEPORT 15
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_ACCEPTCONN 30

// ===================== socklen_t =====================
typedef uint32_t socklen_t;

// ===================== SCM_RIGHTS fd count limit =====================
#define SCM_MAX_FD 8

#ifdef __cplusplus
}
#endif

#endif // COMMON_SOCKET_H
