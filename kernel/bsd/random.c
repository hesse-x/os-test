/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/random.c — sys_getrandom + /dev/random + /dev/urandom
//
// 三条对外路径共用 Xcore 层 csprng_read() 后端，无第二实现。
// 语义对齐 Linux getrandom(2)：
//   - 永不阻塞（Linux 5.6+ 行为），不返回 EAGAIN/EINTR
//   - flags 三值（GRND_NONBLOCK/GRND_RANDOM/GRND_INSECURE）语义同义：单池
//   - 单次上限 32MiB-1（Linux urandom 上限），超出短读
//   - 不被信号中断：池恒就绪（csprng_read 同步），循环到 done==len；
//     Linux getrandom(2) 在池就绪后亦不返回短读。仅 copy_to_user 失败时
//     以短读形式返回（EFAULT 语义，Linux 同款）。

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

#define GETRANDOM_MAX 33554431 // 32MiB-1，Linux urandom 单次上限
#define RANDOM_CHUNK 256 // 单块 ≤256B，保持 Linux ≤256B 原子性保证

// 核心：循环 csprng_read 取内核小块 → copy_to_user 追加；不被信号中断
static int64_t random_read_common(void __user *ubuf, size_t len) {
  uint8_t chunk[RANDOM_CHUNK];
  size_t done = 0;
  while (done < len) {
    size_t n = len - done < RANDOM_CHUNK ? len - done : RANDOM_CHUNK;
    csprng_read(chunk, n);
    if (copy_to_user((void __force *)(uint8_t __user *)ubuf + done, chunk, n)) {
      if (done == 0)
        return -EFAULT;
      break; // 已拷部分以短读形式返回（EFAULT 语义，Linux 同款）
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

// 熵注入不做（无混合池写入路径）：接收并丢弃，返回 count
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
    .driver_pid = 0, // 内核设备
    .is_block = false,
    .subsystem = "misc",
    .devtype = "random",
    .read = random_dev_read,
    .write = random_dev_write,
    .poll = random_dev_poll,
};

void random_dev_init(void) {
  devtmpfs_create("random", &random_ops, NULL);
  devtmpfs_create("urandom", &random_ops, NULL); // 同一 ops，同义
}
