/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_SIGNALFD_H
#define KERNEL_BSD_SIGNALFD_H

#include <stdint.h>

#include "kernel/xcore/spinlock.h"

#define SFD_CLOEXEC 0x8000
#define SFD_NONBLOCK 0x800

typedef struct signalfd_ctx {
  uint64_t sigmask; // signals this fd accepts (sigset_t = uint64_t)
  spinlock lock;
} signalfd_ctx;

// signalfd_siginfo: 128 bytes (Linux UAPI layout). Only the fields this kernel
// can populate are meaningful; the trailing pad is zeroed.
typedef struct signalfd_siginfo {
  uint32_t ssi_signo;
  int32_t ssi_errno;
  int32_t ssi_code;
  uint32_t ssi_pid;
  uint32_t ssi_uid;
  uint8_t _pad[112]; // pad to 128 bytes total
} signalfd_siginfo;

struct file;
struct proc;

// Pending signals of interest to this signalfd (sig_pending | shared_pending),
// masked by the fd's sigmask. Read-only query, no locking.
uint64_t proc_signalfd_pending(signalfd_ctx *sfd);

// Does any open signalfd in this process accept and consume `signo` (i.e. the
// signal is in the fd's mask AND not blocked by the process)? If so, the
// normal handler-delivery path is bypassed and the signal is left pending for
// the signalfd reader.
int signalfd_consumes(struct proc *bp, int signo);

// Return the wait_queue_head of the first signalfd accepting `signo`, or NULL.
// Caller must hold rcu_read_lock across the call and the use of the
// returned wq (the file can otherwise be closed/freed concurrently by a
// sibling thread sharing files).
struct wait_queue_head *signalfd_wq(struct proc *bp, int signo);

int64_t sys_signalfd4(int64_t fd, int64_t sigmask_ptr, int64_t sizemask,
                      int64_t flags);
int64_t signalfd_do_read(struct file *f, void *buf);

#endif // KERNEL_BSD_SIGNALFD_H
