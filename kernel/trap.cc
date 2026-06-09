#include "kernel/trap.h"
#include "kernel/proc.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "arch/x64/apic.h"
#include "kernel/serial.h"
#include "kernel/fb.h"

// ===================== IRQ handler registry =====================
#define MAX_IRQ_HANDLERS 48
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS];

// ===================== IRQ owner (user-space driver binding) =====================
static pid_t irq_owner[MAX_IRQ_HANDLERS];

void register_irq(int vec, irq_handler_t fn) {
  if (vec >= 0 && vec < MAX_IRQ_HANDLERS) {
    irq_handlers[vec] = fn;
  }
}

// ===================== Trap dispatch =====================
static uint64_t tick = 0;

void trap_dispatch(trapframe_t *tf) {
  // Hardware IRQ: check user-space driver binding first
  if (tf->trapno >= 32 && tf->trapno < MAX_IRQ_HANDLERS &&
      irq_owner[tf->trapno] >= 0) {
    // Wake up the bound user-space driver process
    for (int i = 0; i < MAX_PROC; i++) {
      if (procs[i].pid == irq_owner[tf->trapno] &&
          procs[i].state == BLOCKED &&
          procs[i].wait_event == WAIT_NOTIFY) {
        procs[i].state = READY;
        procs[i].wait_event = WAIT_NONE;
        break;
      }
    }
    lapic_eoi();
    return;
  }

  // Check registered handler
  if (tf->trapno < MAX_IRQ_HANDLERS &&
      irq_handlers[tf->trapno] != nullptr) {
    irq_handlers[tf->trapno](tf);
    return;
  }

  // Default: timer EOI
  if (tf->trapno == 32) {
    tick++;
    lapic_eoi();
    return;
  }

  // Other hardware IRQ: send EOI
  if (tf->trapno >= 32 && tf->trapno <= 47) {
    lapic_eoi();
    return;
  }

  // CPU exception: print diagnostic and halt
  if (tf->trapno == 14) {
    uint64_t cr2;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
    serial_puts("PAGE FAULT: fault addr=");
    serial_put_hex(cr2);
  } else if (tf->trapno == 6) {
    serial_puts("UNDEFINED OPCODE");
  } else {
    serial_puts("EXCEPTION: vector ");
    serial_put_hex(tf->trapno);
  }
  serial_puts("\n  rip=");
  serial_put_hex(tf->rip);
  serial_puts(" cs=");
  serial_put_hex(tf->cs);
  serial_puts(" rfl=");
  serial_put_hex(tf->rflags);
  serial_puts(" rsp=");
  serial_put_hex(tf->rsp);
  serial_puts(" ss=");
  serial_put_hex(tf->ss);
  serial_puts("\n  rax=");
  serial_put_hex(tf->rax);
  serial_puts(" rbx=");
  serial_put_hex(tf->rbx);
  serial_puts(" rcx=");
  serial_put_hex(tf->rcx);
  serial_puts(" rdx=");
  serial_put_hex(tf->rdx);
  serial_puts("\n  rsi=");
  serial_put_hex(tf->rsi);
  serial_puts(" rdi=");
  serial_put_hex(tf->rdi);
  serial_puts(" rbp=");
  serial_put_hex(tf->rbp);
  serial_puts(" r08=");
  serial_put_hex(tf->r8);
  serial_puts("\n  r09=");
  serial_put_hex(tf->r9);
  serial_puts(" r10=");
  serial_put_hex(tf->r10);
  serial_puts(" r11=");
  serial_put_hex(tf->r11);
  serial_puts(" r12=");
  serial_put_hex(tf->r12);
  serial_puts("\n  r13=");
  serial_put_hex(tf->r13);
  serial_puts(" r14=");
  serial_put_hex(tf->r14);
  serial_puts(" r15=");
  serial_put_hex(tf->r15);
  serial_puts(" err=");
  serial_put_hex(tf->err_code);
  serial_puts("\n  cr3=");
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  serial_put_hex(cr3);
  // Print current process info and CR3 comparison
  if (current_proc) {
    serial_puts(" pid=");
    serial_put_hex((uint64_t)current_proc->pid);
    serial_puts(" proc_cr3=");
    serial_put_hex(current_proc->cr3);
  }
  // Stack dump (8 qwords at rsp)
  serial_puts("\n  stack:");
  uint64_t *sp = (uint64_t *)tf->rsp;
  for (int i = 0; i < 8; i++) {
    serial_puts(" ");
    serial_put_hex(sp[i]);
  }
  serial_puts("\n");
  halt();
}

// ===================== Timer IRQ handler =====================
static void timer_handler(trapframe_t *tf) {
  tick++;
  lapic_eoi();
  schedule();
}

// Keyboard IRQ handler: wake up bound user-space driver (handled by irq_owner above)
// No kernel keyboard ISR needed — kbd_driver process handles it.

void isr_init() {
  // Initialize IRQ owner table
  for (int i = 0; i < MAX_IRQ_HANDLERS; i++) {
    irq_owner[i] = -1;
  }

  // Register default handlers
  register_irq(32, timer_handler);

  // Re-initialize GDT with per-CPU setup (now running at virtual address)
  smp_init_cpu(0, 0, (uint64_t)&stack_bottom + 8192);
  smp_apply_cpu(0);

  // Enable NX bit (CR4.NXDE + EFER.NXE) before IDT install
  enable_nx();

  idt_install();
  setup_syscall();
  apic_init();

  sti();
}

// ===================== Syscall dispatch =====================
#define NR_SYSCALL 7
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_putc,      // 0: 输出字符
    sys_getpid,    // 1: 获取 PID
    sys_yield,     // 2: 主动让出 CPU
    sys_getc,      // 3: 读键盘输入（阻塞）
    sys_wait,      // 4: 阻塞等待通知
    sys_notify,    // 5: 唤醒指定进程
    sys_irq_bind,  // 6: 绑定当前进程到指定 IRQ
};

void syscall_dispatch(trapframe_t *tf) {
    serial_puts("syscall: ");
    serial_put_hex(tf->rax);
    serial_putc('\n');
    if (tf->rax < NR_SYSCALL) {
        tf->rax = syscall_table[tf->rax](
            tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8);
    } else {
        tf->rax = (uint64_t)-1;
    }
}

// sys_putc(char c) — syscall 0
uint64_t sys_putc(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    fb_putc((char)arg1, 0xFFFFFF);
    serial_putc((char)arg1);
    return 0;
}

// sys_getpid() — syscall 1
uint64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return (uint64_t)current_proc->pid;
}

// sys_yield() — syscall 2
uint64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    schedule();
    return 0;
}

// sys_getc() — syscall 3 (deprecated: keyboard now handled by user-space driver)
uint64_t sys_getc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return (uint64_t)-1;
}

// sys_wait() — syscall 4 (阻塞等待通知)
uint64_t sys_wait(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    serial_puts("wait pid=");
    serial_put_hex((uint64_t)current_proc->pid);
    serial_puts("\n");
    current_proc->state = BLOCKED;
    current_proc->wait_event = WAIT_NOTIFY;
    schedule();
    return 0;
}

// sys_notify(pid) — syscall 5 (唤醒指定进程)
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    pid_t target_pid = (pid_t)arg1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid == target_pid &&
            procs[i].state == BLOCKED &&
            procs[i].wait_event == WAIT_NOTIFY) {
            procs[i].state = READY;
            procs[i].wait_event = WAIT_NONE;
            break;
        }
    }
    return 0;
}

// sys_irq_bind(irq) — syscall 6 (绑定当前进程到指定 IRQ)
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)-1;
    irq_owner[irq] = current_proc->pid;
    return 0;
}
