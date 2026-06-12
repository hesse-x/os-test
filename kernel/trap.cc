#include "kernel/trap.h"
#include "kernel/proc.h"
#include "kernel/spinlock.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "arch/x64/apic.h"
#include "kernel/serial.h"
#include "kernel/mem/slab.h"
#include "common/errno.h"
#include "kernel/fb.h"

#define HEAP_START 0x600000

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
    // Direct index by PID — no scan needed
    if (owner_pid >= 0 && owner_pid < MAX_PROC) {
      proc_t *target = &procs[owner_pid];
      int target_cpu = target->assigned_cpu;
      spin_lock(&cpu_locals[target_cpu].scheduler_lock);
      if (target->pid == owner_pid &&
          target->state == BLOCKED &&
          target->wait_event == WAIT_NOTIFY) {
        target->state = READY;
        target->wait_event = WAIT_NONE;
        list_push_back(&cpu_locals[target_cpu].run_queue, &target->run_node);
        cpu_locals[target_cpu].run_count++;
      }
      spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
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

  // CPU exception: kill user process, halt for kernel exceptions
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
  if (current_proc) {
    serial_puts(" pid=");
    serial_put_hex((uint64_t)current_proc->pid);
    serial_puts(" proc_cr3=");
    serial_put_hex(current_proc->cr3);
  }
  serial_puts("\n");

  // 栈回溯：遍历 RBP 链（需 -fno-omit-frame-pointer）
  serial_puts("BACKTRACE:\n");
  uint64_t *rbp = (uint64_t *)tf->rbp;
  for (int i = 0; i < 16 && (uint64_t)rbp >= 0xFFFFFFFF80000000ULL; i++) {
    uint64_t ret_addr = rbp[1];
    serial_puts("  #");
    serial_put_hex((uint64_t)i);
    serial_puts(" ");
    serial_put_hex(ret_addr);
    serial_puts("\n");
    rbp = (uint64_t *)rbp[0];
    if (!rbp) break;
  }

  if (tf->cs == 0x2B) {
    // User-mode exception: kill process, don't halt the machine
    serial_puts("Process ");
    serial_put_hex((uint64_t)current_proc->pid);
    serial_puts(" crashed: vector ");
    serial_put_hex(tf->trapno);
    serial_puts("\n");
    sys_exit((uint64_t)-1, 0, 0, 0, 0);
    // sys_exit does not return
  }
  // Kernel-mode exception: unrecoverable, halt
  halt();
}

// ===================== Timer IRQ handler =====================
static void timer_handler(trapframe_t *tf) {
  tick++;
  lapic_eoi();

  // Check timer queue for expired deadlines
  int cpu = get_cpu_local()->cpu_id;
  uint64_t now = sched_clock();
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  list_node_t *head = &cpu_locals[cpu].timer_queue;
  while (!list_empty(head)) {
    proc_t *p = LIST_ENTRY(list_front(head), proc_t, wait_node);
    if (p->wait_deadline > now) break;  // sorted, stop at first unexpired
    list_remove(&p->wait_node);
    if (p->state == BLOCKED) {
      p->state = READY;
      p->wait_event = WAIT_NONE;
      p->wait_timed_out = 1;
      p->wait_deadline = 0;
      list_push_back(&cpu_locals[cpu].run_queue, &p->run_node);
      cpu_locals[cpu].run_count++;
    }
  }
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

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
#define NR_SYSCALL 17
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    nullptr,            // 0: sys_putc removed (returns -ENOSYS)
    sys_getpid,         // 1: 获取 PID
    sys_yield,          // 2: 主动让出 CPU
    sys_getc,           // 3: 读键盘输入（废弃）
    sys_wait,           // 4: 阻塞等待通知（带超时）
    sys_notify,         // 5: 唤醒指定进程
    sys_irq_bind,       // 6: 绑定当前进程到指定 IRQ
    sys_sbrk,           // 7: 扩展用户态堆
    sys_exit,           // 8: 进程退出
    sys_waitpid,        // 9: 等待子进程退出
    sys_spawn,          // 10: 创建子进程
    sys_mmap,           // 11: 匿名内存映射
    sys_munmap,         // 12: 解除内存映射
    sys_serial_write,   // 13: 串口输出
    sys_fb_info,        // 14: 获取 framebuffer 信息
    sys_shm_create,     // 15: 创建共享内存
    sys_shm_attach,     // 16: 附加共享内存
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->rax < NR_SYSCALL && syscall_table[tf->rax] != nullptr) {
        tf->rax = syscall_table[tf->rax](
            tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8);
    } else {
        tf->rax = (uint64_t)ENOSYS;
    }
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
    return (uint64_t)EPERM;
}

// sys_wait(timeout_ms) — syscall 4 (阻塞等待通知，可选超时)
// timeout_ms=0: 无限等待; >0: 超时后唤醒
// 返回: 0=notify 唤醒, 1=超时唤醒
uint64_t sys_wait(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    uint32_t timeout_ms = (uint32_t)arg1;

    current_proc->state = BLOCKED;
    current_proc->wait_event = WAIT_NOTIFY;
    current_proc->wait_timed_out = 0;

    if (timeout_ms > 0) {
        current_proc->wait_deadline = sched_clock() + (uint64_t)timeout_ms * 1000000ULL;
        int cpu = current_proc->assigned_cpu;
        spin_lock(&cpu_locals[cpu].scheduler_lock);
        timer_queue_insert(cpu, current_proc);
        spin_unlock(&cpu_locals[cpu].scheduler_lock);
    } else {
        current_proc->wait_deadline = 0;
    }

    __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
    schedule();
    return (uint64_t)current_proc->wait_timed_out;
}

// sys_notify(pid) — syscall 5 (唤醒指定进程)
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    pid_t target_pid = (pid_t)arg1;
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)EINVAL;

    proc_t *target = &procs[target_pid];
    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    // Verify slot is still valid and process is BLOCKED
    if (target->pid == target_pid &&
        target->state == BLOCKED &&
        (target->wait_event == WAIT_NOTIFY ||
         target->wait_event == WAIT_CHILD)) {
        // If process has a deadline, remove from timer queue
        if (target->wait_deadline != 0) {
            timer_queue_remove(target);
            target->wait_deadline = 0;
        }
        target->state = READY;
        target->wait_event = WAIT_NONE;
        target->wait_timed_out = 0;
        list_push_back(&cpu_locals[target_cpu].run_queue, &target->run_node);
        cpu_locals[target_cpu].run_count++;
    }
    spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
    return 0;
}

// sys_irq_bind(irq) — syscall 6 (绑定当前进程到指定 IRQ)
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)EINVAL;
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
                return 0;  // ENOMEM
            }
        }
    } else {
        // 缩小堆
        uint64_t dec = (uint64_t)(-(int64_t)increment);
        if (dec >= old_brk - HEAP_START)
            return 0;  // EINVAL: cannot shrink below HEAP_START
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

// sys_exit(exit_code) — syscall 8 (进程退出)
uint64_t sys_exit(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    proc_t *proc = current_proc;
    int32_t exit_code = (int32_t)arg1;
    proc->exit_code = exit_code;

    if (proc->parent_pid < 0) {
        // No parent: directly reap all resources
        proc_reap(proc);
    } else {
        // Has parent: become ZOMBIE, wait for sys_waitpid to reap
        int cpu = proc->assigned_cpu;
        uint64_t flags;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
        proc->state = ZOMBIE;
        __atomic_add_fetch(&cpu_locals[cpu].run_count, -1, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
        // Notify parent so it can call waitpid
        sys_notify((uint64_t)proc->parent_pid, 0, 0, 0, 0);
    }

    schedule();  // never returns
    return 0;    // unreachable
}

// sys_waitpid(pid, exit_code_ptr) — syscall 9 (等待子进程退出)
uint64_t sys_waitpid(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    pid_t pid = (pid_t)arg1;
    int32_t *exit_code_ptr = (int32_t *)arg2;

    if (pid < 0 || pid >= MAX_PROC) return 0;  // EINVAL

    proc_t *child = &procs[pid];

    // Validate: pid must be our child (under procs_lock to prevent reap)
    spin_lock(&procs_lock);
    if (child->pid != pid || child->parent_pid != current_proc->pid) {
        spin_unlock(&procs_lock);
        return 0;  // ECHILD
    }
    spin_unlock(&procs_lock);

    while (1) {
        // Check if child is ZOMBIE under its scheduler_lock
        // Also atomically claim it with REAPING state to prevent races
        int cpu = child->assigned_cpu;
        uint64_t flags;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
        if (child->state == ZOMBIE) {
            child->state = REAPING;  // claimed by parent, scheduler ignores this
            spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

        // Child not yet exited: block on WAIT_CHILD
        current_proc->wait_event = WAIT_CHILD;
        current_proc->state = BLOCKED;
        __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
        schedule();

        // Woken up by notify — re-validate child still exists
        spin_lock(&procs_lock);
        if (child->pid != pid) {
            // Child was reaped by someone else — should not happen
            spin_unlock(&procs_lock);
            return 0;  // ECHILD
        }
        spin_unlock(&procs_lock);
    }

    // Reap child resources
    if (exit_code_ptr) {
        // Validate user pointer: must be in user canonical low half, not kernel space
        uint64_t ptr_val = (uint64_t)exit_code_ptr;
        if (ptr_val >= 0xFFFFFFFF80000000ULL || !ptr_val || (ptr_val + sizeof(int32_t) - 1) >= 0xFFFFFFFF80000000ULL)
            return 0;  // EFAULT
        *exit_code_ptr = child->exit_code;
    }
    proc_reap(child);
    return (uint64_t)pid;
}

// sys_spawn(elf_data, elf_size, iopl) — syscall 10 (创建子进程)
uint64_t sys_spawn(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    const uint8_t *elf_data_user = (const uint8_t *)arg1;
    uint64_t elf_size = arg2;
    uint32_t iopl = (uint32_t)arg3;

    // IOPL permission check
    if (current_proc->iopl < iopl) return (uint64_t)EPERM;

    // Basic parameter validation
    if (!elf_data_user || elf_size == 0 || elf_size > 256 * 1024)
        return (uint64_t)EINVAL;

    // Validate user pointer range
    uint64_t ptr_start = (uint64_t)elf_data_user;
    uint64_t ptr_end = ptr_start + elf_size;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    // Copy ELF data from user space to kernel buffer to prevent TOCTOU
    uint8_t *elf_buf = (uint8_t *)kmalloc(elf_size);
    if (!elf_buf) return (uint64_t)ENOMEM;
    __memcpy(elf_buf, elf_data_user, elf_size);

    // Create child process from ELF
    proc_t *child = process_create_elf(elf_buf, elf_size, iopl);
    kfree(elf_buf);
    if (!child) return (uint64_t)ENOMEM;

    child->parent_pid = current_proc->pid;
    return (uint64_t)child->pid;
}

// sys_mmap(size) — syscall 11 (匿名私有映射)
// Returns: mapped address on success, 0 on failure
uint64_t sys_mmap(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    size_t size = (size_t)arg1;
    if (size == 0) return 0;  // EINVAL

    size = ALIGN_UP(size, PAGE_SIZE);
    proc_t *proc = current_proc;
    uint64_t vaddr = proc->mmap_brk;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->cr3);
    uint64_t flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

    // 逐页分配物理页并映射
    size_t npages = size / PAGE_SIZE;
    uint64_t *phys_pages = (uint64_t *)kmalloc(npages * sizeof(uint64_t));
    if (!phys_pages) return 0;

    size_t mapped = 0;
    for (size_t i = 0; i < npages; i++) {
        Page *page = bfc_alloc.alloc_page(1);
        if (!page) {
            // 回滚
            for (size_t j = 0; j < mapped; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }
            kfree(phys_pages);
            return 0;
        }
        phys_pages[i] = page_to_phys(page);
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, phys_pages[i], flags)) {
            // 回滚
            bfc_alloc.free_page(&BFCAllocator::frames[PHY_TO_PAGE(phys_pages[i])], 1);
            for (size_t j = 0; j < mapped; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }
            kfree(phys_pages);
            return 0;
        }
        mapped++;
    }

    // 分配 mmap_region 节点记录
    mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!region) {
        // 回滚所有映射
        for (size_t i = 0; i < npages; i++) {
            uint64_t va = vaddr + i * PAGE_SIZE;
            unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
        }
        kfree(phys_pages);
        return 0;
    }

    region->vaddr = vaddr;
    region->size = size;
    region->next = proc->mmap_regions;
    proc->mmap_regions = region;
    proc->mmap_brk = vaddr + size;

    kfree(phys_pages);
    return vaddr;
}

// sys_munmap(addr, size) — syscall 12 (解除内存映射)
// Returns: 0 on success, positive errno on failure
uint64_t sys_munmap(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    uint64_t addr = arg1;
    size_t size = (size_t)arg2;

    if (size == 0) return (uint64_t)EINVAL;

    proc_t *proc = current_proc;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->cr3);

    // 查找匹配的 mmap_region
    mmap_region **pp = &proc->mmap_regions;
    while (*pp) {
        if ((*pp)->vaddr == addr) {
            mmap_region *region = *pp;
            size = region->size;

            // 逐页解映射 + 释放物理页
            size_t npages = size / PAGE_SIZE;
            for (size_t i = 0; i < npages; i++) {
                uint64_t va = addr + i * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }

            // 从链表删除
            *pp = region->next;
            kfree(region);
            return 0;
        }
        pp = &(*pp)->next;
    }

    return (uint64_t)EINVAL;
}

// sys_serial_write(buf, len) — syscall 13 (用户态串口输出)
uint64_t sys_serial_write(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    const char *buf = (const char *)arg1;
    size_t len = (size_t)arg2;

    if (!buf || len == 0) return 0;

    // Validate user pointer range
    uint64_t ptr_start = (uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    for (size_t i = 0; i < len; i++)
        serial_putc(buf[i]);

    return 0;
}

// sys_fb_info(buf) — syscall 14 (获取 framebuffer 信息)
// Returns: 0 on success, positive errno on failure
uint64_t sys_fb_info(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    void *user_buf = (void *)arg1;
    if (!user_buf) return (uint64_t)EINVAL;

    // Validate user pointer range
    uint64_t ptr = (uint64_t)user_buf;
    if (ptr >= 0xFFFFFFFF80000000ULL || ptr + sizeof(kms_fb_info) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    __memcpy(user_buf, &g_fb_info, sizeof(kms_fb_info));
    return 0;
}

// sys_shm_create(size) — syscall 15 (创建共享内存)
// Returns: virtual address on success, 0 on failure
uint64_t sys_shm_create(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    size_t size = (size_t)arg1;
    if (size == 0) return 0;
    size = ALIGN_UP(size, PAGE_SIZE);
    size_t npages = size / PAGE_SIZE;

    proc_t *proc = current_proc;

    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_SHM_PER_PROC; i++) {
        if (proc->shm_regions[i].ref_count == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;

    // Allocate physical pages
    Page *pages = bfc_alloc.alloc_page(npages);
    if (!pages) return 0;
    uint64_t phys = (uint64_t)(pages - BFCAllocator::frames) * PAGE_SIZE;

    // Zero the pages
    uint8_t *vptr = (uint8_t *)phys_to_virt(phys);
    for (size_t i = 0; i < size; i++) vptr[i] = 0;

    // Pick virtual address: scan existing regions, find gap above SHM_VADDR_BASE
    uint64_t vaddr = SHM_VADDR_BASE;
    for (int i = 0; i < MAX_SHM_PER_PROC; i++) {
        if (proc->shm_regions[i].ref_count > 0) {
            uint64_t end = proc->shm_regions[i].vaddr + proc->shm_regions[i].npages * PAGE_SIZE;
            if (end > vaddr) vaddr = ALIGN_UP(end, PAGE_SIZE);
        }
    }

    // Map into caller's PML4
    uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->cr3);
    uint64_t flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;
    for (size_t i = 0; i < npages; i++) {
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags)) {
            // Rollback: unmap already-mapped pages
            for (size_t j = 0; j < i; j++)
                unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
            bfc_alloc.free_page(&BFCAllocator::frames[PHY_TO_PAGE(phys)], npages);
            return 0;
        }
    }

    // Fill slot
    proc->shm_regions[slot].vaddr = vaddr;
    proc->shm_regions[slot].phys = phys;
    proc->shm_regions[slot].npages = npages;
    proc->shm_regions[slot].ref_count = 1;

    return vaddr;
}

// sys_shm_attach(target_pid) — syscall 16 (附加共享内存)
// Returns: virtual address on success, 0 on failure
uint64_t sys_shm_attach(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    pid_t target_pid = (pid_t)arg1;
    if (target_pid < 0 || target_pid >= MAX_PROC) return 0;

    proc_t *proc = current_proc;
    proc_t *target = &procs[target_pid];

    // Under procs_lock, find target's first active shm region
    spin_lock(&procs_lock);
    if (target->pid != target_pid) {
        spin_unlock(&procs_lock);
        return 0;
    }

    shm_region *src = nullptr;
    for (int i = 0; i < MAX_SHM_PER_PROC; i++) {
        if (target->shm_regions[i].ref_count > 0) {
            src = &target->shm_regions[i];
            break;
        }
    }
    if (!src) {
        spin_unlock(&procs_lock);
        return 0;
    }

    // Find free slot in our shm_regions
    int slot = -1;
    for (int i = 0; i < MAX_SHM_PER_PROC; i++) {
        if (proc->shm_regions[i].ref_count == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&procs_lock);
        return 0;
    }

    // Increment ref_count while still under lock
    src->ref_count++;
    spin_unlock(&procs_lock);

    // Pick virtual address
    uint64_t vaddr = SHM_VADDR_BASE;
    for (int i = 0; i < MAX_SHM_PER_PROC; i++) {
        if (proc->shm_regions[i].ref_count > 0) {
            uint64_t end = proc->shm_regions[i].vaddr + proc->shm_regions[i].npages * PAGE_SIZE;
            if (end > vaddr) vaddr = ALIGN_UP(end, PAGE_SIZE);
        }
    }

    // Map shared physical pages into our PML4
    uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->cr3);
    uint64_t flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;
    for (size_t i = 0; i < src->npages; i++) {
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, src->phys + i * PAGE_SIZE, flags)) {
            // Rollback: unmap already-mapped pages
            for (size_t j = 0; j < i; j++)
                unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
            // Decrement ref_count
            spin_lock(&procs_lock);
            src->ref_count--;
            spin_unlock(&procs_lock);
            return 0;
        }
    }

    // Fill our slot (ref_count=1 means we hold a reference; actual physical page
    // refcount is tracked by counting entries across all procs[].shm_regions)
    proc->shm_regions[slot].vaddr = vaddr;
    proc->shm_regions[slot].phys = src->phys;
    proc->shm_regions[slot].npages = src->npages;
    proc->shm_regions[slot].ref_count = 1;

    return vaddr;
}
