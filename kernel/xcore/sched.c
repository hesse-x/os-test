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

xtask_t tasks[MAX_PROC];
// current_task is per-CPU (in cpu_local_t), accessed via macro

spinlock_t tasks_lock = SPINLOCK_INIT;
pid_t init_pid = -1;

// ===================== Process table =====================

// Allocate a free slot from tasks[] under tasks_lock.
// Returns pointer to the free xtask_t, or NULL if no free slot.
// Caller must hold tasks_lock; on success the slot's pid is still -1
// (caller sets it after allocating resources).
xtask_t *xtask_alloc(void) {
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].pid < 0) {
            return &tasks[i];
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
    trapframe_t tf = {0};
    tf.ss      = 0x23;                   // USER_DS
    tf.rsp     = 0x00007FFFFFFFE000;      // user stack top
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

// FPU 上下文切换 C helper（不修改 switch_to 汇编）
// 在 switch_to(prev, next) 之前调用：保存 prev 的 FPU 状态 + 设 CR0.TS
void fpu_context_switch(xtask_t *prev, xtask_t *next) {
    (void)next;
    if (prev && prev->used_fpu) {
        if (!prev->fpu_page) {
            // lazy 分配，页对齐满足 fxsave 16 字节要求
            prev->fpu_page = bfc_alloc_page(1);
        }
        if (prev->fpu_page) {
            void *fpu_data = (void *)(__force uintptr_t)phys_to_virt(page_to_phys(prev->fpu_page));
            // 防御：fxsave 目标必须是 BFC 数据页虚拟地址，不能是 Page* 元数据指针
            // （历史 bug：误把 bfc_alloc_page 返回的 Page* 直接当数据指针，fxsave 覆盖
            //  Page 元数据，后续 kfree 检测 page->status 异常才暴露，定位困难）
            uint64_t vma_start = (__force uint64_t)phys_to_virt(0);
            uint64_t vma_end = vma_start + total_page_frames * PAGE_SIZE;
            ASSERT((uint64_t)fpu_data >= vma_start && (uint64_t)fpu_data < vma_end);
            __asm__ volatile("fxsave (%0)" :: "r"(fpu_data));
        }
    }
    // 设 TS=1，新线程首次用 SSE 触发 #NM
    __asm__ volatile("clts");  // 先清，确保 fxsave 之后的状态干净
    uint64_t cr0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 3);  // CR0.TS
    __asm__ volatile("movq %0, %%cr0" :: "r"(cr0));
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
void task_reap(xtask_t *proc) {
    ASSERT(proc->state == ZOMBIE || proc->state == REAPING);
    pid_t owner_pid = proc->pid;

    // 1. Free kernel stack (2 pages)
    uint64_t k_stack_phys_base = (__force uint64_t)PHY_ADDR(proc->k_stack_top - 2 * PAGE_SIZE);
    Page *stack_page = &bfc_frames[PHY_TO_PAGE(k_stack_phys_base)];
    bfc_free_page(stack_page, 2);

    // 2. Free IOPM bitmap
    if (proc->iopm) {
        kfree(proc->iopm);
        proc->iopm = NULL;
    }

    // 2b. Free FPU state page (lazy-allocated via bfc_alloc_page)
    if (proc->fpu_page) {
        bfc_free_page(proc->fpu_page, 1);
        proc->fpu_page = NULL;
    }

    // 3. mm_put (decrement triggers mm_release when ref_count hits 0)
    //    mm_release will: free user pages+PML4+mmap+SHM+files+devtmpfs+irq_owner+wake waiters
    if (proc->mm) {
        pid_t pid_for_cleanup = owner_pid;
        mm_t *mm = proc->mm;
        proc->mm = NULL;
        if (refcount_dec_and_test(&mm->m_count)) {
            mm_release(mm, pid_for_cleanup);
        }
    } else {
        // idle process: still need cleanup for devtmpfs/irq_owner
        if (devtmpfs_cleanup_hook) devtmpfs_cleanup_hook(owner_pid);
        irq_owner_cleanup(owner_pid);
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

    // 6. POSIX cleanup: close fds, free proc
    if (proc_reap_hook) proc_reap_hook(proc);

    // 7. Clear PCB slot
    spin_lock(&tasks_lock);
    proc->pid = -1;
    proc->state = UNUSED;
    proc->k_rsp = 0;
    proc->k_stack_top = 0;
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
    proc->fpu_page = NULL;
    proc->used_fpu = 0;
    spin_unlock(&tasks_lock);
}
