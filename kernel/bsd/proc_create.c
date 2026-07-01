// kernel/bsd/proc_create.c — Process creation entry point (process_create_elf)
// BSD layer: creates user process from ELF data, bridging Xcore scheduler + BSD proc + ELF loader

#include <stdint.h>
#include <stddef.h>

#include "kernel/xcore/xtask.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/sched.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/bsd/elf_loader.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "common/signal.h"
#include "common/macro.h"

// process_create_elf: create user process from ELF data
xtask_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size) {
    xtask_t *proc = NULL;
    Page *stack_pages = NULL;
    mm_t *mm = NULL;
    Page *user_stack_page = NULL;
    int user_stack_mapped = 0;  // number of user stack pages successfully mapped

    // 1. Find free slot under tasks_lock
    spin_lock(&tasks_lock);
    proc = xtask_alloc();
    if (!proc) {
        spin_unlock(&tasks_lock);
        printk(LOG_ERROR, "process_create_elf: no free slot\n");
        return NULL;
    }
    int alloc_idx = (int)(proc - tasks);

    // 2. Allocate kernel stack (8KB = 2 pages)
    stack_pages = bfc_alloc_page(2);
    if (!stack_pages) goto fail_slot;
    uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
    uint64_t k_stack_top = (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) + 2 * PAGE_SIZE;

    // 2b. Pre-allocate FPU state page (eager FPU: every user task gets one)
    if (!xcore_fpu_alloc(proc)) goto fail_stack;

    // 3. Create mm_t (allocates PML4 + files_t)
    mm = mm_create();
    if (!mm) goto fail_stack;
    uint64_t pml4_phys = mm->cr3;
    uint64_t pml4_virt = (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);
    uint64_t *new_pml4 = (uint64_t *)pml4_virt;

    // 4. Load ELF segments into user address space
    elf_load_result_t lr = elf_load(elf_data, elf_size, new_pml4);
    if (!lr.success) {
        printk(LOG_ERROR, "process_create_elf: elf_load failed\n");
        goto fail_mm;
    }

    // 5. Map user stack: 2048 pages (8MB) at 0x7FFFFFFF0000-0x7FFFFFFFE000
    int user_stack_pages = 2048;
    user_stack_page = bfc_alloc_page(user_stack_pages);
    if (!user_stack_page) goto fail_mm;
    uint64_t user_stack_phys = (__force uint64_t)page_to_phys(user_stack_page);
    uint64_t stack_base = 0x00007FFFFFFFE000 - (uint64_t)user_stack_pages * PAGE_SIZE;

    for (int i = 0; i < user_stack_pages; i++) {
        if (!map_user_page_direct(new_pml4, stack_base + i * PAGE_SIZE,
                                 user_stack_phys + i * PAGE_SIZE,
                                 PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
            user_stack_mapped = i;  // remember how many were mapped for partial cleanup
            goto fail_user_stack;
        }
    }
    user_stack_mapped = user_stack_pages;

    // Map shared trampoline page at fixed user address (failure is non-critical, just log)
    if (sig_trampoline_phys != 0) {
        if (!map_user_page_direct(new_pml4, SIG_TRAMPOLINE_ADDR, sig_trampoline_phys,
                                 PTE_PRESENT | PTE_USER)) {
            printk(LOG_ERROR, "process_create_elf: failed to map trampoline page\n");
        }
    }

    // 6. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack(k_stack_top, lr.entry);

    // 7. Fill PCB (still under tasks_lock)
    int assigned_cpu = pick_cpu();
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pml4_phys;  // cached
    proc->entry = lr.entry;
    proc->wait_event = WAIT_NONE;
    proc->tgid = proc->pid;
    proc->mm = mm;
    proc->assigned_cpu = assigned_cpu;
    proc->iopm = NULL;
    proc->proc = NULL;  // created below
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    // POSIX fields are in proc (created separately)
    list_init(&proc->run_node);
    list_init(&proc->wait_node);

    // Create proc for this user process
    proc_t *bp = proc_create();
    if (!bp) goto fail_mm;
    proc->proc = bp;
    bp->xtask = proc;

    // Main thread (leader): tgid == pid (set above), signal->parent_pid = -1 (root process)
    bp->signal->parent_pid = -1;

    // Create stack mmap_region
    mmap_region_t *stack_region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
    if (stack_region) {
        __memset(stack_region, 0, sizeof(mmap_region_t));
        stack_region->vaddr = stack_base;
        stack_region->size = (uint64_t)user_stack_pages * PAGE_SIZE;
        stack_region->phys = 0; // not MAP_PHYSICAL — anonymous stack
        stack_region->prot = PROT_READ | PROT_WRITE;
        stack_region->next = NULL;
        mm->mmap_regions = stack_region;
    }

    printk(LOG_DEBUG, "process_create_elf: pid=%d kstack_phys=0x%lx kstack_top=0x%lx\n",
        proc->pid, k_stack_phys, k_stack_top);

    spin_unlock(&tasks_lock);

    // Enqueue to target CPU's run_queue under scheduler_lock
    uint64_t rflags;
    spin_lock_irqsave(&cpu_locals[assigned_cpu].scheduler_lock, &rflags);
    list_push_back(&cpu_locals[assigned_cpu].run_queue, &proc->run_node);
    cpu_locals[assigned_cpu].run_count++;
    spin_unlock_irqrestore(&cpu_locals[assigned_cpu].scheduler_lock, rflags);

    return proc;

    // === Error cleanup paths ===
fail_user_stack:
    // Unmap partially-mapped user stack pages (mm_put will free PML4 + page table pages)
    for (int i = 0; i < user_stack_mapped; i++) {
        unmap_user_pages(new_pml4, stack_base + i * PAGE_SIZE, stack_base + (i + 1) * PAGE_SIZE, 1);
    }
    bfc_free_page(user_stack_page, user_stack_pages);
    // fall through to mm cleanup
fail_mm:
    mm_put(mm);  // releases PML4 + page table pages + files_t
    // fall through to stack cleanup
fail_stack:
    bfc_free_page(stack_pages, 2);
    // Free FPU state page if allocated (eager FPU pre-allocation)
    if (proc->fpu_page) { bfc_free_page(proc->fpu_page, 1); proc->fpu_page = NULL; }
    // fall through to slot cleanup
fail_slot:
    // proc slot was never modified (pid still -1, state still UNUSED)
    spin_unlock(&tasks_lock);
    return NULL;
}
