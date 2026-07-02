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
        // 卡死定位辅助：默认 LOG_DEBUG 不输出，构建时开 LOG_LEVEL_DEBUG 才打。
        // 卡死时看 log 是"只有 WAIT 没 WAKE"（wakeup 路径丢）还是"WAKE 了但 nwake=0"
        //（waiter 没入队 / uaddr 不匹配）。纯观测点，不改唤醒语义。
        printk(LOG_DEBUG, "futex WAKE: pid=%d uaddr=%p val=%d nwake=%d\n",
               (int)cur->pid, (void *)uaddr, (int)val, nwake);
        // 2. 释放后唤醒（bucket lock 外调用，防锁序逆序）。
        //    用 wake_with_event(t, WAIT_FUTEX) 收敛"持 scheduler_lock + check + wake"
        //    语义（见 kernel/xcore/sched.h），不再 open-code。
        for (int i = 0; i < nwake; i++) {
            wake_with_event(to_wake[i], WAIT_FUTEX);
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
        // 卡死定位辅助：val 不匹配说明 waker 已改值，waiter 直接 EAGAIN 返回。
        // 若卡死时只看到 WAIT 没有对应 WAKE 且这条 EAGAIN 频繁出现，说明
        // waiter 和 waker 对 *uaddr 的观测不一致（典型：缺少 memory barrier）。
        printk(LOG_DEBUG, "futex WAIT EAGAIN: pid=%d uaddr=%p val=%d cur=%d\n",
               (int)cur->pid, (void *)uaddr, (int)val, (int)cur_val);
        return (int64_t)-EAGAIN;
    }
    cur->proc->futex_uaddr = uaddr;
    list_push_back(&bucket->waiters, &cur->proc->futex_node);
    spin_unlock(&bucket->lock);
    printk(LOG_DEBUG, "futex WAIT: pid=%d uaddr=%p val=%d\n",
           (int)cur->pid, (void *)uaddr, (int)val);

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
