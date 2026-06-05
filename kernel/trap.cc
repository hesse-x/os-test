#include "kernel/trap.h"
#include "kernel/proc.h"
#include "arch/x86/utils.h"
#include "arch/x86/paging.h"
#include "arch/x86/trap.h"
#include "kernel/serial.h"
#include "driver/kbd.h"
#include "driver/fb.h"

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
  // Syscall vector 128
  if (tf->trapno == 128) {
    syscall_dispatch(tf);
    return;
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
  schedule();
}

// Keyboard IRQ handler: buffer + wakeup blocked processes
static void keyboard_handler(trapframe_t *tf) {
  kbd_handle();
  // Wake up process waiting for keyboard
  for (int i = 0; i < MAX_PROC; i++) {
    if (procs[i].pid >= 0 && procs[i].state == BLOCKED &&
        procs[i].wait_event == WAIT_KBD) {
      procs[i].state = READY;
      procs[i].wait_event = WAIT_NONE;
      break;  // only wake one
    }
  }
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

// ===================== Syscall dispatch =====================
typedef uint32_t (*syscall_fn_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

#define NR_SYSCALL 4
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_putc,    // 0: 输出字符
    sys_getpid,  // 1: 获取 PID
    sys_yield,   // 2: 主动让出 CPU
    sys_getc,    // 3: 读键盘输入（阻塞）
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->regs.eax < NR_SYSCALL) {
        tf->regs.eax = syscall_table[tf->regs.eax](
            tf->regs.ebx, tf->regs.ecx, tf->regs.edx,
            tf->regs.esi, tf->regs.edi);
    } else {
        tf->regs.eax = (uint32_t)-1;  // 无效 syscall 号
    }
}

// sys_putc(char c) — syscall 0
uint32_t sys_putc(uint32_t arg1, uint32_t, uint32_t, uint32_t, uint32_t) {
    fb_putc((char)arg1, 0xFFFFFF);
    serial_putc((char)arg1);
    return 0;
}

// sys_getpid() — syscall 1
uint32_t sys_getpid(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    return (uint32_t)current_proc->pid;
}

// sys_yield() — syscall 2
uint32_t sys_yield(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    schedule();
    return 0;
}

// sys_getc() — syscall 3 (阻塞等待键盘输入)
uint32_t sys_getc(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    if (kbd_buffer_empty()) {
        current_proc->state = BLOCKED;
        current_proc->wait_event = WAIT_KBD;
        schedule();
    }
    return (uint32_t)kbd_buffer_pop();
}
