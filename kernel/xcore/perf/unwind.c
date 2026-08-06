/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/unwind.h"

#ifdef PERF

#include <stdbool.h>
#include <stddef.h>

#include <xos/perf.h>

#include "kernel/xcore/perf/sample.h"
#include "kernel/xcore/sparse.h"

#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/xtask.h"

extern uint8_t __text_start[] __attribute__((visibility("hidden")));
extern uint8_t __text_end[] __attribute__((visibility("hidden")));

static bool canonical(uint64_t address) {
  uint64_t high = address >> 48;
  return high == 0 || high == 0xffff;
}

static bool kernel_stack_address(uint64_t address, size_t bytes) {
  cpu_local *local = get_cpu_local();
  xtask *task = current_task;
  if (task && address >= task->k_stack_top - KERNEL_STACK_SIZE &&
      address + bytes >= address && address + bytes <= task->k_stack_top)
    return true;
  if (local->irq_stack_top &&
      address >= local->irq_stack_top - IRQ_STACK_BYTES &&
      address + bytes >= address && address + bytes <= local->irq_stack_top)
    return true;
  uint64_t nmi_top = per_cpu_ist_stack[local->cpu_id][0];
  return nmi_top && address >= nmi_top - 4096 && address + bytes >= address &&
         address + bytes <= nmi_top;
}

static bool kernel_text(uint64_t address) {
  return address >= (uint64_t)__text_start && address < (uint64_t)__text_end;
}

static bool user_translate(uint64_t address, bool executable,
                           uint64_t *physical) {
  xtask *task = current_task;
  if (!task || address >= KERNEL_VMA_BOUNDARY || !canonical(address))
    return false;
  uint64_t *table = (uint64_t *)phys_to_virt(
      (__force phys_addr_t)(task->cr3 & PTE_PHYS_MASK));
  const unsigned shifts[] = {39, 30, 21};
  for (unsigned level = 0; level < 3; level++) {
    uint64_t entry = table[(address >> shifts[level]) & 0x1ffU];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_PS))
      return false;
    table =
        (uint64_t *)phys_to_virt((__force phys_addr_t)(entry & PTE_PHYS_MASK));
  }
  uint64_t pte = table[(address >> 12) & 0x1ffU];
  if ((pte & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
      (executable && (pte & PTE_NX)))
    return false;
  *physical = (pte & PTE_PHYS_MASK) | (address & 0xfffU);
  return true;
}

static bool user_read64(uint64_t address, uint64_t *value) {
  if ((address & 7U) != 0 || (address & 0xfffU) > 0xff8U)
    return false;
  uint64_t physical;
  if (!user_translate(address, false, &physical))
    return false;
  *value = *(const uint64_t *)(__force uintptr_t)phys_to_virt(
      (__force phys_addr_t)physical);
  return true;
}

uint8_t perf_unwind_trapframe(const trapframe *tf, uint64_t *frames,
                              uint8_t *stop_reason) {
  *stop_reason = XOS_PERF_UNWIND_COMPLETE;
  if (!tf || !canonical(tf->rip)) {
    *stop_reason = XOS_PERF_UNWIND_BAD_FRAME;
    return 0;
  }
  bool user = (tf->cs & 3U) == 3U;
  if ((!user && !kernel_text(tf->rip)) ||
      (user && !user_translate(tf->rip, true, &(uint64_t){0}))) {
    *stop_reason = XOS_PERF_UNWIND_BAD_RETURN;
    return 0;
  }

  uint8_t depth = 0;
  frames[depth++] = tf->rip;
  uint64_t frame = tf->rbp;
  while (frame != 0 && depth < PERF_CALLCHAIN_MAX_DEPTH) {
    if ((frame & 7U) != 0 || !canonical(frame)) {
      *stop_reason = XOS_PERF_UNWIND_BAD_FRAME;
      break;
    }
    uint64_t next;
    uint64_t ret;
    if (user) {
      if (!user_read64(frame, &next) || !user_read64(frame + 8, &ret)) {
        *stop_reason = XOS_PERF_UNWIND_UNMAPPED;
        break;
      }
      uint64_t ignored;
      if (!user_translate(ret, true, &ignored)) {
        *stop_reason = XOS_PERF_UNWIND_BAD_RETURN;
        break;
      }
    } else {
      if (!kernel_stack_address(frame, 16)) {
        *stop_reason = XOS_PERF_UNWIND_BAD_FRAME;
        break;
      }
      next = *(const uint64_t *)(uintptr_t)frame;
      ret = *(const uint64_t *)(uintptr_t)(frame + 8);
      if (!kernel_text(ret)) {
        *stop_reason = XOS_PERF_UNWIND_BAD_RETURN;
        break;
      }
    }
    frames[depth++] = ret;
    if (next == 0)
      break;
    if (next <= frame || next - frame > KERNEL_STACK_SIZE) {
      *stop_reason = XOS_PERF_UNWIND_BAD_FRAME;
      break;
    }
    frame = next;
  }
  if (depth == PERF_CALLCHAIN_MAX_DEPTH && frame != 0)
    *stop_reason = XOS_PERF_UNWIND_DEPTH;
  return depth;
}

#endif
