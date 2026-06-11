#include "kernel/trap.h"
#include "kernel/proc.h"
#include "kernel/spinlock.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "arch/x64/apic.h"
#include "kernel/serial.h"
#include "kernel/fb.h"
#include "common/errno.h"

#define HEAP_START 0x600000

// ===================== fb_lock (framebuffer cursor + buffer) =====================
spinlock_t fb_lock = {0};

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
      __atomic_load_n(&irq_owner[tf->trapno], __ATOMIC_ACQUIRE) >= 0) {
    pid_t owner_pid = __atomic_load_n(&irq_owner[tf->trapno], __ATOMIC_ACQUIRE);
    // Wake up the bound user-space driver process
    for (int i = 0; i < MAX_PROC; i++) {
      if (procs[i].pid == owner_pid &&
          procs[i].state == BLOCKED &&
          procs[i].wait_event == WAIT_NOTIFY) {
        int target_cpu = procs[i].assigned_cpu;
        spin_lock(&cpu_locals[target_cpu].scheduler_lock);
        procs[i].state = READY;
        procs[i].wait_event = WAIT_NONE;
        list_push_back(&cpu_locals[target_cpu].run_queue, &procs[i].run_node);
        cpu_locals[target_cpu].run_count++;
        spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
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
  if (tf->cs == 0x2B) {   // from user mode
    schedule();
  }
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
}

// ===================== Syscall dispatch =====================
#define NR_SYSCALL 8
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_putc,      // 0: 输出字符
    sys_getpid,    // 1: 获取 PID
    sys_yield,     // 2: 主动让出 CPU
    sys_getc,      // 3: 读键盘输入（阻塞）
    sys_wait,      // 4: 阻塞等待通知
    sys_notify,    // 5: 唤醒指定进程
    sys_irq_bind,  // 6: 绑定当前进程到指定 IRQ
    sys_sbrk,      // 7: 扩展用户态堆
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->rax < NR_SYSCALL) {
        tf->rax = syscall_table[tf->rax](
            tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8);
    } else {
        tf->rax = (uint64_t)-ENOSYS;
    }
}

// sys_putc(char c) — syscall 0
uint64_t sys_putc(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    spin_lock(&fb_lock);
    fb_putc((char)arg1, 0xFFFFFF);
    spin_unlock(&fb_lock);
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
    return (uint64_t)-EPERM;
}

// sys_wait() — syscall 4 (阻塞等待通知)
uint64_t sys_wait(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    current_proc->state = BLOCKED;
    current_proc->wait_event = WAIT_NOTIFY;
    __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
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
            int target_cpu = procs[i].assigned_cpu;
            spin_lock(&cpu_locals[target_cpu].scheduler_lock);
            procs[i].state = READY;
            procs[i].wait_event = WAIT_NONE;
            list_push_back(&cpu_locals[target_cpu].run_queue, &procs[i].run_node);
            cpu_locals[target_cpu].run_count++;
            spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
            break;
        }
    }
    return 0;
}

// sys_irq_bind(irq) — syscall 6 (绑定当前进程到指定 IRQ)
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)-EINVAL;
    __atomic_store_n(&irq_owner[irq], current_proc->pid, __ATOMIC_RELEASE);
    return 0;
}

// sys_sbrk(increment) — syscall 7 (扩展/缩小用户态堆)
uint64_t sys_sbrk(uint64_t increment, uint64_t, uint64_t, uint64_t, uint64_t) {
    uint64_t old_brk = current_proc->brk;

    if (increment == 0)
        return old_brk;

    uint64_t new_brk;
    if ((int64_t)increment > 0) {
        new_brk = old_brk + increment;

        // 需要映射的页范围：[old_brk 向上取整, new_brk 向上取整)
        uint64_t page_start = ALIGN_UP(old_brk, PAGE_SIZE);
        uint64_t page_end   = ALIGN_UP(new_brk, PAGE_SIZE);

        if (page_start < page_end) {
            int pages_mapped = 0;
            uint64_t *pml4 = (uint64_t *)phys_to_virt(current_proc->cr3);
            uint64_t flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

            if (!map_user_pages(pml4, page_start, page_end, flags, &pages_mapped)) {
                if (pages_mapped > 0)
                    unmap_user_pages(pml4, page_start, page_start + pages_mapped * PAGE_SIZE, pages_mapped);
                return (uint64_t)-ENOMEM;
            }
        }
    } else {
        // 缩小堆
        uint64_t dec = (uint64_t)(-(int64_t)increment);
        if (dec >= old_brk - HEAP_START)
            return (uint64_t)-EINVAL;
        new_brk = old_brk - dec;

        // 需要解映射的页范围：[new_brk 向上取整, old_brk 向上取整)
        uint64_t page_start = ALIGN_UP(new_brk, PAGE_SIZE);
        uint64_t page_end   = ALIGN_UP(old_brk, PAGE_SIZE);

        if (page_start < page_end) {
            uint64_t *pml4 = (uint64_t *)phys_to_virt(current_proc->cr3);
            int count = (page_end - page_start) / PAGE_SIZE;
            unmap_user_pages(pml4, page_start, page_end, count);
        }
    }

    current_proc->brk = new_brk;
    return old_brk;
}
