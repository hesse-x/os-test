/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// getrandom/getentropy/arc4random_buf/arc4random_uniform 封装
//
// 内核 SYS_GETRANDOM 语义对齐 Linux getrandom(2)：永不阻塞、
// 信号短读以返回字节数形式体现（不返回 EAGAIN/EINTR）。

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <xos/errno.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
  int64_t r = __syscall3(SYS_GETRANDOM, (int64_t)(uintptr_t)buf,
                         (int64_t)buflen, (int64_t)flags);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (ssize_t)r;
}

int getentropy(void *buf, size_t buflen) {
  if (buflen > 256) {
    errno = EIO;
    return -1;
  }
  // 短读重试直到填满
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
      // 校验失败属编程错误（内核不失败路径下不应到达）：填 0 防御并返回
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
  // 拒绝采样消除模偏置：min = 2^32 mod upper_bound
  uint32_t min = (uint32_t)(-upper_bound) % upper_bound;
  for (;;) {
    uint32_t r;
    arc4random_buf(&r, sizeof(r));
    if (r >= min)
      return r % upper_bound;
  }
}
