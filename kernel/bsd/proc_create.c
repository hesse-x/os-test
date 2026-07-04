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
#include "arch/x64/memlayout.h"
#include "xos/signal.h"
#include "xos/elf.h"
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
    int alloc_idx = -1;
    proc = xtask_alloc(&alloc_idx);
    if (!proc) {
        spin_unlock(&tasks_lock);
        printk(LOG_ERROR, "process_create_elf: no free slot\n");
        return NULL;
    }

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

    // === argc/argv/envp/auxv 栈构建（标准 SysV ABI） ===
    // 内核直接创建的 init 进程没有 execve 路径，注入 argv[0]="/init"。
    // 栈是连续物理内存，用户态地址 [stack_base, USER_STACK_TOP) 映射连续。
    #define ARG_MAX 128
    char (*argv_strings)[256] = (char (*)[256])kmalloc(sizeof(char[ARG_MAX][256]));
    uint64_t *argv_str_vaddrs = (uint64_t *)kmalloc(sizeof(uint64_t) * ARG_MAX);
    if (!argv_strings || !argv_str_vaddrs) {
        if (argv_strings) kfree(argv_strings);
        if (argv_str_vaddrs) kfree(argv_str_vaddrs);
        goto fail_mm;
    }
    // argv[0] = "/init"
    static const char init_path[] = "/init";
    int argc = 1;
    for (int k = 0; init_path[k]; k++) argv_strings[0][k] = init_path[k];
    argv_strings[0][sizeof(init_path) - 1] = '\0';
    int envc = 0;

    // STACK_KV: sp_user 用户态地址 → 内核虚拟地址
    #define STACK_KV(sp_user) \
        ((void *)((__force uint64_t)phys_to_virt((__force phys_addr_t)(user_stack_phys \
            + (uint64_t)user_stack_pages * PAGE_SIZE - (USER_STACK_TOP - (sp_user))))))

    uint64_t sp_user = USER_STACK_TOP;

    // 1. AT_RANDOM 16 字节（填 0）
    sp_user -= 16;
    __memset(STACK_KV(sp_user), 0, 16);
    uint64_t at_random_vaddr = sp_user;

    // 2. AT_EXECFN
    int execfn_len = sizeof(init_path) - 1;
    sp_user -= (execfn_len + 1);
    __memcpy(STACK_KV(sp_user), init_path, execfn_len + 1);
    uint64_t at_execfn_vaddr = sp_user;

    // 3. argv 字符串
    for (int i = argc - 1; i >= 0; i--) {
        int len = 0;
        while (argv_strings[i][len]) len++;
        sp_user -= (len + 1);
        __memcpy(STACK_KV(sp_user), argv_strings[i], len + 1);
        argv_str_vaddrs[i] = sp_user;
    }

    // 4. 16 字节对齐
    while ((sp_user % 16) != 0) sp_user--;

    // 5. auxv（6 对 + AT_NULL）
    int auxc = 6;
    sp_user -= (auxc + 1) * 16;
    uint64_t *auxv = (uint64_t *)STACK_KV(sp_user);
    int ai = 0;
    auxv[ai++] = AT_PHDR;   auxv[ai++] = lr.phdr_vaddr;
    auxv[ai++] = AT_PHENT;  auxv[ai++] = lr.phent;
    auxv[ai++] = AT_PHNUM;  auxv[ai++] = lr.phnum;
    auxv[ai++] = AT_ENTRY;  auxv[ai++] = lr.entry;
    auxv[ai++] = AT_PAGESZ; auxv[ai++] = PAGE_SIZE;
    auxv[ai++] = AT_RANDOM; auxv[ai++] = at_random_vaddr;
    auxv[ai++] = AT_EXECFN; auxv[ai++] = at_execfn_vaddr;
    auxv[ai++] = AT_NULL;   auxv[ai++] = 0;

    // 6. envp 指针数组 + NULL
    sp_user -= (envc + 1) * 8;
    uint64_t *envp_arr = (uint64_t *)STACK_KV(sp_user);
    envp_arr[envc] = 0;

    // 7. argv 指针数组 + NULL
    sp_user -= (argc + 1) * 8;
    uint64_t *argv_arr = (uint64_t *)STACK_KV(sp_user);
    argv_arr[argc] = 0;
    for (int i = 0; i < argc; i++) argv_arr[i] = argv_str_vaddrs[i];

    // 8. argc
    sp_user -= 8;
    *(uint64_t *)STACK_KV(sp_user) = (uint64_t)argc;

    uint64_t user_sp = sp_user;
    #undef STACK_KV
    #undef ARG_MAX

    kfree(argv_strings);
    kfree(argv_str_vaddrs);

    // 6. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack_user_rsp(k_stack_top, lr.entry, user_sp);

    // 7. Fill PCB (still under tasks_lock)
    // 顺序约束:assigned_cpu 必须先于 pid 赋值(wake 路径在锁前无锁读 assigned_cpu,
    // 防 pid 生效后 assigned_cpu 仍是 -1 导致 cpu_locals[-1] 越界)
    int assigned_cpu = pick_cpu();
    proc->assigned_cpu = assigned_cpu;
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pml4_phys;  // cached
    proc->entry = lr.entry;
    proc->wait_event = WAIT_NONE;
    proc->tgid = proc->pid;
    proc->mm = mm;
    proc->iopm = NULL;
    proc->proc = NULL;  // created below
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    proc->exit_code = 0;
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
    int cpu = proc->assigned_cpu;
    uint64_t rflags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
    // TOCTOU 重检:入队期间目标 CPU 已被塞满则重新选 CPU
    if (__atomic_load_n(&cpu_locals[cpu].run_count, __ATOMIC_RELAXED) > RECHECK_THRESHOLD) {
        int new_cpu = pick_cpu();
        if (new_cpu != cpu) {
            spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
            proc->assigned_cpu = new_cpu;   // 此时尚未入队,改字段安全
            cpu = new_cpu;
            spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
        }
    }
    list_push_back(&cpu_locals[cpu].run_queue, &proc->run_node);
    cpu_locals[cpu].run_count++;
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);

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
    // 动态化：slot 由 xtask_alloc 从 xtask_cache 分配，失败须回收到 cache
    // 防 leak。槽位从未发布（pid 仍 -1、未入任何队列），无 schedule() 引用，
    // 此处直接 free 安全。注意 k_stack/fpu_page 已在 fail_stack 释放，
    // 对象本身无 lazy 资源残留。
    tasks[alloc_idx] = NULL;
    kmem_cache_free(xtask_cache, proc);
    spin_unlock(&tasks_lock);
    return NULL;
}
