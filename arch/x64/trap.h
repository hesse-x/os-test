/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_TRAP_H
#define ARCH_X64_TRAP_H

#include "arch/x64/utils.h"
#include <stddef.h>
#include <stdint.h>

#define IDT_ENTRIES 256

// ===================== IDT (16-byte gate for 64-bit) =====================
typedef struct idt_gate {
  uint16_t offset_low;  // offset[15:0]
  uint16_t sel;         // code segment selector
  uint8_t ist;          // IST offset (0 = don't use IST)
  uint8_t flags;        // 0x8E = interrupt gate, 0xEE = user interrupt
  uint16_t offset_mid;  // offset[31:16]
  uint32_t offset_high; // offset[63:32]
  uint32_t reserved;    // must be 0
} __attribute__((packed)) idt_gate_t;

typedef struct idt_register {
  uint16_t limit;
  uint16_t base_low;
  uint32_t base_high;
  uint32_t base_upper;
  uint32_t reserved;
} __attribute__((packed)) idt_register_t;

// ===================== Trapframe (64-bit) =====================
typedef struct trapframe {
  // __alltraps pushed manually (high address to low)
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
  uint64_t trapno;
  uint64_t err_code;

  // CPU-pushed by interrupt (popped by iretq)
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
} trapframe_t;

// ===================== Export interface =====================
void set_idt_gate(int n, uint64_t handler, uint8_t flags, uint8_t ist);
void set_idt();
void idt_install();
void setup_syscall();

#endif // ARCH_X64_TRAP_H
