// kernel/xcore/sched.c — Scheduler and process table management
// Extracted from kernel/proc.c (phase 4 step 4.1)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kernel/xcore/xtask.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mem/kasan.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "common/macro.h"
#include "common/errno.h"
#include "common/shm.h"
#include "common/signal.h"

// Validate assembly offset assumptions in trapentry.S (switch_to uses hardcoded offsets)
_Static_assert(offsetof(xtask_t, k_rsp) == 8,  "switch_to asm: k_rsp offset mismatch");
_Static_assert(offsetof(xtask_t, cr3)   == 24, "switch_to asm: cr3 offset mismatch");
_Static_assert(sizeof(trapframe_t)   == 176, "trapframe size must be 176 (22 × uint64_t)");
_Static_assert(offsetof(cpu_local_t, tss_rsp0) == 48,
               "syscall_fast_entry asm: tss_rsp0 offset mismatch (expected 48)");

// fs_base offset consumed by trapentry.S (wrmsr FS_BASE before returning to
// user mode). Pinned by static_assert at the bottom of this file.
const uint64_t xtask_fs_base_offset = offsetof(xtask_t, fs_base);

xtask_t tasks[MAX_PROC];
// current_task is per-CPU (in cpu_local_t), accessed via macro

spinlock_t tasks_lock = SPINLOCK_INIT;
pid_t init_pid = -1;

// ===================== Process table =====================

// Allocate a free slot from tasks[] under tasks_lock.
// Returns pointer to the free xtask_t, or NULL if no free slot.
// Caller must hold tasks_lock; on success the slot's pid is still -1
// (caller sets it after allocating resources).
//
// reclaim_lazy_resources: 释放 task_reap 延迟处理的资源。
//
// 延迟释放清单（SMP 竞态防护）——这些资源在 schedule() 的切换路径中被引用，
// task_reap 与 schedule() 无共享锁，立即释放会 UAF。留到 slot 复用时此处统一释放：
//   - k_stack (2 pages)  : switch_to 汇编在子进程栈上执行 ret
//   - fpu_page (1 page)  : fpu_context_switch 在 prev 分支 fxsave 该页
// slot 复用意味着子进程早已切走，安全。新增同类资源（如扩展 FPU 状态页）请加入此函数。
static void reclaim_lazy_resources(xtask_t *t) {
    if (t->k_stack_top != 0) {
        uint64_t k_stack_phys_base = (__force uint64_t)PHY_ADDR(
            t->k_stack_top - 2 * PAGE_SIZE);
        Page *stack_page = &bfc_frames[PHY_TO_PAGE(k_stack_phys_base)];
        bfc_free_page(stack_page, 2);
        t->k_stack_top = 0;
    }
    if (t->fpu_page) {
        bfc_free_page(t->fpu_page, 1);
        t->fpu_page = NULL;
    }
}

xtask_t *xtask_alloc(void) {
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].pid < 0) {
            xtask_t *t = &tasks[i];
            reclaim_lazy_resources(t);
            return t;
        }
    }
    return NULL;
}

void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        tasks[i].pid = -1;
        tasks[i].state = UNUSED;
        tasks[i].k_rsp = 0;
        tasks[i].k_stack_top = 0;
        tasks[i].cr3 = 0;
        tasks[i].entry = 0;
        tasks[i].wait_event = WAIT_NONE;
        tasks[i].tgid = -1;
        tasks[i].mm = NULL;
        tasks[i].assigned_cpu = -1;
        tasks[i].iopm = NULL;
        tasks[i].proc = NULL;  // NULL = no POSIX semantics
        tasks[i].wait_deadline = 0;
        tasks[i].wait_timed_out = 0;
        tasks[i].recv_intr = 0;
        list_init(&tasks[i].run_node);
        list_init(&tasks[i].wait_node);
        // recv queue
        tasks[i].recv_head = 0;
        tasks[i].recv_tail = 0;
        tasks[i].recv_lock = SPINLOCK_INIT;
        // REQ state
        tasks[i].req_caller_pid = -1;
        tasks[i].req_reply_buf = NULL;
        tasks[i].req_result = 0;
        tasks[i].req_target_pid = -1;
        // MSG state
        tasks[i].msg_reply_buf = NULL;
        tasks[i].msg_reply_len = 0;
        tasks[i].msg_caller_pid = -1;
        tasks[i].msg_result = 0;
        tasks[i].msg_target_pid = -1;
        tasks[i].cpu_time_ns = 0;
        tasks[i].last_sched = 0;
        tasks[i].exit_code = 0;
        tasks[i].detached = 0;
        tasks[i].tls_page = 0;
        tasks[i].tls_total = 0;
        tasks[i].user_stack_base = 0;
        tasks[i].user_stack_size = 0;
        // POSIX fields (sig, sid, pgid, ctty, exit_code) are in proc (NULL = no POSIX semantics)
    }
    cpu_locals[0]._cur_proc = NULL;
    cpu_locals[0].run_count = 0;
    cpu_locals[0].idle_proc = NULL;
    for (int c = 0; c < NUM_KMALLOC_CLASSES; c++) {
        cpu_locals[0].active_slab[c] = NULL;
    }
}

// switch_to restore frame: callee-saved registers + return address
typedef struct switch_frame_t {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ret_addr;
} switch_frame_t;

_Static_assert(sizeof(switch_frame_t) == 56,  "switch_frame size must be 56 (7 × uint64_t)");

uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip) {
    return build_kstack_user_rsp(k_stack_top, entry_rip, 0x00007FFFFFFFE000ULL);
}

uint64_t build_kstack_user_rsp(uint64_t k_stack_top, uint64_t entry_rip, uint64_t user_rsp) {
    trapframe_t tf = {0};
    tf.ss      = 0x23;                   // USER_DS
    if (user_rsp == 0) user_rsp = 0x00007FFFFFFFE000ULL;
    tf.rsp     = user_rsp;               // user stack pointer
    // 用户栈顶必须 16 字节对齐：iret 进入 _start 时 rsp%16==0，
    // _start 用 andq $-16 兜底后符合 ABI（call 后 rsp%16==8）。
    // 若此处不对齐，用户态 movaps/movdqa 会 #GP。
    ASSERT(tf.rsp % 16 == 0);
    tf.rflags  = 0x202;                  // IF=1, IOPL=0
    tf.cs      = 0x2B;                   // USER_CS
    tf.rip     = entry_rip;
    tf.err_code = 0;
    tf.trapno  = 0;

    switch_frame_t sf = {0};
    sf.ret_addr = (uint64_t)process_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(trapframe_t);
    __memcpy(sp, &tf, sizeof(trapframe_t));

    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));

    return (uint64_t)sp;
}

// Build idle kernel stack: only switch_frame (no trapframe), ret_addr = idle_entry
static uint64_t build_idle_kstack(uint64_t k_stack_top) {
    switch_frame_t sf = {0};
    sf.ret_addr = (uint64_t)idle_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));

    return (uint64_t)sp;
}

// Create idle process for the specified CPU
xtask_t *create_idle_process(int cpu_id) {
    spin_lock(&tasks_lock);
    xtask_t *proc = xtask_alloc();
    if (!proc) { spin_unlock(&tasks_lock); printk(LOG_ERROR, "create_idle_process: no free slot\n"); return NULL; }
    int alloc_idx = proc->pid >= 0 ? proc->pid : -1;  // pid not yet set, derive index from pointer
    alloc_idx = (int)(proc - tasks);

    // Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc_page(2);
    if (!stack_pages) { spin_unlock(&tasks_lock); printk(LOG_ERROR, "create_idle_process: alloc stack failed\n"); return NULL; }
    uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
    uint64_t k_stack_top = (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) + 2 * PAGE_SIZE;

    // Build idle switch_frame on kernel stack (no trapframe, no user mode)
    uint64_t k_rsp = build_idle_kstack(k_stack_top);

    // Fill PCB: idle uses kernel PML4, no user address space
    proc->pid = alloc_idx;
    proc->state = RUNNING;  // idle starts as RUNNING on its CPU
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = (__force uint64_t)PHY_ADDR((uintptr_t)pml4); // kernel PML4 physical address (cached)
    proc->entry = (uint64_t)idle_entry;
    proc->wait_event = WAIT_NONE;
    proc->tgid = proc->pid;
    proc->mm = NULL;  // idle has no address space
    proc->assigned_cpu = cpu_id;
    proc->iopm = NULL;
    proc->proc = NULL;  // will be created by proc_create for user processes
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    // POSIX fields are in proc (created separately)
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&tasks_lock);

    cpu_locals[cpu_id].idle_proc = proc;

    return proc;
}

__attribute__((no_sanitize("kernel-address")))
void idle_entry() {
    sti();
    while (1) {
        // Idle is a quiescent state — report RCU quiescence so
        // synchronize_rcu() doesn't spin forever waiting for this CPU.
        rcu_read_lock();
        rcu_read_unlock();

        // Reap zombie processes (BSD hook for POSIX cleanup)
        if (reap_hook) reap_hook();

        schedule();
        sti();
        __asm__ volatile("hlt");
    }
}

// Pick the CPU with the fewest runnable processes
int pick_cpu(void) {
    int best = 0;
    int min = __atomic_load_n(&cpu_locals[0].run_count, __ATOMIC_RELAXED);
    for (int i = 1; i < ncpu; i++) {
        int r = __atomic_load_n(&cpu_locals[i].run_count, __ATOMIC_RELAXED);
        if (r < min) {
            min = r;
            best = i;
        }
    }
    return best;
}

// Update TSS IOPM for the current CPU to match the given process
static void update_tss_iopm(xtask_t *proc) {
    int cpu = get_cpu_local()->cpu_id;
    tss_t *tss = &per_cpu_tss[cpu];
    if (proc->iopm) {
        __memcpy(tss->iopm, proc->iopm, IOPM_SIZE);
    } else {
        // Deny all ports
        for (int i = 0; i < IOPM_SIZE; i++)
            tss->iopm[i] = 0xFF;
    }
}

// FPU 上下文切换 C helper（eager 模式，不修改 switch_to 汇编）
// 在 switch_to(prev, next) 之前调用：fxsave prev 的 FPU 状态 + fxrstor next 的 FPU 状态。
// 不设 CR0.TS，用户态 SSE 指令直接执行不触发 #NM。idle 进程 fpu_page=NULL，自动跳过。
//
// UAF 防御：fpu_page 的释放是延迟的（见 task_reap 资源清单），但若有人错误地在
// task_reap 立即释放，schedule() 此处会 UAF。ASSERT page->status == PAGE_USED 可在
// DEBUG 构建下立即捕获——bfc_free_page 释放后 status 变 PAGE_FREE，fxsave 前暴露。
void fpu_context_switch(xtask_t *prev, xtask_t *next) {
    if (prev && prev->fpu_page) {
        ASSERT(prev->fpu_page->status == PAGE_USED);
        void *fpu_data = (void *)(__force uintptr_t)phys_to_virt(page_to_phys(prev->fpu_page));
        // 防御：fxsave 目标必须是 BFC 数据页虚拟地址，不能是 Page* 元数据指针
        // （历史 bug：误把 bfc_alloc_page 返回的 Page* 直接当数据指针，fxsave 覆盖
        //  Page 元数据，后续 kfree 检测 page->status 异常才暴露，定位困难）
        uint64_t vma_start __attribute__((unused)) = (__force uint64_t)phys_to_virt(0);
        ASSERT((uint64_t)fpu_data >= vma_start &&
               (uint64_t)fpu_data < vma_start + total_page_frames * PAGE_SIZE);
        kernel_fpu_save(fpu_data);  // 内部先 clts 再 fxsave
    }
    if (next && next->fpu_page) {
        ASSERT(next->fpu_page->status == PAGE_USED);
        void *fpu_data = (void *)(__force uintptr_t)phys_to_virt(page_to_phys(next->fpu_page));
        uint64_t vma_start __attribute__((unused)) = (__force uint64_t)phys_to_virt(0);
        ASSERT((uint64_t)fpu_data >= vma_start &&
               (uint64_t)fpu_data < vma_start + total_page_frames * PAGE_SIZE);
        kernel_fpu_restore(fpu_data);  // 内部先 clts 再 fxrstor
    }
}

// 分配 FPU 状态页并初始化为合法 fxsave 镜像。
// fxsave 512 字节布局中 MXCSR 位于 offset 24，必须为合法值（否则 fxrstor 触发 #GP）。
// memset 0 + 设 MXCSR=0x1F80（默认值：异常屏蔽位全 1，舍入=nearest）等价于 fninit+ldmxcsr 后的 init state。
// 纯内存操作，无 SSE 指令，不破坏调用者 xmm 寄存器。
int xcore_fpu_alloc(xtask_t *t) {
    t->fpu_page = bfc_alloc_page(1);
    if (!t->fpu_page) return 0;
    void *fpu_data = (void *)(__force uintptr_t)phys_to_virt(page_to_phys(t->fpu_page));
    __memset(fpu_data, 0, 512);
    *(uint32_t *)((uint8_t *)fpu_data + 24) = 0x1F80;  // MXCSR default
    return 1;
}

__attribute__((no_sanitize("kernel-address")))
void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    xtask_t *idle = get_cpu_local()->idle_proc;
    xtask_t *prev = current_task;

    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

    // Check if run_queue has a runnable process
    if (list_empty(&cpu_locals[my_cpu].run_queue)) {
        // If prev is BLOCKED, ZOMBIE, or REAPING, it cannot continue running —
        // switch to idle so the CPU halts until an IRQ wakes a process.
        if (prev != idle && (prev->state == BLOCKED || prev->state == ZOMBIE || prev->state == REAPING)) {
#ifndef NDEBUG
            if (prev->state == BLOCKED) {
                printk(LOG_DEBUG, "schedule: pid=%d BLOCKED wait_event=%d\n",
                       prev->pid, (int)prev->wait_event);
            }
#endif
            // Account prev's CPU time before switching to idle
            if (prev->last_sched != 0) {
                prev->cpu_time_ns += sched_clock() - prev->last_sched;
            }
            current_task = idle;
            per_cpu_tss[my_cpu].rsp0 = idle->k_stack_top;
            get_cpu_local()->tss_rsp0 = idle->k_stack_top;
            update_tss_iopm(idle);
            spin_unlock(&cpu_locals[my_cpu].scheduler_lock);
            ASSERT(get_cpu_local()->rcu.nesting == 0);  // must not schedule inside RCU read-side CS
            fpu_context_switch(prev, idle);
            switch_to(prev, idle);
            spin_lock(&cpu_locals[my_cpu].scheduler_lock);
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
        return; // no runnable process, prev continues
    }

    // Dequeue next process from head (FIFO round-robin)
    list_node_t *next_node = list_front(&cpu_locals[my_cpu].run_queue);
    xtask_t *next = LIST_ENTRY(next_node, xtask_t, run_node);
    list_remove(&next->run_node);

    // Account prev's CPU time before switching out
    if (prev != idle && prev->last_sched != 0) {
        prev->cpu_time_ns += sched_clock() - prev->last_sched;
    }

    // State transition for prev
    if (prev != idle && prev->state == RUNNING) {
        prev->state = READY;
        list_push_back(&cpu_locals[my_cpu].run_queue, &prev->run_node);
        cpu_locals[my_cpu].run_count++;
    }
    // if prev->state == BLOCKED, ZOMBIE, or REAPING: don't enqueue, run_count unchanged

    next->state = RUNNING;
    next->last_sched = sched_clock();
    cpu_locals[my_cpu].run_count--;
    current_task = next;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;
    update_tss_iopm(next);

    // Release lock but keep interrupts disabled — switch_to must run under cli
    // to prevent interrupt handlers from corrupting the stack during RSP/CR3 switch.
    // After switch_to returns (prev is resumed on prev's stack), re-acquire lock
    // WITHOUT saving flags (spin_lock, not spin_lock_irqsave) so the original
    // flags saved before the context switch remain intact. Then unlock+irqrestore
    // using those original flags to correctly restore the interrupt state.
    spin_unlock(&cpu_locals[my_cpu].scheduler_lock);
    ASSERT(get_cpu_local()->rcu.nesting == 0);  // must not schedule inside RCU read-side CS
    fpu_context_switch(prev, next);
    switch_to(prev, next);
    spin_lock(&cpu_locals[my_cpu].scheduler_lock);
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}

// ===================== Timer queue operations =====================
// Must be called under scheduler_lock of the target CPU

void timer_queue_insert(int cpu, xtask_t *proc) {
    // Remove from any existing list first to prevent duplicate insertion.
    // Skip if wait_node is self-referencing (never inserted into a list).
    if (!list_empty(&proc->wait_node))
        list_remove(&proc->wait_node);

    list_node_t *head = &cpu_locals[cpu].timer_queue;
    list_node_t *node = head->next;
    while (node != head) {
        xtask_t *p = LIST_ENTRY(node, xtask_t, wait_node);
        if (p->wait_deadline > proc->wait_deadline) break;
        node = node->next;
    }
    // Insert before node
    proc->wait_node.prev = node->prev;
    proc->wait_node.next = node;
    node->prev->next = &proc->wait_node;
    node->prev = &proc->wait_node;
}

void timer_queue_remove(xtask_t *proc) {
    list_remove(&proc->wait_node);
}

// task_reap: reclaim all resources of a process
// Called by sys_exit (no-parent path) or sys_waitpid
//
// 资源释放清单（注意 SMP 竞态）：
//   immediate（无 schedule() 路径引用）:
//     - iopm           : 仅 update_tss_iopm 在 sched_lock 下读
//     - mm (mm_put)    : refcount 管理，最后释放者负责
//     - recv queue     : 仅本进程访问
//   lazy（schedule() 切换路径仍引用，延迟到 xtask_alloc 复用 slot 时释放）:
//     - k_stack (2pg)  : switch_to 汇编在子进程栈上 ret
//     - fpu_page (1pg) : fpu_context_switch 在 prev 分支 fxsave 该页
//   由 BSD 层释放（proc_reap_hook）:
//     - fds/signal/proc_t
//
// 立即释放 lazy 类资源会与 schedule() 竞态 UAF（task_reap 与 schedule() 无共享锁）。
// 新增同类资源请归入对应类别，lazy 类资源统一在 xtask_alloc 的 reclaim_lazy_resources 释放。
void task_reap(xtask_t *proc) {
    ASSERT(proc->state == ZOMBIE || proc->state == REAPING);

    // 1. Free IOPM bitmap (immediate)
    if (proc->iopm) {
        kfree(proc->iopm);
        proc->iopm = NULL;
    }

    // 2b. Free FPU state page (lazy-allocated via bfc_alloc_page)
    //     延迟到 xtask_alloc 复用 slot 时释放（与内核栈同理，见函数顶部清单）：
    //     子进程在 schedule() 的 fpu_context_switch 中可能仍持有 fpu_page 引用，
    //     此处立即释放会与 schedule() 竞态 UAF。slot 复用时子进程早已切走。
    //     if (proc->fpu_page) {
    //     bfc_free_page(proc->fpu_page, 1);
    //     proc->fpu_page = NULL;
    // }

    // 2c. Detached thread: unmap user TLS + stack BEFORE mm_put
    //     (after mm_put, pml4 may be freed by mm_release → UAF)
    if (proc->detached && proc->mm) {
        uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);
        if (proc->tls_page && proc->tls_total) {
            size_t npages = proc->tls_total / PAGE_SIZE;
            for (size_t i = 0; i < npages; i++) {
                unmap_user_pages(pml4, proc->tls_page + i * PAGE_SIZE,
                                 proc->tls_page + (i + 1) * PAGE_SIZE, 1);
            }
        }
        if (proc->user_stack_base && proc->user_stack_size) {
            size_t npages = proc->user_stack_size / PAGE_SIZE;
            for (size_t i = 0; i < npages; i++) {
                unmap_user_pages(pml4, proc->user_stack_base + i * PAGE_SIZE,
                                 proc->user_stack_base + (i + 1) * PAGE_SIZE, 1);
            }
        }
    }

    // 3. mm_put (decrement triggers mm_release when ref_count hits 0)
    //    mm_release will: free user pages+PML4+mmap+SHM+devtmpfs+irq_owner+wake waiters
    if (proc->mm) {
        pid_t pid_for_cleanup = proc->pid;
        mm_t *mm = proc->mm;
        proc->mm = NULL;
        if (refcount_dec_and_test(&mm->m_count)) {
            mm_release(mm, pid_for_cleanup);
        }
    }

    // 4. Free any RECV_MSG entries in recv queue (kfree their kmaddr)
    spin_lock(&proc->recv_lock);
    uint32_t idx = proc->recv_tail;
    while (idx != proc->recv_head) {
        recv_msg_t *m = (recv_msg_t *)proc->recv_buf[idx];
        if (m->type == RECV_MSG && m->msg.kmaddr) {
            kfree(m->msg.kmaddr);
            m->msg.kmaddr = NULL;
        }
        idx = (idx + 1) % RECV_QUEUE_SIZE;
    }
    spin_unlock(&proc->recv_lock);

    // 5. Clear MSG caller state (server died before responding)
    proc->msg_caller_pid = -1;

    // 6. POSIX cleanup: close fds, signal_put, free proc
    if (proc_reap_hook) proc_reap_hook(proc);

    // 7. Clear PCB slot — EXCEPT lazy-reclaim resources (k_stack_top, fpu_page),
    //    which task_reap deliberately leaves set so xtask_alloc's
    //    reclaim_lazy_resources can free them when the slot is reused
    //    (SMP race: schedule() may still be referencing them — see top of function).
    spin_lock(&tasks_lock);
    proc->pid = -1;
    proc->state = UNUSED;
    proc->k_rsp = 0;
    // proc->k_stack_top intentionally preserved (lazy reclaim)
    proc->cr3 = 0;
    proc->entry = 0;
    proc->wait_event = WAIT_NONE;
    proc->tgid = -1;
    proc->mm = NULL;
    proc->assigned_cpu = -1;
    proc->iopm = NULL;
    proc->wait_deadline = 0;
    proc->wait_timed_out = 0;
    list_init(&proc->run_node);
    // If wait_node is still linked in a list, list_init silently corrupts it
    WARN_ON(proc->wait_node.prev != &proc->wait_node);
    list_init(&proc->wait_node);
    // recv queue
    proc->recv_head = 0;
    proc->recv_tail = 0;
    proc->recv_lock = SPINLOCK_INIT;
    // REQ state
    proc->req_caller_pid = -1;
    proc->req_reply_buf = NULL;
    proc->req_result = 0;
    proc->req_target_pid = -1;
    // MSG state
    proc->msg_reply_buf = NULL;
    proc->msg_reply_len = 0;
    proc->msg_caller_pid = -1;
    proc->msg_result = 0;
    proc->msg_target_pid = -1;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    // Clear threading fields
    proc->fs_base = 0;
    proc->detached = 0;
    proc->tls_page = 0;
    proc->tls_total = 0;
    proc->user_stack_base = 0;
    proc->user_stack_size = 0;
    // proc->fpu_page intentionally preserved (lazy reclaim, 见函数顶部清单)
    spin_unlock(&tasks_lock);
}
