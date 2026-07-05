/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fd_set — POSIX select(2) */
#define FD_SETSIZE 1024

typedef struct {
  unsigned long fds_bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) /
                         (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set)                                                           \
  do {                                                                         \
    size_t __i;                                                                \
    for (__i = 0; __i < sizeof(fd_set) / sizeof(long); __i++)                  \
      ((fd_set *)(set))->fds_bits[__i] = 0;                                   \
  } while (0)

#define FD_CLR(fd, set)                                                        \
  (((fd_set *)(set))                                                           \
       ->fds_bits[(fd) / (8 * sizeof(unsigned long))] &=                       \
       ~(1UL << ((fd) % (8 * sizeof(unsigned long)))))

#define FD_SET(fd, set)                                                        \
  (((fd_set *)(set))                                                           \
       ->fds_bits[(fd) / (8 * sizeof(unsigned long))] |=                       \
       1UL << ((fd) % (8 * sizeof(unsigned long))))

#define FD_ISSET(fd, set)                                                      \
  ((((const fd_set *)(set))                                                    \
        ->fds_bits[(fd) / (8 * sizeof(unsigned long))] &                       \
    (1UL << ((fd) % (8 * sizeof(unsigned long))))) != 0)

/* select/pselect wrappers (stub — poll-based fallback) */
LIBC_EXPORT int select(int nfds, fd_set *readfds, fd_set *writefds,
                       fd_set *exceptfds, struct timeval *timeout);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SELECT_H */
