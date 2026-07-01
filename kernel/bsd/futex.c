// kernel/bsd/futex.c — Futex implementation (anon key: cr3 + page_off)
// 阶段 3b：FUTEX_WAIT / FUTEX_WAKE only (no timeout, no private/shared flag).

#include "kernel/bsd/futex.h"
#include "kernel/bsd/proc.h"
#include "kernel/xcore/xtask.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "arch/x64/smp.h"
#include "common/errno.h"

struct futex_bucket futex_table[FUTEX_HASH_SIZE];

struct futex_key {
    uint32_t type;       // 0=anon
    uint64_t cr3;
    uint64_t page_off;
};

static uint32_t futex_hash(struct futex_key *key) {
    uint64_t h = key->cr3 ^ key->page_off;
    return (uint32_t)((h >> 3) & (FUTEX_HASH_SIZE - 1));
}

static void get_futex_key(uint64_t uaddr, mm_t *mm, struct futex_key *key) {
    key->type = 0;
    key->cr3 = mm->cr3;
    key->page_off = uaddr >> 12;  // PAGE_SHIFT=12
}

int64_t sys_futex(int64_t arg1, int64_t arg2, int64_t arg3,
                  int64_t arg4, int64_t arg5, int64_t arg6) {
    (void)arg4;(void)arg5;(void)arg6;
    uint64_t uaddr = (uint64_t)arg1;
    int op = (int)arg2;
    uint32_t val = (uint32_t)arg3;
    xtask_t *cur = current_task;

    // 只支持 FUTEX_WAIT / FUTEX_WAKE
    int real_op = op & 0x7f;
    if (real_op != FUTEX_WAIT && real_op != FUTEX_WAKE) return (int64_t)-ENOSYS;

    struct futex_key key;
    get_futex_key(uaddr, cur->mm, &key);
    struct futex_bucket *bucket = &futex_table[futex_hash(&key)];

    if (real_op == FUTEX_WAKE) {
        // 1. 持 bucket lock 收集 waiter
        xtask_t *to_wake[MAX_PROC];
        int nwake = 0;
        spin_lock(&bucket->lock);
        list_node_t *node = bucket->waiters.next;
        while (node != &bucket->waiters && nwake < (int)val) {
            proc_t *p = LIST_ENTRY(node, proc_t, futex_node);
            node = node->next;
            if (p->futex_uaddr == uaddr) {
                to_wake[nwake++] = p->xtask;
                list_remove(&p->futex_node);
                p->futex_uaddr = 0;
            }
        }
        spin_unlock(&bucket->lock);
        // 2. 释放后唤醒（bucket lock 外调用 wake_process，防锁序逆序）
        for (int i = 0; i < nwake; i++) {
            wake_process(to_wake[i]->pid);
        }
        return (int64_t)nwake;
    }

    // FUTEX_WAIT
    // 1. 持锁验值（防 lost wake-up）
    spin_lock(&bucket->lock);
    uint32_t cur_val;
    if (copy_from_user(&cur_val, (void __user *)uaddr, 4) != 0) {
        spin_unlock(&bucket->lock);
        return (int64_t)-EFAULT;
    }
    if (cur_val != val) {
        spin_unlock(&bucket->lock);
        return (int64_t)-EAGAIN;
    }
    cur->proc->futex_uaddr = uaddr;
    list_push_back(&bucket->waiters, &cur->proc->futex_node);
    spin_unlock(&bucket->lock);

    // 2. 设 BLOCKED + WAIT_FUTEX + schedule
    int cpu = cur->assigned_cpu;
    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
    cur->wait_event = WAIT_FUTEX;
    cur->state = BLOCKED;
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
    schedule();

    // 3. 唤醒后清理（若 FUTEX_WAKE 未摘除节点，自摘）
    spin_lock(&bucket->lock);
    if (cur->proc->futex_uaddr) {
        list_remove(&cur->proc->futex_node);
        cur->proc->futex_uaddr = 0;
    }
    spin_unlock(&bucket->lock);
    return 0;
}
