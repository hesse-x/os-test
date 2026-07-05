#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Constants (from xos/socket.h) =====================
#include <xos/socket.h>

// ===================== Syscall wrappers =====================
// These directly invoke the kernel syscalls via the inline assembly wrappers.

#include "syscall.h"

static inline int socket(int domain, int type, int protocol) {
  int64_t ret =
      __syscall3(SYS_SOCKET, (int64_t)domain, (int64_t)type, (int64_t)protocol);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

static inline int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
  int64_t ret = __syscall3(SYS_BIND, (int64_t)fd, (int64_t)(uintptr_t)addr,
                           (int64_t)addrlen);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

static inline int listen(int fd, int backlog) {
  int64_t ret = __syscall2(SYS_LISTEN, (int64_t)fd, (int64_t)backlog);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

static inline int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  int64_t ret = __syscall3(SYS_ACCEPT, (int64_t)fd, (int64_t)(uintptr_t)addr,
                           (int64_t)(uintptr_t)addrlen);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

static inline int connect(int fd, const struct sockaddr *addr,
                          socklen_t addrlen) {
  int64_t ret = __syscall3(SYS_CONNECT, (int64_t)fd, (int64_t)(uintptr_t)addr,
                           (int64_t)addrlen);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

static inline int socketpair(int domain, int type, int protocol, int sv[2]) {
  int64_t ret = __syscall4(SYS_SOCKETPAIR, (int64_t)domain, (int64_t)type,
                           (int64_t)protocol, (int64_t)(uintptr_t)sv);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

static inline ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
  int64_t ret = __syscall3(SYS_SENDMSG, (int64_t)fd, (int64_t)(uintptr_t)msg,
                           (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (ssize_t)ret;
}

static inline ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
  int64_t ret = __syscall3(SYS_RECVMSG, (int64_t)fd, (int64_t)(uintptr_t)msg,
                           (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (ssize_t)ret;
}

static inline int shutdown(int fd, int how) {
  int64_t ret = __syscall2(SYS_SHUTDOWN, (int64_t)fd, (int64_t)how);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

// ===================== sockaddr compat =====================
// In our kernel, struct sockaddr is the same as struct sockaddr_un for AF_UNIX.
// Define a minimal generic sockaddr for API compatibility.

#ifdef __cplusplus
}
#endif

#endif // _SYS_SOCKET_H
