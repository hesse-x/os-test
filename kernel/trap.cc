#include "kernel/trap.h"
#include "arch/x86/utils.h"
#include "arch/x86/paging.h"
#include "arch/x86/trap.h"
#include "kernel/serial.h"
#include "driver/kbd.h"

// ===================== IRQ handler registry =====================
#define MAX_IRQ_HANDLERS 48
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS];

void register_irq(int vec, irq_handler_t fn) {
  if (vec >= 0 && vec < MAX_IRQ_HANDLERS) {
    irq_handlers[vec] = fn;
  }
}

// ===================== Trap dispatch =====================
static uint32_t tick = 0;

void trap_dispatch(trapframe_t *tf) {
  // #PF from ring 3: verify ring 3 switch succeeded
  if (tf->trapno == 14 && (tf->cs & 0x3) == 3) {
    serial_puts("Ring 3 switch verified! #PF at EIP=");
    serial_put_hex(tf->eip);
    serial_puts("\n");
    while (1) __asm__ volatile("hlt");
  }

  // Check registered handler first
  if (tf->trapno >= 0 && tf->trapno < MAX_IRQ_HANDLERS &&
      irq_handlers[tf->trapno] != nullptr) {
    irq_handlers[tf->trapno](tf);
    return;
  }

  // Default: timer EOI
  if (tf->trapno == 32) {
    tick++;
    outb(0x20, 0x20);
    return;
  }

  // Other hardware IRQ: send EOI
  if (tf->trapno >= 32 && tf->trapno <= 47) {
    if (tf->trapno >= 40)
      outb(0xA0, 0x20);
    outb(0x20, 0x20);
    return;
  }

  // CPU exception: print diagnostic and halt
  serial_puts("EXCEPTION: vector ");
  serial_put_hex(tf->trapno);
  serial_puts(" err ");
  serial_put_hex(tf->err_code);
  serial_puts(" eip ");
  serial_put_hex(tf->eip);
  serial_puts("\n");
  __asm__ volatile("cli; hlt");
}

// ===================== Timer IRQ handler =====================
static void timer_handler(trapframe_t *tf) {
  tick++;
  outb(0x20, 0x20); /* EOI */
}

// Keyboard IRQ handler wrapper
static void keyboard_handler(trapframe_t *tf) {
  kbd_handle();
  outb(0x20, 0x20); /* EOI */
}

void isr_init() {
  // Register default handlers
  register_irq(32, timer_handler);
  register_irq(33, keyboard_handler);

  gdt_init();
  idt_install();
  pic_remap();
  pit_init();
  kbd_init();
  __asm__ volatile("sti");
}

// Stub: syscall dispatch (int 0x80)
void syscall_dispatch(trapframe_t *tf) {
  serial_puts("syscall: vector ");
  serial_put_hex(tf->trapno);
  serial_puts("\n");
}
