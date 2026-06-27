#ifndef COMMON_SOCKET_H
#define COMMON_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "common/macro.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Address family =====================
#define AF_UNIX     1
#define AF_LOCAL    AF_UNIX

// ===================== Socket types =====================
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_SEQPACKET 5

// ===================== Protocol =====================
#define SOL_SOCKET  1
#define SCM_RIGHTS  1

// ===================== Shutdown how =====================
#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

// ===================== Poll events =====================
#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010

// ===================== sockaddr (generic) =====================
typedef struct sockaddr {
    uint16_t sa_family;            // address family (AF_UNIX = 1)
    char     sa_data[14];          // address data
} sockaddr_t;

// ===================== sockaddr_un =====================
#define UNIX_PATH_MAX 108

typedef struct sockaddr_un {
    uint16_t sun_family;            // AF_UNIX = 1
    char     sun_path[UNIX_PATH_MAX]; // path or abstract (\0 prefix)
} sockaddr_un_t;

// ===================== iovec =====================
typedef struct iovec {
    void   *iov_base;   // buffer address
    size_t  iov_len;    // buffer length
} iovec_t;

// ===================== cmsghdr / CMSG macros =====================
// Linux UAPI compatible layout (cmsg_len is socklen_t = uint32_t on x86-64)
typedef struct cmsghdr {
    uint32_t cmsg_len;    // data byte count including header (socklen_t)
    int      cmsg_level;  // originating protocol
    int      cmsg_type;   // protocol-specific type
    // followed by unsigned char cmsg_data[];
} cmsghdr_t;

#define CMSG_ALIGN(len)     ALIGN_UP(len, sizeof(size_t))
#define CMSG_DATA(cmsg)     ((void *)(((char *)(cmsg)) + sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(msg, cmsg)                                          \
    (((char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) >=                  \
      (char *)((msg)->msg_control) + (msg)->msg_controllen) ?           \
     (struct cmsghdr *)NULL :                                            \
     (struct cmsghdr *)((char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_FIRSTHDR(msg)                                              \
    ((msg)->msg_control && (msg)->msg_controllen >= sizeof(struct cmsghdr) ? \
     (struct cmsghdr *)(msg)->msg_control : (struct cmsghdr *)NULL)

// ===================== msghdr =====================
// Linux UAPI x86-64 exact layout
typedef struct msghdr {
    void            *msg_name;       // optional address
    uint32_t         msg_namelen;    // 4-byte
    unsigned int     __pad0;         // padding 4-byte
    struct iovec    *msg_iov;        // scatter/gather array
    size_t           msg_iovlen;     // # elements
    void            *msg_control;    // ancillary data (SCM_RIGHTS)
    size_t           msg_controllen; // ancillary data size
    int              msg_flags;      // flags on received message
} msghdr_t;

// ===================== pollfd =====================
typedef unsigned long nfds_t;

typedef struct pollfd {
    int    fd;         // fd to poll
    short  events;     // requested events
    short  revents;    // returned events
} pollfd_t;

// ===================== Flags for sendmsg/recvmsg =====================
#define MSG_EOR     0x80    // end of record
#define MSG_TRUNC   0x20    // data truncated
#define MSG_CTRUNC  0x08    // control data truncated
#define MSG_OOB     0x01    // out-of-band data
#define MSG_DONTWAIT 0x40   // nonblocking

// ===================== socklen_t =====================
typedef uint32_t socklen_t;

// ===================== SCM_RIGHTS fd count limit =====================
#define SCM_MAX_FD 8

#ifdef __cplusplus
}
#endif

#endif // COMMON_SOCKET_H
