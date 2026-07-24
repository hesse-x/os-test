/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/log.h"
#include "arch/x64/memlayout.h" // KERNEL_VMA_BOUNDARY
#include "arch/x64/smp.h"       // cpu_locals, get_cpu_local, cur_tf
#include "arch/x64/trap.h"
#include "arch/x64/utils.h" // halt()
#include "kernel/xcore/serial_hook.h"
#include "kernel/xcore/trap.h" // syscall_name()
#include "utils/kvformat.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef LOG_LEVEL_DEBUG
int log_level = LOG_DEBUG;
#else
int log_level = LOG_INFO;
#endif

static const char *level_tags[] = {"DEBUG", "INFO", "WARN", "ERROR", "PANIC"};

static void dump_stack_trace_locked(void);

// (frame_opt.md 块三) Decrement the printk re-entry depth on any return from
// printk's body — paired with the increment below.  A cleanup attribute keeps
// it correct even if serial output faults mid-line.
static void printk_depth_leave(void *arg) {
  (void)arg;
  get_cpu_local()->printk_depth--;
}

void printk(int level, const char *fmt, ...) {
  if (level < log_level && level != LOG_PANIC)
    return;

  // (frame_opt.md 块三) Re-entry guard.  trap_dispatch dumps a lot of state via
  // printk, and printk can itself fault (formatting a corrupt pointer, touching
  // an unmapped symbol).  We track nesting depth on this CPU: the outermost
  // printk (depth 0 → 1) prints normally; any re-entrant printk (depth already
  // ≥1) means we faulted inside a printk body, so emit only a bare marker and
  // bail — re-formatting would risk another fault and an unbounded
  // fault-during-fault cascade.  PANIC bypasses the guard so the panic message
  // is never suppressed.  (Previously trap_dispatch armed a flag before the
  // dump, which made every dump-time printk look "recursive" and suppressed
  // the banner itself — only a bare BACKTRACE survived.)
  cpu_local *cl = get_cpu_local();
  if (level != LOG_PANIC && cl->printk_depth >= 1) {
    uint64_t flags = serial_tx_acquire();
    serial_printf_locked("\n[recursive oops — printk re-entered]\n");
    serial_tx_release(flags);
    return;
  }

  cl->printk_depth++;
  int depth_guard __attribute__((cleanup(printk_depth_leave))) = 1;
  (void)depth_guard;

  // Hold the TX lock across tag+body so one printk line is never split by
  // output from another CPU.
  uint64_t flags = serial_tx_acquire();
  serial_printf_locked("[%s] ", level < 5 ? level_tags[level] : "???");
  va_list ap;
  va_start(ap, fmt);
  serial_vprintf_locked(fmt, ap);
  va_end(ap);
  serial_tx_release(flags);
}

void panic(const char *fmt, ...) {
  // Hold the TX lock across the whole dump so it stays contiguous.
  uint64_t flags = serial_tx_acquire();

  va_list ap;
  va_start(ap, fmt);
  serial_vprintf_locked(fmt, ap);
  va_end(ap);

  serial_printf_locked("[PANIC] --- PANIC ---");

  // Print current syscall name if in syscall context
  trapframe *tf = get_cpu_local()->cur_tf;
  if (tf) {
    serial_printf_locked("\nCPU %d  syscall=%s(%lu)\n", get_cpu_local()->cpu_id,
                         syscall_name(tf->rax), (unsigned long)tf->rax);
  } else {
    serial_printf_locked("\nCPU %d  (no trapframe)\n", get_cpu_local()->cpu_id);
  }

  dump_stack_trace_locked();

  serial_tx_release(flags);

  for (;;) {
    halt();
  }
}

static void dump_stack_trace_locked(void) {
  serial_printf_locked("BACKTRACE:\n");
  uint64_t *rbp;
  __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
  for (int depth = 0; depth < 16; depth++) {
    if (!rbp || (uint64_t)rbp < KERNEL_VMA_BOUNDARY)
      break;
    uint64_t ret_addr = rbp[1];
    serial_printf_locked("    0x%016lX\n", ret_addr);
    rbp = (uint64_t *)rbp[0];
    if (!rbp)
      break;
  }
}

void dump_stack_trace(void) {
  uint64_t flags = serial_tx_acquire();
  dump_stack_trace_locked();
  serial_tx_release(flags);
}

// ===================== snprintf (backed by kvformat) =====================
struct snprintf_arg {
  char *buf;
  size_t pos;
  size_t cap;
};

static void snprintf_putc(char c, void *arg) {
  struct snprintf_arg *a = (struct snprintf_arg *)arg;
  if (a->pos + 1 < a->cap)
    a->buf[a->pos++] = c;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  struct snprintf_arg a = {buf, 0, size};
  int n = kvformat(snprintf_putc, &a, fmt, ap);
  if (size > 0) {
    if (a.pos < size)
      buf[a.pos] = '\0';
    else
      buf[size - 1] = '\0';
  }
  return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
