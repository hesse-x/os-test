#ifndef ARCH_X86_TRAP_H
#define ARCH_X86_TRAP_H

#include <stdint.h>
#include <stddef.h>
#include "arch/x86/utils.h"

#define IDT_ENTRIES 256

// ===================== IDT =====================
typedef struct {
  uint16_t low_offset;
  uint16_t sel;
  uint8_t always0;
  uint8_t flags;
  uint16_t high_offset;
} __attribute__((packed)) idt_gate_t;

typedef struct {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed)) idt_register_t;

// ===================== Trapframe =====================
typedef struct {
  uint32_t edi;
  uint32_t esi;
  uint32_t ebp;
  uint32_t esp_ignored;
  uint32_t ebx;
  uint32_t edx;
  uint32_t ecx;
  uint32_t eax;
} pushregs_t;

typedef struct {
  pushregs_t regs;
  uint32_t gs;
  uint32_t fs;
  uint32_t es;
  uint32_t ds;
  uint32_t trapno;
  uint32_t err_code;
  uint32_t eip;
  uint32_t cs;
  uint32_t eflags;
  uint32_t esp;
  uint32_t ss;
} trapframe_t;

// ===================== Export interface =====================
extern "C" {
void set_idt_gate(int n, uint32_t handler, uint8_t flags = 0x8E);
void set_idt();
void idt_install();
void pic_remap();
void pit_init();
}

#endif // ARCH_X86_TRAP_H
