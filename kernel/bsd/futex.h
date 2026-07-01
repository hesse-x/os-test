#ifndef KERNEL_BSD_FUTEX_H
#define KERNEL_BSD_FUTEX_H

#include <stdint.h>
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/list.h"

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE 64  // (1 << FUTEX_HASH_BITS)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

struct futex_bucket {
    list_node_t waiters;    // 等待线程链表（proc_t->futex_node 挂载）
    spinlock_t  lock;
};

extern struct futex_bucket futex_table[FUTEX_HASH_SIZE];

int64_t sys_futex(int64_t arg1, int64_t arg2, int64_t arg3,
                  int64_t arg4, int64_t arg5, int64_t arg6);

#endif // KERNEL_BSD_FUTEX_H
