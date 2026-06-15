#include <stdint.h>
#include <stddef.h>

#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/trap.h"
#include "kernel/trap.h"
#include "kernel/mem/alloc.h"
#include "kernel/mem/slab.h"
#include "kernel/elf_loader.h"
#include "common/shm.h"
#include "common/macro.h"
#include "common/errno.h"
#include "kernel/fb.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "common/dev.h"
#include "arch/x64/apic.h"

proc_t procs[MAX_PROC];
// current_proc is per-CPU (in cpu_local_t), accessed via macro

spinlock_t procs_lock = {0};

// ===================== Timer queue operations =====================
// Must be called under scheduler_lock of the target CPU

// Insert process into per-CPU timer queue, sorted by wait_deadline ascending
// Must be called under scheduler_lock of the target CPU
void timer_queue_insert(int cpu, proc_t *proc) {
    list_node_t *head = &cpu_locals[cpu].timer_queue;
    list_node_t *node = head->next;
    while (node != head) {
        proc_t *p = LIST_ENTRY(node, proc_t, wait_node);
        if (p->wait_deadline > proc->wait_deadline) break;
        node = node->next;
    }
    // Insert before node
    proc->wait_node.prev = node->prev;
    proc->wait_node.next = node;
    node->prev->next = &proc->wait_node;
    node->prev = &proc->wait_node;
}

// Remove process from timer queue (no-op if not on any queue)
void timer_queue_remove(proc_t *proc) {
    list_remove(&proc->wait_node);
}

// ===================== Process table =====================

void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        procs[i].pid = -1;
        procs[i].state = READY;
        procs[i].k_rsp = 0;
        procs[i].k_stack_top = 0;
        procs[i].cr3 = 0;
        procs[i].entry = 0;
        procs[i].wait_event = WAIT_NONE;
        procs[i].assigned_cpu = -1;
        procs[i].iopl = 0;
        procs[i].parent_pid = -1;
        procs[i].exit_code = 0;
        procs[i].mmap_brk = 0;
        procs[i].mmap_regions = nullptr;
        procs[i].wait_deadline = 0;
        procs[i].wait_timed_out = 0;
        for (int j = 0; j < MAX_SHM_PER_PROC; j++) {
            procs[i].shm_regions[j].vaddr = 0;
            procs[i].shm_regions[j].phys = 0;
            procs[i].shm_regions[j].npages = 0;
            procs[i].shm_regions[j].ref_count = 0;
        }
        for (int j = 0; j < MAX_FD; j++) {
            procs[i].fd_table[j].type = FD_NONE;
            procs[i].fd_table[j].flags = 0;
            procs[i].fd_table[j].pipe = nullptr;
        }
        list_init(&procs[i].run_node);
        list_init(&procs[i].wait_node);
        // recv queue
        procs[i].recv_head = 0;
        procs[i].recv_tail = 0;
        procs[i].recv_lock = SPINLOCK_INIT;
        // RPC state
        procs[i].rpc_caller_pid = -1;
        procs[i].rpc_reply_buf = nullptr;
        procs[i].rpc_result = 0;
        procs[i].rpc_target_pid = -1;
        procs[i].cpu_time_ns = 0;
        procs[i].last_sched = 0;
    }
    cpu_locals[0]._cur_proc = nullptr;
    cpu_locals[0].run_count = 0;
    cpu_locals[0].idle_proc = nullptr;
    for (int c = 0; c < NUM_KMALLOC_CLASSES; c++) {
        cpu_locals[0].active_slab[c] = nullptr;
    }
}

// switch_to 恢复帧：callee-saved 寄存器 + 返回地址
struct switch_frame_t {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ret_addr;
};

// 在内核栈顶构建 trapframe + switch_frame，返回 k_rsp
static uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip, uint8_t iopl) {
    trapframe_t tf = {};
    tf.ss      = 0x23;                   // USER_DS
    tf.rsp     = 0x00007FFFFFFFE000;      // user stack top (top of mapped page at 0x7FFFFFFFD000)
    tf.rflags  = 0x202 | ((uint64_t)iopl << 12); // IF=1, IOPL
    tf.cs      = 0x2B;                   // USER_CS
    tf.rip     = entry_rip;
    tf.err_code = 0;
    tf.trapno  = 0;

    switch_frame_t sf = {};
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
    switch_frame_t sf = {};
    sf.ret_addr = (uint64_t)idle_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));

    return (uint64_t)sp;
}

// Create idle process for the specified CPU
proc_t *create_idle_process(int cpu_id) {
    spin_lock(&procs_lock);
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) { spin_unlock(&procs_lock); serial_puts("create_idle_process: no free slot\n"); return nullptr; }

    // Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) { spin_unlock(&procs_lock); serial_puts("create_idle_process: alloc stack failed\n"); return nullptr; }
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // Build idle switch_frame on kernel stack (no trapframe, no user mode)
    uint64_t k_rsp = build_idle_kstack(k_stack_top);

    // Fill PCB: idle uses kernel PML4, no user address space
    proc->pid = alloc_idx;
    proc->state = RUNNING;  // idle starts as RUNNING on its CPU
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = PHY_ADDR((uintptr_t)pml4); // kernel PML4 physical address
    proc->entry = (uint64_t)idle_entry;
    proc->wait_event = WAIT_NONE;
    proc->assigned_cpu = cpu_id;
    proc->iopl = 0;
    proc->parent_pid = -1;
    proc->exit_code = 0;
    proc->mmap_brk = 0x800000;
    proc->mmap_regions = nullptr;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    for (int j = 0; j < MAX_FD; j++) {
        proc->fd_table[j].type = FD_NONE;
        proc->fd_table[j].flags = 0;
        proc->fd_table[j].pipe = nullptr;
    }
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&procs_lock);

    cpu_locals[cpu_id].idle_proc = proc;

    return proc;
}

void idle_entry() {
    sti();
    while (1) {
        schedule();
        sti();
        __asm__ volatile("hlt");
    }
}

// Pick the CPU with the fewest runnable processes
static int pick_cpu() {
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

proc_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size, uint8_t iopl, bool map_fb) {
    // 1. Find free slot under procs_lock
    spin_lock(&procs_lock);
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) { spin_unlock(&procs_lock); return nullptr; }

    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) { spin_unlock(&procs_lock); return nullptr; }
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Allocate per-process PML4
    Page *pml4_page = bfc_alloc.alloc_page(1);
    if (!pml4_page) { spin_unlock(&procs_lock); return nullptr; }
    uint64_t pml4_phys = page_to_phys(pml4_page);
    uint64_t pml4_virt = phys_to_virt(pml4_phys);

    // 4. Clear PML4 + copy kernel entries
    uint64_t *new_pml4 = (uint64_t *)pml4_virt;
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
    }
    new_pml4[511] = pml4[511];

    // 5. Load ELF segments into user address space
    elf_load_result lr = elf_load(elf_data, elf_size, new_pml4);
    if (!lr.success) { spin_unlock(&procs_lock); return nullptr; }

    // 6b. Map framebuffer pages for KMS process
    if (map_fb && g_fb_info.fb_phys != 0) {
        uint64_t fb_phys = g_fb_info.fb_phys;
        uint64_t fb_size = g_fb_info.fb_size;
        uint64_t fb_end = ALIGN_UP(fb_phys + fb_size, PAGE_SIZE);
        uint64_t fb_start_page = fb_phys & ~0xFFFULL;  // page-align physical address
        uint64_t user_vaddr = 0x700000;
        uint64_t fb_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

        for (uint64_t pa = fb_start_page, va = user_vaddr;
             pa < fb_end;
             pa += PAGE_SIZE, va += PAGE_SIZE) {
            if (!map_user_page_direct(new_pml4, va, pa, fb_flags)) {
                spin_unlock(&procs_lock);
                return nullptr;
            }
        }
    }

    // 7. Map user stack page at 0x00007FFFFFFFD000
    Page *user_stack_page = bfc_alloc.alloc_page(1);
    if (!user_stack_page) { spin_unlock(&procs_lock); return nullptr; }
    uint64_t user_stack_phys = page_to_phys(user_stack_page);

    if (!map_user_page_direct(new_pml4, 0x00007FFFFFFFD000, user_stack_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
        spin_unlock(&procs_lock);
        return nullptr;
    }

    // 8. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack(k_stack_top, lr.entry, iopl);

    // 9. Fill PCB (still under procs_lock)
    int assigned_cpu = pick_cpu();
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pml4_phys;
    proc->entry = lr.entry;
    proc->wait_event = WAIT_NONE;
    proc->assigned_cpu = assigned_cpu;
    proc->iopl = iopl;
    proc->parent_pid = -1;
    proc->exit_code = 0;
    proc->mmap_brk = 0x800000;
    proc->mmap_regions = nullptr;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    for (int j = 0; j < MAX_FD; j++) {
        proc->fd_table[j].type = FD_NONE;
        proc->fd_table[j].flags = 0;
        proc->fd_table[j].pipe = nullptr;
    }
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&procs_lock);

    // Enqueue to target CPU's run_queue under scheduler_lock
    spin_lock(&cpu_locals[assigned_cpu].scheduler_lock);
    list_push_back(&cpu_locals[assigned_cpu].run_queue, &proc->run_node);
    cpu_locals[assigned_cpu].run_count++;
    spin_unlock(&cpu_locals[assigned_cpu].scheduler_lock);

    return proc;
}

void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    proc_t *idle = get_cpu_local()->idle_proc;
    proc_t *prev = current_proc;

    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

    // Check if run_queue has a runnable process
    if (list_empty(&cpu_locals[my_cpu].run_queue)) {
        // If prev is BLOCKED, ZOMBIE, or REAPING, it cannot continue running —
        // switch to idle so the CPU halts until an IRQ wakes a process.
        if (prev != idle && (prev->state == BLOCKED || prev->state == ZOMBIE || prev->state == REAPING)) {
            // Account prev's CPU time before switching to idle
            if (prev->last_sched != 0) {
                prev->cpu_time_ns += sched_clock() - prev->last_sched;
            }
            current_proc = idle;
            per_cpu_tss[my_cpu].rsp0 = idle->k_stack_top;
            get_cpu_local()->tss_rsp0 = idle->k_stack_top;
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
            switch_to(prev, idle);
            spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
        return; // no runnable process, prev continues
    }

    // Dequeue next process from head (FIFO round-robin)
    list_node_t *next_node = list_front(&cpu_locals[my_cpu].run_queue);
    proc_t *next = LIST_ENTRY(next_node, proc_t, run_node);
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
    current_proc = next;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;

    // Release lock before switch_to, re-acquire after — same pattern as old BKL
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
    switch_to(prev, next);
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}

// Free a page table page by physical address
static void free_table_page(uint64_t phys) {
    Page *p = &BFCAllocator::frames[PHY_TO_PAGE(phys)];
    bfc_alloc.free_page(p, 1);
}

// proc_reap: reclaim all resources of a process
// Called by sys_exit (no-parent path) or sys_waitpid
void proc_reap(proc_t *proc) {
    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(proc->cr3);

    // 1. Walk user PML4 entries (0-255, canonical low half), free leaf pages + page table pages
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        uint64_t pdpt_entry = pml4_virt[pml4_idx];
        if (!(pdpt_entry & PTE_PRESENT)) continue;

        uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pdpt_phys);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            uint64_t pd_entry = pdpt_virt[pdpt_idx];
            if (!(pd_entry & PTE_PRESENT)) continue;
            // Skip huge pages (shouldn't exist in user PML4, but safe check)
            if (pd_entry & PTE_PS) continue;

            uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
            uint64_t *pd_virt = (uint64_t *)phys_to_virt(pd_phys);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                uint64_t pt_entry = pd_virt[pd_idx];
                if (!(pt_entry & PTE_PRESENT)) continue;
                // Skip huge pages at PT level (2MB pages mapped as PD entries with PS bit)
                if (pt_entry & PTE_PS) continue;

                uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
                uint64_t *pt_virt = (uint64_t *)phys_to_virt(pt_phys);

                // Free all leaf pages in PT
                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pte = pt_virt[pt_idx];
                    if (pte & PTE_PRESENT) {
                        // Mask: keep only physical address bits [51:12], clear flags/NX
                        uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
                        // Skip dynamic shm regions (don't free shared pages)
                        bool is_shared = false;
                        for (int s = 0; s < MAX_SHM_PER_PROC; s++) {
                            if (proc->shm_regions[s].ref_count > 0) {
                                uint64_t sphys = proc->shm_regions[s].phys;
                                size_t snp = proc->shm_regions[s].npages;
                                if (leaf_phys >= sphys && leaf_phys < sphys + snp * PAGE_SIZE) {
                                    is_shared = true;
                                    break;
                                }
                            }
                        }
                        if (!is_shared) {
                            Page *leaf_page = &BFCAllocator::frames[PHY_TO_PAGE(leaf_phys)];
                            bfc_alloc.free_page(leaf_page, 1);
                        }
                        pt_virt[pt_idx] = 0;
                    }
                }
                // Free PT page itself
                free_table_page(pt_phys);
                pd_virt[pd_idx] = 0;
            }
            // Free PD page itself
            free_table_page(pd_phys);
            pdpt_virt[pdpt_idx] = 0;
        }
        // Free PDPT page itself
        free_table_page(pdpt_phys);
        pml4_virt[pml4_idx] = 0;
    }

    // 2. Free PML4 page itself
    free_table_page(proc->cr3);

    // 3. Free kernel stack (2 pages)
    // k_stack_top is the virtual address of stack top; compute physical base
    uint64_t k_stack_phys_base = PHY_ADDR(proc->k_stack_top - 2 * PAGE_SIZE);
    Page *stack_page = &BFCAllocator::frames[PHY_TO_PAGE(k_stack_phys_base)];
    bfc_alloc.free_page(stack_page, 2);

    // 4. Free mmap region metadata (physical pages already freed in step 1)
    mmap_region *region = proc->mmap_regions;
    while (region) {
        mmap_region *next = region->next;
        kfree(region);
        region = next;
    }

    // 5. Handle dynamic shm regions: unmap virtual pages were already freed
    //    in step 1 (PML4 walk skipped them). Now check if physical pages
    //    should be freed by scanning all processes for remaining references.
    for (int s = 0; s < MAX_SHM_PER_PROC; s++) {
        if (proc->shm_regions[s].ref_count > 0) {
            uint64_t sphys = proc->shm_regions[s].phys;
            size_t snp = proc->shm_regions[s].npages;
            // Count remaining references across all processes
            int refs = 0;
            spin_lock(&procs_lock);
            for (int p = 0; p < MAX_PROC; p++) {
                if (procs[p].pid < 0) continue;
                for (int j = 0; j < MAX_SHM_PER_PROC; j++) {
                    if (procs[p].shm_regions[j].ref_count > 0 &&
                        procs[p].shm_regions[j].phys == sphys &&
                        &procs[p] != proc) {
                        refs++;
                    }
                }
            }
            spin_unlock(&procs_lock);
            // If no other process references these pages, free them
            if (refs == 0) {
                Page *page = &BFCAllocator::frames[PHY_TO_PAGE(sphys)];
                bfc_alloc.free_page(page, snp);
            }
            proc->shm_regions[s].ref_count = 0;
        }
    }

    // 5b. Close all open fds
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (proc->fd_table[fd].type != FD_NONE) {
            struct pipe *p = proc->fd_table[fd].pipe;
            if (p) {
                p->ref_count--;
                // Notify blocked peer
                if (proc->fd_table[fd].flags & (O_WRONLY | O_RDWR)) {
                    if (p->read_pid >= 0) wake_process(p->read_pid);
                }
                if (proc->fd_table[fd].flags & (O_RDONLY | O_RDWR)) {
                    if (p->write_pid >= 0) wake_process(p->write_pid);
                }
                if (p->ref_count == 0) {
                    kfree(p->buf);
                    kfree(p);
                }
            }
            proc->fd_table[fd].type = FD_NONE;
            proc->fd_table[fd].flags = 0;
            proc->fd_table[fd].pipe = nullptr;
        }
    }

    // 6. Clear dev_table entries for this PID
    dev_table_cleanup(proc->pid);

    // 6b. Wake any processes waiting for RPC reply from this process
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid >= 0 &&
            procs[i].state == BLOCKED &&
            procs[i].wait_event == WAIT_RPC_REPLY &&
            procs[i].rpc_target_pid == proc->pid) {
            int wcpu = procs[i].assigned_cpu;
            spin_lock(&cpu_locals[wcpu].scheduler_lock);
            if (procs[i].state == BLOCKED && procs[i].wait_event == WAIT_RPC_REPLY) {
                procs[i].state = READY;
                procs[i].wait_event = WAIT_NONE;
                procs[i].rpc_result = ESRCH;
                list_push_back(&cpu_locals[wcpu].run_queue, &procs[i].run_node);
                cpu_locals[wcpu].run_count++;
            }
            spin_unlock(&cpu_locals[wcpu].scheduler_lock);
        }
    }

    // 7. Clear PCB slot
    spin_lock(&procs_lock);
    proc->pid = -1;
    proc->state = READY;
    proc->k_rsp = 0;
    proc->k_stack_top = 0;
    proc->cr3 = 0;
    proc->entry = 0;
    proc->wait_event = WAIT_NONE;
    proc->assigned_cpu = -1;
    proc->iopl = 0;
    proc->parent_pid = -1;
    proc->exit_code = 0;
    proc->mmap_brk = 0;
    proc->mmap_regions = nullptr;
    proc->wait_deadline = 0;
    proc->wait_timed_out = 0;
    for (int j = 0; j < MAX_SHM_PER_PROC; j++) {
        proc->shm_regions[j].vaddr = 0;
        proc->shm_regions[j].phys = 0;
        proc->shm_regions[j].npages = 0;
        proc->shm_regions[j].ref_count = 0;
    }
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    // recv queue
    proc->recv_head = 0;
    proc->recv_tail = 0;
    proc->recv_lock = SPINLOCK_INIT;
    // RPC state
    proc->rpc_caller_pid = -1;
    proc->rpc_reply_buf = nullptr;
    proc->rpc_result = 0;
    proc->rpc_target_pid = -1;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    spin_unlock(&procs_lock);
}
