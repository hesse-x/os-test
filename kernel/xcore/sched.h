#ifndef KERNEL_XCORE_SCHED_H
#define KERNEL_XCORE_SCHED_H

#include <stdint.h>
#include "kernel/xcore/xtask.h"

// Scheduler and process table (kernel/xcore/sched.c)

xtask_t *xtask_alloc(void);
void xtask_free(xtask_t *t);
void proc_init(void);
void schedule(void);
void task_reap(xtask_t *proc);
xtask_t *create_idle_process(int cpu_id);
void idle_entry(void);
void try_steal_task(void);
void timer_queue_insert(int cpu, xtask_t *proc);
void timer_queue_remove(xtask_t *proc);

static inline void timer_queue_cancel(xtask_t *proc) {
    if (proc->wait_deadline != 0) {
        timer_queue_remove(proc);
        proc->wait_deadline = 0;
    }
}

static inline void wake_from_wait(xtask_t *p) {
    timer_queue_cancel(p);
    p->state = READY;
    p->wait_event = WAIT_NONE;
    p->wait_timed_out = 0;
    int cpu = p->assigned_cpu;
    list_push_back(&cpu_locals[cpu].run_queue, &p->run_node);
    cpu_locals[cpu].run_count++;
}

// wake_with_event: 持 target 的 scheduler_lock，仅当 target 处于 BLOCKED 且
// wait_event == expected_event 时唤醒。event 不匹配则 no-op（lost wake-up 安全：
// target 已被其它路径唤醒或不在该类等待）。在 bucket lock / socket_lock 外调用。
// 收敛 futex.c / ipc.c 的"持 scheduler_lock + check + wake_from_wait"重复写法。
static inline void wake_with_event(xtask_t *target, wait_event_t expected_event) {
    int tcpu = target->assigned_cpu;
    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &flags);
    if (target->state == BLOCKED && target->wait_event == expected_event) {
        wake_from_wait(target);
    }
    spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, flags);
}

// wake_process_any: 中断任意阻塞态（用于 signal 投递——signal 应能打断
// WAIT_FUTEX/WAIT_CHILD/WAIT_PIPE 等任意等待）。与 wake_process 的区别：
// wake_process 窄语义只处理 IPC 类等待（PIPE/POLL/RECV），命中其它 event 即 ASSERT；
// wake_process_any 不区分 event，无条件唤醒 BLOCKED 状态的 target。
static inline void wake_process_any(xtask_t *target) {
    int tcpu = target->assigned_cpu;
    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &flags);
    if (target->state == BLOCKED) {
        wake_from_wait(target);
    }
    spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, flags);
}

// Assembly entry points
void switch_to(xtask_t *prev, xtask_t *next);
void process_entry(void);

// Internal helpers
#define AFFINITY_THRESHOLD 1   // 亲和性阈值:偏好 CPU 负载与最小值差距 <= 此值时选偏好 CPU
#define RECHECK_THRESHOLD 2   // TOCTOU 重检阈值:队列已有 > 此值个等待任务时考虑换 CPU
int pick_cpu(void);
int pick_cpu_pref(int pref_cpu);
uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip);
// Variant allowing caller-supplied user rsp (e.g. for argc/argv/auxv stack layout)
uint64_t build_kstack_user_rsp(uint64_t k_stack_top, uint64_t entry_rip, uint64_t user_rsp);

// ===================== FPU state save/restore =====================
// fxsave/fxrstor 本身是 FPU 指令，在 CR0.TS=1 时会触发 #NM。eager 模式下 TS 恒为 0，
// clts 是冗余的 no-op，但保留作防御（若 TS 被意外设置，fxsave/fxrstor 会嵌套 #NM → #DF）。
// 这两个 helper 是唯一允许操作 FPU 状态的入口，从结构上杜绝"忘记 clts"。
static inline void kernel_fpu_save(void *buf) {
    __asm__ volatile("clts\n\t"
                     "fxsave (%0)"
                     :: "r"(buf) : "memory");
}

static inline void kernel_fpu_restore(void *buf) {
    __asm__ volatile("clts\n\t"
                     "fxrstor (%0)"
                     :: "r"(buf) : "memory");
}

void fpu_lazy_switch(xtask_t *t);
void fpu_context_switch(xtask_t *prev, xtask_t *next);

// 分配 FPU 状态页并初始化为合法 fxsave 镜像（memset 0 + MXCSR=0x1F80）。
// 返回 1 成功 / 0 失败（bfc_alloc_page 失败）。调用者负责失败回滚。
int xcore_fpu_alloc(xtask_t *t);

#endif // KERNEL_XCORE_SCHED_H
