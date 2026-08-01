/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// getentropy + arc4random_* (BSD extensions) — getrandom itself comes from
// musl src/linux/getrandom.c (musl_linux_objs), so it is no longer defined
// here. arc4random_buf/arc4random_uniform are BSD APIs musl does not ship;
// they stay (todo.md:349). getentropy is also kept here rather than pulling
// musl src/misc/getentropy.c into a new module: it is a trivial getrandom
// loop with a 256-byte cap, and getrandom now resolves from the musl object
// in the same archive at link time.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <sys/random.h>
#include <sys/types.h>

int getentropy(void *buf, size_t buflen) {
  if (buflen > 256) {
    errno = EIO;
    return -1;
  }
  // Retry on short reads until the buffer is full.
  size_t done = 0;
  while (done < buflen) {
    ssize_t n = getrandom((char *)buf + done, buflen - done, 0);
    if (n < 0)
      return -1;
    done += (size_t)n;
  }
  return 0;
}

void arc4random_buf(void *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = getrandom((char *)buf + done, n - done, 0);
    if (r <= 0) {
      // Verification failure is a programming error (unreachable on kernel
      // non-failure paths): defensively zero-fill and return.
      memset((char *)buf + done, 0, n - done);
      return;
    }
    done += (size_t)r;
  }
}

uint32_t arc4random_uniform(uint32_t upper_bound) {
  if (upper_bound == 0) {
    uint32_t r;
    arc4random_buf(&r, sizeof(r));
    return r;
  }
  // Rejection sampling to eliminate modulo bias: min = 2^32 mod upper_bound
  uint32_t min = (uint32_t)(-upper_bound) % upper_bound;
  for (;;) {
    uint32_t r;
    arc4random_buf(&r, sizeof(r));
    if (r >= min)
      return r % upper_bound;
  }
}
