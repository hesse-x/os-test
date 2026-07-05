/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/log.h"
#include "arch/x64/smp.h" // cpu_locals, get_cpu_local, cur_tf
#include "arch/x64/trap.h"
#include "arch/x64/utils.h" // halt()
#include "kernel/xcore/serial_hook.h"
#include "kernel/xcore/trap.h" // syscall_name()
#include <stdarg.h>
#include <stdint.h>

#ifdef LOG_LEVEL_DEBUG
int log_level = LOG_DEBUG;
#else
int log_level = LOG_INFO;
#endif

static const char *level_tags[] = {"DEBUG", "INFO", "WARN", "ERROR", "PANIC"};

void printk(int level, const char *fmt, ...) {
  if (level < log_level && level != LOG_PANIC)
    return;

  serial_printf("[%s] ", level < 5 ? level_tags[level] : "???");

  va_list ap;
  va_start(ap, fmt);
  serial_vprintf(fmt, ap);
  va_end(ap);
}

void panic(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  serial_vprintf(fmt, ap);
  va_end(ap);

  printk(LOG_PANIC, "--- PANIC ---");

  // Print current syscall name if in syscall context
  trapframe_t *tf = get_cpu_local()->cur_tf;
  if (tf) {
    serial_printf("\nCPU %d  syscall=%s(%lu)\n", get_cpu_local()->cpu_id,
                  syscall_name(tf->rax), (unsigned long)tf->rax);
  } else {
    serial_printf("\nCPU %d  (no trapframe)\n", get_cpu_local()->cpu_id);
  }

  dump_stack_trace();

  for (;;) {
    halt();
  }
}

void dump_stack_trace(void) {
  serial_printf("BACKTRACE:\n");
  uint64_t *rbp;
  __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
  for (int depth = 0; depth < 16; depth++) {
    if (!rbp || (uint64_t)rbp < 0xFFFFFFFF80000000)
      break;
    uint64_t ret_addr = rbp[1];
    serial_printf("    0x%016X\n", ret_addr);
    rbp = (uint64_t *)rbp[0];
    if (!rbp)
      break;
  }
}
