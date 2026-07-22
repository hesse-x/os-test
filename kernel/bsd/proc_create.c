/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/proc_create.c — Process creation entry point (process_create_elf)
// BSD layer: creates user process from ELF data, bridging Xcore scheduler + BSD
// proc + ELF loader

#include <stddef.h>
#include <stdint.h>

#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/elf_loader.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signal.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <xos/elf.h>
#include <xos/mman.h>
#include <xos/page.h>
#include <xos/signal.h>

// process_create_elf: create user process from ELF data
xtask *process_create_elf(const uint8_t *elf_data, uint64_t elf_size) {
  xtask *proc = NULL;
  struct page *stack_pages = NULL;
  mm *mm = NULL;
  struct page *user_stack_page = NULL;
  int user_stack_mapped = 0; // number of user stack pages successfully mapped

  // 1. Find free slot under tasks_lock
  spin_lock(&tasks_lock);
  int alloc_idx = -1;
  proc = xtask_alloc(&alloc_idx);
  if (!proc) {
    spin_unlock(&tasks_lock);
    printk(LOG_ERROR, "process_create_elf: no free slot\n");
    return NULL;
  }

  // 2. Allocate kernel stack (KERNEL_STACK_SIZE)
  stack_pages = bfc_alloc_page(KERNEL_STACK_PAGES);
  if (!stack_pages)
    goto fail_slot;
  uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
  uint64_t k_stack_top =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) +
      KERNEL_STACK_SIZE;

  // 2b. Pre-allocate FPU state page (eager FPU: every user task gets one)
  if (!xcore_fpu_alloc(proc))
    goto fail_stack;

  // 3. Create mm (allocates PML4 + files)
  mm = mm_create();
  if (!mm)
    goto fail_stack;
  uint64_t pml4_phys = mm->cr3;
  uint64_t pml4_virt =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);
  uint64_t *new_pml4 = (uint64_t *)pml4_virt;

  // 4. Load ELF segments into user address space
  elf_load_result lr = elf_load(elf_data, elf_size, new_pml4);
  if (!lr.success) {
    printk(LOG_ERROR, "process_create_elf: elf_load failed\n");
    goto fail_mm;
  }

  // 5. Map user stack: 2048 pages (8MB) at 0x7FFFFFFF0000-0x7FFFFFFFE000
  int user_stack_pages = 2048;
  user_stack_page = bfc_alloc_page(user_stack_pages);
  if (!user_stack_page)
    goto fail_mm;
  uint64_t user_stack_phys = (__force uint64_t)page_to_phys(user_stack_page);
  uint64_t stack_base =
      0x00007FFFFFFFE000 - (uint64_t)user_stack_pages * PAGE_SIZE;

  for (int i = 0; i < user_stack_pages; i++) {
    if (!map_user_page_direct(new_pml4, stack_base + i * PAGE_SIZE,
                              user_stack_phys + i * PAGE_SIZE,
                              PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
      user_stack_mapped =
          i; // remember how many were mapped for partial cleanup
      goto fail_user_stack;
    }
  }
  user_stack_mapped = user_stack_pages;

  // Map shared trampoline page at fixed user address (failure is non-critical,
  // just log)
  if (sig_trampoline_phys != 0) {
    if (!map_user_page_direct(new_pml4, SIG_TRAMPOLINE_ADDR,
                              sig_trampoline_phys, PTE_PRESENT | PTE_USER)) {
      printk(LOG_ERROR, "process_create_elf: failed to map trampoline page\n");
    }
  }

// === argc/argv/envp/auxv stack construction (standard SysV ABI) ===
// The init process created directly by the kernel has no execve path; inject
// argv[0]="/init". The stack is contiguous physical memory; user-space
// addresses [stack_base, USER_STACK_TOP) are contiguously mapped.
#define ARG_MAX 128
  char(*argv_strings)[256] = (char(*)[256])kmalloc(sizeof(char[ARG_MAX][256]));
  uint64_t *argv_str_vaddrs = (uint64_t *)kmalloc(sizeof(uint64_t) * ARG_MAX);
  if (!argv_strings || !argv_str_vaddrs) {
    if (argv_strings)
      kfree(argv_strings);
    if (argv_str_vaddrs)
      kfree(argv_str_vaddrs);
    goto fail_mm;
  }
  // argv[0] = "/init"
  static const char init_path[] = "/init";
  int argc = 1;
  for (int k = 0; init_path[k]; k++)
    argv_strings[0][k] = init_path[k];
  argv_strings[0][sizeof(init_path) - 1] = '\0';
  int envc = 0;

// STACK_KV: sp_user user-space address -> kernel virtual address
#define STACK_KV(sp_user)                                                      \
  ((void *)((__force uint64_t)phys_to_virt((__force phys_addr_t)(              \
      user_stack_phys + (uint64_t)user_stack_pages * PAGE_SIZE -               \
      (USER_STACK_TOP - (sp_user))))))

  uint64_t sp_user = USER_STACK_TOP;

  // 1. AT_RANDOM 16 bytes (zero-filled)
  sp_user -= 16;
  __memset(STACK_KV(sp_user), 0, 16);
  uint64_t at_random_vaddr = sp_user;

  // 2. AT_EXECFN
  int execfn_len = sizeof(init_path) - 1;
  sp_user -= (execfn_len + 1);
  __memcpy(STACK_KV(sp_user), init_path, execfn_len + 1);
  uint64_t at_execfn_vaddr = sp_user;

  // 3. argv strings
  for (int i = argc - 1; i >= 0; i--) {
    int len = 0;
    while (argv_strings[i][len])
      len++;
    sp_user -= (len + 1);
    __memcpy(STACK_KV(sp_user), argv_strings[i], len + 1);
    argv_str_vaddrs[i] = sp_user;
  }

  // 4. 16-byte alignment
  while ((sp_user % 16) != 0)
    sp_user--;

  // 5. auxv (6 pairs + AT_NULL)
  int auxc = 6;
  sp_user -= (auxc + 1) * 16;
  uint64_t *auxv = (uint64_t *)STACK_KV(sp_user);
  int ai = 0;
  auxv[ai++] = AT_PHDR;
  auxv[ai++] = lr.phdr_vaddr;
  auxv[ai++] = AT_PHENT;
  auxv[ai++] = lr.phent;
  auxv[ai++] = AT_PHNUM;
  auxv[ai++] = lr.phnum;
  auxv[ai++] = AT_ENTRY;
  auxv[ai++] = lr.entry;
  auxv[ai++] = AT_PAGESZ;
  auxv[ai++] = PAGE_SIZE;
  auxv[ai++] = AT_RANDOM;
  auxv[ai++] = at_random_vaddr;
  auxv[ai++] = AT_EXECFN;
  auxv[ai++] = at_execfn_vaddr;
  auxv[ai++] = AT_NULL;
  auxv[ai++] = 0;

  // 6. envp pointer array + NULL
  sp_user -= (envc + 1) * 8;
  uint64_t *envp_arr = (uint64_t *)STACK_KV(sp_user);
  envp_arr[envc] = 0;

  // 7. argv pointer array + NULL
  sp_user -= (argc + 1) * 8;
  uint64_t *argv_arr = (uint64_t *)STACK_KV(sp_user);
  argv_arr[argc] = 0;
  for (int i = 0; i < argc; i++)
    argv_arr[i] = argv_str_vaddrs[i];

  // 8. argc
  sp_user -= 8;
  *(uint64_t *)STACK_KV(sp_user) = (uint64_t)argc;

  uint64_t user_sp = sp_user;
#undef STACK_KV
#undef ARG_MAX

  kfree(argv_strings);
  kfree(argv_str_vaddrs);

  // 6. Build trapframe + switch_to frame on kernel stack
  uint64_t k_rsp = sched_build_kstack_user_rsp(k_stack_top, lr.entry, user_sp);

  // 7. Fill PCB (still under tasks_lock)
  // Ordering constraint: assigned_cpu must be set before pid (the wake path
  // reads assigned_cpu locklessly before taking the lock, prevent
  // cpu_locals[-1] OOB if pid takes effect while assigned_cpu is still -1)
  int assigned_cpu = sched_pick_cpu();
  proc->assigned_cpu = assigned_cpu;
  proc->pid = alloc_idx;
  proc->state = READY;
  proc->k_rsp = k_rsp;
  proc->k_stack_top = k_stack_top;
  proc->cr3 = pml4_phys; // cached
  proc->entry = lr.entry;
  proc->wait_event = WAIT_NONE;
  proc->tgid = proc->pid;
  proc->mm = mm;
  proc->iopm = NULL;
  proc->proc = NULL; // created below
  proc->cpu_time_ns = 0;
  proc->last_sched = 0;
  proc->exit_code = 0;
  // POSIX fields are in proc (created separately)
  list_init(&proc->run_node);
  list_init(&proc->wait_node);

  // Create proc for this user process
  struct proc *bp = proc_create();
  if (!bp)
    goto fail_mm;
  proc->proc = bp;
  bp->xtask = proc;

  // Main thread (leader): tgid == pid (set above), signal->parent_pid = -1
  // (root process)
  bp->signal->parent_pid = -1;

  // Create stack mmap_region
  mmap_region *stack_region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (stack_region) {
    __memset(stack_region, 0, sizeof(mmap_region));
    stack_region->vaddr = stack_base;
    stack_region->size = (uint64_t)user_stack_pages * PAGE_SIZE;
    stack_region->phys = 0; // not MAP_PHYSICAL — anonymous stack
    stack_region->prot = PROT_READ | PROT_WRITE;
    stack_region->fd = -1; // anonymous
    stack_region->offset = 0;
    stack_region->flags = MAP_ANONYMOUS;
    stack_region->next = NULL;
    mm->mmap_regions = stack_region;
  }

  printk(LOG_DEBUG,
         "process_create_elf: pid=%d kstack_phys=0x%lx kstack_top=0x%lx\n",
         proc->pid, k_stack_phys, k_stack_top);

  spin_unlock(&tasks_lock);

  // Enqueue to target CPU's run_queue under scheduler_lock
  int cpu = proc->assigned_cpu;
  uint64_t rflags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
  // TOCTOU recheck: if the target CPU has been filled past the threshold
  // during enqueue, re-pick a CPU
  if (__atomic_load_n(&cpu_locals[cpu].run_count, __ATOMIC_RELAXED) >
      RECHECK_THRESHOLD) {
    int new_cpu = sched_pick_cpu();
    if (new_cpu != cpu) {
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
      proc->assigned_cpu = new_cpu; // not yet enqueued, safe to mutate field
      cpu = new_cpu;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
    }
  }
  run_queue_push(cpu, proc);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);

  return proc;

  // === Error cleanup paths ===
fail_user_stack:
  // Unmap partially-mapped user stack pages (mm_put will free PML4 + page table
  // pages)
  for (int i = 0; i < user_stack_mapped; i++) {
    unmap_user_pages(new_pml4, stack_base + i * PAGE_SIZE,
                     stack_base + (i + 1) * PAGE_SIZE, 1);
  }
  bfc_free_page(user_stack_page, user_stack_pages);
  // fall through to mm cleanup
fail_mm:
  mm_put(mm); // releases PML4 + page table pages + files
              // fall through to stack cleanup
fail_stack:
  bfc_free_page(stack_pages, KERNEL_STACK_PAGES);
  // Free FPU state page if allocated (eager FPU pre-allocation)
  if (proc->fpu_page) {
    bfc_free_page(proc->fpu_page, 1);
    proc->fpu_page = NULL;
  }
  // fall through to slot cleanup
fail_slot:
  // Dynamic: the slot is allocated by xtask_alloc from xtask_cache; on
  // failure it must be returned to the cache to prevent leaks. The slot was
  // never published (pid still -1, not on any queue), no schedule() references
  // it, so freeing here is safe. Note k_stack/fpu_page were already freed in
  // fail_stack; the object itself has no lazy resource remnants.
  tasks[alloc_idx] = NULL;
  kmem_cache_free(xtask_cache, proc);
  spin_unlock(&tasks_lock);
  return NULL;
}
