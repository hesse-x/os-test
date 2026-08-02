/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// arc4random_* (BSD extensions) — musl does not ship these, so they stay here
// (todo.md). getentropy is now ADOPTED from musl src/misc/getentropy.c
// (musl_misc_objs): its deps (getrandom from musl_linux_objs,
// pthread_setcancelstate from musl_pthread) are satisfied, and it is logic-
// equivalent to the repo version (256-byte cap + getrandom loop, plus a
// cancel-state guard). getrandom itself comes from musl src/linux/getrandom.c
// (musl_linux_objs). The repo's old getentropy here is deleted.

#include <stdint.h>
#include <string.h>

#include <sys/random.h>
#include <sys/types.h>

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
