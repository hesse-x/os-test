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

// Assembly entry points
void switch_to(xtask_t *prev, xtask_t *next);
void process_entry(void);

// Internal helpers
int pick_cpu(void);
uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip);

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
