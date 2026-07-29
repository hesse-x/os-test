/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_SOCKET_H
#define COMMON_SOCKET_H

/* musl's <sys/un.h> hardcodes sun_path[108] and does not export UNIX_PATH_MAX;
 * define it up here so the kernel-side sockaddr_un below can use it. */
#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

/*
 * Socket UAPI. Two faces, one source:
 *
 *  - User TUs (no __KERNEL__): forward to musl's <sys/socket.h>/<sys/un.h> for
 *    sockaddr/msghdr/cmsghdr/iovec/socklen_t and the AF/SOCK/SO/MSG/SHUT/SOL/
 *    CMSG/SCM constants, so the kernel UAPI header and the libc header never
 *    define those twice in the same TU (musl's headers can't be edited to honor
 *    our guards). musl/include + musl/arch/x86_64 are on the global user
 *    include path via MUSL_INCLUDE_FLAGS (after user/include), so these resolve
 *    to upstream musl directly — nothing of musl's is copied here.
 *
 *  - Kernel TUs (__KERNEL__): musl is not on the kernel include path, so this
 *    header is self-contained for the socket types. Layouts/values are aligned
 *    to musl's x86_64 definitions and locked by _Static_assert below — the
 *    single source of truth that the two faces stay byte-compatible.
 *
 * pollfd, the POLL_xxx flags and nfds_t are NOT in musl's <sys/socket.h> (musl
 * puts them in <poll.h>); they are xos UAPI, defined once in the common section
 * below for both faces (the <sys/poll.h> shim and direct includers share this).
 */

#ifndef __KERNEL__

#include <sys/socket.h>
#include <sys/un.h>

#else /* __KERNEL__ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Address family type =====================
typedef uint16_t sa_family_t;

// ===================== Address family =====================
#define AF_UNIX 1
#define AF_NETLINK 16
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

// ===================== sockaddr (generic) =====================
typedef struct sockaddr {
  sa_family_t sa_family; // address family (AF_UNIX = 1)
  char sa_data[14];      // address data
} sockaddr;

// ===================== sockaddr_un =====================
typedef struct sockaddr_un {
  sa_family_t sun_family;       // AF_UNIX = 1
  char sun_path[UNIX_PATH_MAX]; // path or abstract (\0 prefix)
} sockaddr_un;

// ===================== iovec =====================
// Guard with musl's __DEFINED_struct_iovec idiom: musl's
// <fcntl.h>/<sys/socket.h> set __NEED_struct_iovec under _GNU_SOURCE (which
// clang++ predefines for all C++ TUs), so musl's <bits/alltypes.h> also defines
// struct iovec. Kernel TUs have no musl alltypes, so define it here and set the
// flag so a later musl alltypes (never, in-kernel) would skip.
#if !defined(__DEFINED_struct_iovec)
typedef struct iovec {
  void *iov_base; // buffer address
  size_t iov_len; // buffer length
} iovec;
#define __DEFINED_struct_iovec
#endif

// ===================== cmsghdr / CMSG macros =====================
// musl arch/x86_64 layout: cmsg_len + __pad1 + cmsg_level + cmsg_type (16 B).
// recvmsg writes this struct straight into the user control buffer for
// SCM_RIGHTS (kernel/bsd/socket.c), so the field offsets MUST match what
// musl's user-side CMSG_* macros assume.
typedef struct cmsghdr {
  uint32_t cmsg_len; // data byte count including header (socklen_t)
  int __pad1;        // padding (matches musl/glibc x86-64 cmsghdr)
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
// Byte-compatible with musl arch/x86_64 msghdr: the size_t msg_iovlen and
// msg_controllen slots coincide with musl's (int + __pad) on little-endian
// (value in the low 4 bytes). _Static_assert below pins the offsets/size.
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

// ===================== Flags for sendmsg/recvmsg =====================
#define MSG_EOR 0x80                // end of record
#define MSG_TRUNC 0x20              // data truncated
#define MSG_CTRUNC 0x08             // control data truncated
#define MSG_OOB 0x01                // out-of-band data
#define MSG_DONTWAIT 0x40           // nonblocking
#define MSG_PEEK 0x02               // peek without consuming
#define MSG_WAITALL 0x100           // block until full request is satisfied
#define MSG_NOSIGNAL 0x4000         // don't raise SIGPIPE on EPIPE
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

#ifdef __cplusplus
}
#endif

// ===================== Layout/value parity with musl x86_64 (static assert)
// ===================== Single source of truth that the kernel self-contained
// definitions above stay byte-compatible with musl's
// <sys/socket.h>/<bits/socket.h> on x86-64. If any constant or field offset
// drifts away from the Linux/musl standard, the kernel build fails here.
_Static_assert(AF_UNIX == 1 && AF_NETLINK == 16 && AF_LOCAL == 1,
               "xos AF_* must match musl/Linux");
_Static_assert(SOCK_STREAM == 1 && SOCK_DGRAM == 2 && SOCK_SEQPACKET == 5 &&
                   SOCK_CLOEXEC == 02000000 && SOCK_NONBLOCK == 04000,
               "xos SOCK_* must match musl/Linux");
_Static_assert(SOL_SOCKET == 1 && SCM_RIGHTS == 1,
               "xos SOL_SOCKET/SCM_RIGHTS must match musl/Linux");
_Static_assert(SHUT_RD == 0 && SHUT_WR == 1 && SHUT_RDWR == 2,
               "xos SHUT_* must match musl/Linux");
_Static_assert(offsetof(msghdr, msg_iovlen) == 24,
               "msghdr.msg_iovlen offset must match musl x86_64");
_Static_assert(offsetof(msghdr, msg_control) == 32,
               "msghdr.msg_control offset must match musl x86_64");
_Static_assert(offsetof(msghdr, msg_controllen) == 40,
               "msghdr.msg_controllen offset must match musl x86_64");
_Static_assert(offsetof(msghdr, msg_flags) == 48,
               "msghdr.msg_flags offset must match musl x86_64");
_Static_assert(sizeof(msghdr) == 56, "msghdr size must match musl x86_64 (56)");
_Static_assert(offsetof(cmsghdr, cmsg_level) == 8,
               "cmsghdr.cmsg_level offset must match musl x86_64 (8)");
_Static_assert(offsetof(cmsghdr, cmsg_type) == 12,
               "cmsghdr.cmsg_type offset must match musl x86_64 (12)");
_Static_assert(sizeof(cmsghdr) == 16,
               "cmsghdr size must match musl x86_64 (16)");

#endif /* __KERNEL__ */

// ===================== pollfd / POLL flags (common: xos UAPI, not in musl
// socket.h) =====================
#ifdef __cplusplus
extern "C" {
#endif

// musl's <sys/socket.h> does not define pollfd, the POLL_xxx flags, or nfds_t
// (they live in musl <poll.h>); define them here so <sys/poll.h> and direct
// <xos/socket.h> includers see them in both faces without pulling musl
// <poll.h> (which would need <bits/poll.h> plumbing). Guard against a prior
// musl <poll.h>.
#ifndef __DEFINED_pollfd
typedef unsigned long nfds_t;
typedef struct pollfd {
  int fd;        // fd to poll
  short events;  // requested events
  short revents; // returned events
} pollfd;
#define __DEFINED_pollfd
#endif

#ifndef POLLIN
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
#endif

// Kernel-private SCM_RIGHTS fd cap; not in musl.
#ifndef SCM_MAX_FD
#define SCM_MAX_FD 8
#endif

#ifdef __cplusplus
}
#endif

#endif // COMMON_SOCKET_H
