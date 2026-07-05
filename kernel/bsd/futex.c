// kernel/bsd/futex.c — Futex implementation (anon key: cr3 + page_off)
// 阶段 3b→C5: FUTEX_WAIT / FUTEX_WAKE + timeout + EINTR + bucket lock irqsave

#include "kernel/bsd/futex.h"
#include "kernel/bsd/proc.h"
#include "kernel/xcore/xtask.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "arch/x64/smp.h"
#include <xos/errno.h>
#include <xos/time.h>

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
    (void)arg5;(void)arg6;
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
        // 收集 waiter 后释放 bucket lock 再唤醒：wake_with_event 会取 target 的
        // scheduler_lock，持 bucket lock 唤醒会形成 bucket→scheduler 锁序嵌套。
        //
        // 注意：曾在栈上分配 xtask_t *to_wake[MAX_PROC]（8KB）收集 waiter，但
        // 内核栈仅 2 页（8KB），该数组直接占满整栈并向下溢出，踩坏栈下方相邻的
        // slab 对象（典型现象：sys_exit 的 clear_tid futex_wake 后 signal->parent_pid
        // 变成栈残留垃圾，do_exit 访问 parent->proc->sig_pending 触发 #PF）。
        // 改为分批：每批用小固定数组收集 ≤32 个并唤醒，循环至 wake 满 val 个
        // 或 bucket 无更多匹配 waiter。futex wake 语义是"最多唤醒 val 个"，分批
        // 等价。val 来自用户态不可信，分批同时规避了大 val 的栈/堆开销。
        #define FUTEX_WAKE_BATCH 32
        int total_woken = 0;
        while (total_woken < (int)val) {
            xtask_t *to_wake[FUTEX_WAKE_BATCH];
            int nwake = 0;
            uint64_t bflags;
            spin_lock_irqsave(&bucket->lock, &bflags);
            list_node_t *node = bucket->waiters.next;
            int batch = (int)val - total_woken;
            if (batch > FUTEX_WAKE_BATCH) batch = FUTEX_WAKE_BATCH;
            while (node != &bucket->waiters && nwake < batch) {
                proc_t *p = LIST_ENTRY(node, proc_t, futex_node);
                node = node->next;
                if (p->futex_uaddr == uaddr) {
                    to_wake[nwake++] = p->xtask;
                    list_remove(&p->futex_node);
                    p->futex_uaddr = 0;
                }
            }
            spin_unlock_irqrestore(&bucket->lock, bflags);
            for (int i = 0; i < nwake; i++) {
                wake_with_event(to_wake[i], WAIT_FUTEX);
            }
            total_woken += nwake;
            if (nwake < batch) break;  // bucket 已无更多匹配 waiter
        }
        printk(LOG_DEBUG, "futex WAKE: pid=%d uaddr=%p val=%d nwake=%d\n",
               (int)cur->pid, (void *)uaddr, (int)val, total_woken);
        return (int64_t)total_woken;
        #undef FUTEX_WAKE_BATCH
    }

    // FUTEX_WAIT
    // 1. 持锁验值（防 lost wake-up）+ 入队
    uint64_t bflags;
    spin_lock_irqsave(&bucket->lock, &bflags);
    uint32_t cur_val;
    if (copy_from_user(&cur_val, (void __user *)uaddr, 4) != 0) {
        spin_unlock_irqrestore(&bucket->lock, bflags);
        return (int64_t)-EFAULT;
    }
    if (cur_val != val) {
        spin_unlock_irqrestore(&bucket->lock, bflags);
        printk(LOG_DEBUG, "futex WAIT EAGAIN: pid=%d uaddr=%p val=%d cur=%d\n",
               (int)cur->pid, (void *)uaddr, (int)val, (int)cur_val);
        return (int64_t)-EAGAIN;
    }
    cur->proc->futex_uaddr = uaddr;
    list_push_back(&bucket->waiters, &cur->proc->futex_node);
    printk(LOG_DEBUG, "futex WAIT: pid=%d uaddr=%p val=%d\n",
           (int)cur->pid, (void *)uaddr, (int)val);

    // timeout: arg4 = absolute abstime ns (struct timespec * passed as int64_t)
    int64_t abstime_ns = 0;
    int has_timeout = 0;
    if (arg4 != 0) {
        struct timespec ts;
        if (copy_from_user(&ts, (void __user *)arg4, sizeof(ts)) != 0) {
            list_remove(&cur->proc->futex_node);
            cur->proc->futex_uaddr = 0;
            spin_unlock_irqrestore(&bucket->lock, bflags);
            return (int64_t)-EFAULT;
        }
        abstime_ns = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
        has_timeout = 1;
    }

    // 2. 设 BLOCKED（持 bucket lock 防 lost-wakeup 窗口）
    int cpu = cur->assigned_cpu;
    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
    cur->wait_timed_out = 0;
    if (has_timeout) {
        cur->wait_deadline = abstime_ns;  // absolute abstime
        timer_queue_insert(cpu, cur);
    }
    cur->wait_event = WAIT_FUTEX;
    cur->state = BLOCKED;
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
    spin_unlock_irqrestore(&bucket->lock, bflags);
    schedule();

    // 3. 唤醒后清理 + 返回值判定
    int64_t ret_val = 0;
    if (signal_pending_hook && signal_pending_hook(cur)) ret_val = (int64_t)-EINTR;
    else if (cur->wait_timed_out) ret_val = (int64_t)-ETIMEDOUT;

    spin_lock_irqsave(&bucket->lock, &bflags);
    if (cur->proc->futex_uaddr) {
        list_remove(&cur->proc->futex_node);
        cur->proc->futex_uaddr = 0;
    }
    spin_unlock_irqrestore(&bucket->lock, bflags);
    return ret_val;
}
