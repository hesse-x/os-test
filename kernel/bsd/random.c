/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/random.c — sys_getrandom + /dev/random + /dev/urandom
//
// All three external paths share the Xcore-layer csprng_read() backend; there
// is no second implementation.
// Semantics match Linux getrandom(2):
//   - Never blocks (Linux 5.6+ behavior); never returns EAGAIN/EINTR
//   - The three flag values (GRND_NONBLOCK/GRND_RANDOM/GRND_INSECURE) are
//     synonymous: single pool
//   - Single-call limit 32MiB-1 (Linux urandom limit); beyond that, short read
//   - Not interrupted by signals: the pool is always ready (csprng_read is
//     synchronous), loops until done==len; Linux getrandom(2) also never
//     returns a short read once the pool is ready. Only on copy_to_user failure
//     does it return a short read (EFAULT semantics, same as Linux).

#include "kernel/bsd/random.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/poll_types.h"
#include "kernel/bsd/syscall.h"
#include "kernel/xcore/random.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/xtask.h"

#include <xos/epoll.h>
#include <xos/errno.h>

// copy_to_user has no dedicated header; forward-declare.
size_t copy_to_user(void *dst, const void *src, size_t size);

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM 0x0002
#define GRND_INSECURE 0x0004
#define GRND_VALID_MASK (GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)

#define GETRANDOM_MAX 33554431 // 32MiB-1, Linux urandom single-call limit
#define RANDOM_CHUNK 256       // block ≤256B, keeping Linux ≤256B atomicity

// Core: loop csprng_read to fetch small kernel chunks → copy_to_user append;
// never interrupted by signals
static int64_t random_read_common(void __user *ubuf, size_t len) {
  uint8_t chunk[RANDOM_CHUNK];
  size_t done = 0;
  while (done < len) {
    size_t n = len - done < RANDOM_CHUNK ? len - done : RANDOM_CHUNK;
    csprng_read(chunk, n);
    if (copy_to_user((void __force *)(uint8_t __user *)ubuf + done, chunk, n)) {
      if (done == 0)
        return -EFAULT;
      break; // copied part returns as a short read (EFAULT semantics,
             // Linux-like)
    }
    done += n;
  }
  return (int64_t)done;
}

int64_t sys_getrandom(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                      int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  void __user *buf = (void __user *__force)arg1;
  size_t buflen = (size_t)arg2;
  unsigned int flags = (unsigned int)arg3;

  if (flags & ~GRND_VALID_MASK)
    return -EINVAL;
  if (buflen == 0)
    return 0;
  if (!buf)
    return -EFAULT;
  if (buflen > GETRANDOM_MAX)
    buflen = GETRANDOM_MAX;
  return random_read_common(buf, buflen);
}

// ===================== /dev/random + /dev/urandom =====================

static ssize_t random_dev_read(xtask *proc, int fd, void *buf, size_t count) {
  (void)proc;
  (void)fd;
  if (count == 0)
    return 0;
  if (count > GETRANDOM_MAX)
    count = GETRANDOM_MAX;
  return (ssize_t)random_read_common((void __user *__force)buf, count);
}

// Entropy injection is not done (no mixing-pool write path): accept and
// discard, return count
static ssize_t random_dev_write(xtask *proc, int fd, const void *buf,
                                size_t count) {
  (void)proc;
  (void)fd;
  (void)buf;
  return (ssize_t)count;
}

static __poll random_dev_poll(xtask *proc, int events) {
  (void)proc;
  (void)events;
  return EPOLLIN | EPOLLRDNORM;
}

static struct dev_ops random_ops = {
    .driver_pid = 0, // kernel device
    .is_block = false,
    .subsystem = "misc",
    .devtype = "random",
    .read = random_dev_read,
    .write = random_dev_write,
    .poll = random_dev_poll,
};

void random_dev_init(void) {
  devtmpfs_create("random", &random_ops, NULL);
  devtmpfs_create("urandom", &random_ops, NULL); // same ops, synonyms
}
