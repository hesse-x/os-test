/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_SIGNALFD_H
#define _SYS_SIGNALFD_H

#include <signal.h>
#include <stdint.h>
#include <sys/cdefs.h>

#define SFD_CLOEXEC 0x8000
#define SFD_NONBLOCK 0x800

typedef struct {
  uint32_t ssi_signo;
  int32_t ssi_errno;
  int32_t ssi_code;
  uint32_t ssi_pid;
  uint32_t ssi_uid;
  uint8_t _pad[112]; // total 128 bytes (Linux UAPI)
} signalfd_siginfo;

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int signalfd(int fd, const sigset_t *mask, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SIGNALFD_H */
