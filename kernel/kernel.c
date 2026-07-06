/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// 64-bit higher-half kernel, compiled with -mcmodel=kernel
// kernel_main: runs in virtual address space,
//   xcore_init + driver_init + bsd_init + idle + load init from boot_info
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/proc.h"
#include "kernel/kernel.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"

// VFS data structure init (must run before driver_init so devtmpfs_create
// works)
void inode_init(void);
void page_cache_init(void);
void devtmpfs_init(void);

// POSIX hostname (group 1). Default "myos"; guarded against concurrent
// sethostname/gethostname. Plain NUL-terminated C string, len < HOSTNAME_MAX.
char hostname[HOSTNAME_MAX] = "myos";
static spinlock hostname_lock = SPINLOCK_INIT;

void hostname_set(const char *name, size_t len) {
  if (len >= HOSTNAME_MAX)
    len = HOSTNAME_MAX - 1;
  uint64_t flags;
  spin_lock_irqsave(&hostname_lock, &flags);
  __memcpy(hostname, name, len);
  hostname[len] = '\0';
  spin_unlock_irqrestore(&hostname_lock, flags);
}

// Copy out the current hostname (NUL-terminated) into dst; return the length
// not counting the terminator. dst is a kernel buffer (callers do the
// copy_to_user themselves so this stays lock-clean of user faults).
size_t hostname_get(char *dst, size_t maxlen) {
  uint64_t flags;
  spin_lock_irqsave(&hostname_lock, &flags);
  size_t n = 0;
  while (n < maxlen && hostname[n] != '\0') {
    dst[n] = hostname[n];
    n++;
  }
  if (n < maxlen)
    dst[n] = '\0';
  spin_unlock_irqrestore(&hostname_lock, flags);
  return n;
}

// phys_to_virt is available after xcore_init() builds the higher-half
// direct map covering all physical RAM. The stub places init.elf into
// EfiLoaderData memory (recorded in boot_info), and the kernel maps it
// here to create the init process — no early disk I/O required.
extern kern_vaddr_t phys_to_virt(phys_addr_t phys);

void kernel_main(boot_info *bi) {
  // Layered initialization
  xcore_init(bi);

  // VFS data structures must be initialized before driver_init
  // so that devtmpfs_create() calls during driver_init survive
  inode_init();
  page_cache_init();
  devtmpfs_init();

  driver_init();
  bsd_init();

  printk(LOG_INFO, "kernel_main: all subsystems initialized\n");

  // Early BSP-side SSE self-test: verify CR4.OSFXSR lets SSE instructions
  // execute without #UD. This is the exact failure mode seen when APs
  // missed enable_sse() — catching it here gives a clear kernel-side
  // panic instead of a mysterious user-mode #UD later.
  // eager FPU: CR0.TS is always 0, no clts/restore TS needed.
  {
    double src = 3.14, dst = 0.0;
    // Note: no "xmm0" clobber — kernel is compiled with -mno-sse, so gcc
    // rejects listing xmm0. Safe because the kernel never uses SSE regs
    // for its own computation; xmm0 is only touched by this snippet.
    __asm__ volatile("movsd %1, %%xmm0\n"
                     "movsd %%xmm0, %0\n"
                     : "=m"(dst)
                     : "m"(src));

    if (dst != 3.14) {
      /* Kernel printk does not support %f (built with -mno-sse); the
       * self-test only needs a pass/fail signal. */
      panic("kernel_sse_selftest: SSE result wrong\n");
    }
    printk(LOG_INFO, "kernel_sse_selftest: PASS (BSP)\n");
  }

  // Create BSP idle process
  xtask *bsp_idle = sched_create_idle_process(0);
  if (!bsp_idle) {
    printk(LOG_ERROR, "kernel_main: create BSP idle failed\n");
    halt();
  }
  printk(LOG_INFO, "kernel_main: BSP idle created\n");

  // Create init process from the init.elf image loaded by the EFI stub
  // into EfiLoaderData memory (recorded in boot_info). This avoids any
  // early disk I/O — the FAT32 driver is not yet initialized at this point.
  // After init starts, it spawns user services (kbd_driver, terminal, shell,
  // drm_test) via execve, which read from the FAT32 root partition mounted
  // later in vfs_init().
  bool init_loaded = false;
  if (bi->init_elf_addr != 0 && bi->init_elf_size != 0) {
    const uint8_t *init_elf = (__force const uint8_t *)phys_to_virt(
        (__force phys_addr_t)bi->init_elf_addr);
    printk(LOG_INFO, "kernel_main: loading init (phys=0x%lx size=%lu)...\n",
           bi->init_elf_addr, (unsigned long)bi->init_elf_size);
    xtask *init_proc = process_create_elf(init_elf, bi->init_elf_size);
    if (init_proc) {
      init_loaded = true;
      init_pid = init_proc->pid;
      printk(LOG_INFO, "kernel_main: init created (pid=%d)\n", init_pid);
    } else {
      printk(LOG_ERROR, "kernel_main: process_create_elf for init failed\n");
    }
  } else {
    printk(LOG_ERROR, "kernel_main: no init.elf in boot_info\n");
  }

  if (!init_loaded)
    printk(LOG_ERROR, "kernel_main: init FAILED to load\n");

  printk(LOG_INFO, "kernel_main: all tasks loaded, entering idle\n");

  sti();

  // Set current_task to BSP idle, switch to idle kernel stack, enter
  // sched_idle_entry
  current_task = bsp_idle;
  bsp_idle->state = RUNNING;
  per_cpu_tss[0].rsp0 = bsp_idle->k_stack_top;
  cpu_locals[0].tss_rsp0 = bsp_idle->k_stack_top;

  sti();

  uint64_t idle_rsp = bsp_idle->k_rsp;
  __asm__ volatile("movq %0, %%rsp\n"
                   "popq %%rbx\n"
                   "popq %%rbp\n"
                   "popq %%r12\n"
                   "popq %%r13\n"
                   "popq %%r14\n"
                   "popq %%r15\n"
                   "retq\n" ::"r"(idle_rsp)
                   : "memory");
  // never reaches here
}
