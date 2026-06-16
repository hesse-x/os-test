#include "kernel/trap.h"
#include "kernel/proc.h"
#include "kernel/spinlock.h"
#include "kernel/ahci.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "arch/x64/apic.h"
#include "kernel/serial.h"
#include "kernel/mem/slab.h"
#include "kernel/mem/alloc.h"
#include "common/errno.h"
#include "kernel/fb.h"
#include "kernel/pci.h"
#include "common/dev.h"

// ===================== IRQ handler registry =====================
#define MAX_IRQ_HANDLERS 48
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS];

// ===================== IRQ owner (user-space driver binding) =====================
static pid_t irq_owner[MAX_IRQ_HANDLERS];

// ===================== Device table (dev_type → PID) =====================
static pid_t dev_table[DEV_TYPE_MAX];

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

      // Enqueue RECV_IRQ message to target's recv queue
      spin_lock(&target->recv_lock);
      uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
      if (next != target->recv_tail) {  // drop if full
        recv_msg *slot = (recv_msg *)target->recv_buf[target->recv_head];
        slot->type = RECV_IRQ;
        slot->src = tf->trapno;
        target->recv_head = next;
      }
      spin_unlock(&target->recv_lock);

      // Wake target if in WAIT_RECV
      int target_cpu = target->assigned_cpu;
      spin_lock(&cpu_locals[target_cpu].scheduler_lock);
      if (target->pid == owner_pid &&
          target->state == BLOCKED &&
          target->wait_event == WAIT_RECV) {
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

  // Initialize device table
  for (int i = 0; i < DEV_TYPE_MAX; i++) {
    dev_table[i] = 0;
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
#define NR_SYSCALL 34
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_getpid,         // 0
    sys_yield,          // 1
    sys_recv,           // 2
    sys_req,            // 3
    sys_resp,           // 4
    sys_irq_bind,       // 5
    sys_exit,           // 6
    sys_waitpid,        // 7
    sys_spawn,          // 8
    sys_mmap,           // 9
    sys_munmap,         // 10
    sys_serial_write,   // 11
    sys_fb_info,        // 12
    sys_shm_create,     // 13
    sys_shm_attach,     // 14
    sys_pipe,           // 15
    sys_write,          // 16
    sys_read,           // 17
    sys_close,          // 18
    sys_load_dev,       // 19
    sys_lookup_dev,     // 20
    sys_notify,         // 21
    sys_gettime,        // 22
    sys_clock,          // 23
    sys_msg,            // 24
    sys_msg_resp,       // 25
    sys_ioperm,         // 26
    sys_dup2,           // 27
    sys_fcntl,          // 28
    sys_dma_alloc,      // 29
    sys_dma_free,       // 30
    sys_pci_dev_info,   // 31
    sys_block_read,     // 32
    sys_block_write,    // 33
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->rax < NR_SYSCALL) {
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

// sys_recv(buf, data_buf, data_buf_len, timeout_ms) — syscall 2 (统一事件接收：IRQ/REQ/notify/MSG)
// timeout_ms=0: 无限等待; >0: 超时后唤醒
// 返回: 0=成功, 正数=errno
uint64_t sys_recv(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t) {
    void *buf = (void *)arg1;
    void *data_buf = (void *)arg2;
    size_t data_buf_len = (size_t)arg3;
    uint32_t timeout_ms = (uint32_t)arg4;

    // Validate user pointer
    uint64_t ptr = (uint64_t)buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    // Validate data_buf if provided
    if (data_buf && (data_buf_len == 0 ||
        (uint64_t)data_buf >= 0xFFFFFFFF80000000ULL ||
        (uint64_t)data_buf + data_buf_len > 0xFFFFFFFF80000000ULL))
        return (uint64_t)EFAULT;

    proc_t *proc = current_proc;
    int cpu = proc->assigned_cpu;

    while (1) {
        // Try to dequeue a message from recv queue
        spin_lock(&proc->recv_lock);
        if (proc->recv_head != proc->recv_tail) {
            // Message available: copy to user buffer
            __memcpy(buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
            // If this is an REQ request, record the caller PID for sys_resp
            recv_msg *msg = (recv_msg *)proc->recv_buf[proc->recv_tail];
            if (msg->type == RECV_REQ) {
                proc->req_caller_pid = (pid_t)msg->src;
            }
            // If this is RECV_MSG, copy data to user data_buf and free kernel buffer
            if (msg->type == RECV_MSG) {
                void *kmaddr = msg->msg.kmaddr;
                size_t len = msg->msg.len;
                proc->msg_caller_pid = (pid_t)msg->src;

                if (!data_buf || data_buf_len < len) {
                    // User didn't provide enough buffer — free kernel buf and return error
                    kfree(kmaddr);
                    // Write msg.len into data field so user knows the required size
                    recv_msg *umsg = (recv_msg *)buf;
                    umsg->msg.kmaddr = nullptr;
                    umsg->msg.len = len;
                    proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
                    spin_unlock(&proc->recv_lock);
                    return (uint64_t)EINVAL;
                }

                // Copy data to user buffer under current CR3
                __memcpy(data_buf, kmaddr, len);
                kfree(kmaddr);

                // Rewrite recv_msg for user: put len in data field
                recv_msg *umsg = (recv_msg *)buf;
                umsg->msg.kmaddr = nullptr;
                umsg->msg.len = len;
            }
            proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
            spin_unlock(&proc->recv_lock);
            return 0;  // success
        }
        spin_unlock(&proc->recv_lock);

        // Queue empty: block on WAIT_RECV
        proc->state = BLOCKED;
        proc->wait_event = WAIT_RECV;
        proc->wait_timed_out = 0;

        if (timeout_ms > 0) {
            proc->wait_deadline = sched_clock() + (uint64_t)timeout_ms * 1000000ULL;
            spin_lock(&cpu_locals[cpu].scheduler_lock);
            timer_queue_insert(cpu, proc);
            spin_unlock(&cpu_locals[cpu].scheduler_lock);
        } else {
            proc->wait_deadline = 0;
        }

        __atomic_add_fetch(&cpu_locals[cpu].run_count, -1, __ATOMIC_RELAXED);
        schedule();

        // Woken up: check if timed out
        if (proc->wait_timed_out) {
            // Re-check queue before returning timeout (race: message may have arrived)
            spin_lock(&proc->recv_lock);
            if (proc->recv_head != proc->recv_tail) {
                __memcpy(buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
                recv_msg *msg = (recv_msg *)proc->recv_buf[proc->recv_tail];
                if (msg->type == RECV_REQ) {
                    proc->req_caller_pid = (pid_t)msg->src;
                }
                if (msg->type == RECV_MSG) {
                    void *kmaddr = msg->msg.kmaddr;
                    size_t len = msg->msg.len;
                    proc->msg_caller_pid = (pid_t)msg->src;

                    if (!data_buf || data_buf_len < len) {
                        kfree(kmaddr);
                        recv_msg *umsg = (recv_msg *)buf;
                        umsg->msg.kmaddr = nullptr;
                        umsg->msg.len = len;
                        proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
                        spin_unlock(&proc->recv_lock);
                        return (uint64_t)EINVAL;
                    }
                    __memcpy(data_buf, kmaddr, len);
                    kfree(kmaddr);
                    recv_msg *umsg = (recv_msg *)buf;
                    umsg->msg.kmaddr = nullptr;
                    umsg->msg.len = len;
                }
                proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
                spin_unlock(&proc->recv_lock);
                return 0;
            }
            spin_unlock(&proc->recv_lock);
            return (uint64_t)ETIMEDOUT;
        }
        // Non-timeout wakeup: a message was enqueued, loop back to dequeue it
    }
}

// sys_req(pid, request, reply) — syscall 3 (同步 REQ)
// 向目标发送 64 字节请求，阻塞等待 reply
// 返回: 0=成功, 正数=errno
uint64_t sys_req(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    pid_t target_pid = (pid_t)arg1;
    void *request = (void *)arg2;
    void *reply = (void *)arg3;

    // Validate target PID
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)ESRCH;

    // Validate user pointers
    uint64_t req_ptr = (uint64_t)request;
    uint64_t rep_ptr = (uint64_t)reply;
    if (!req_ptr || req_ptr >= 0xFFFFFFFF80000000ULL ||
        req_ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;
    if (!rep_ptr || rep_ptr >= 0xFFFFFFFF80000000ULL ||
        rep_ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) return (uint64_t)ESRCH;

    // Build RECV_REQ message
    uint8_t msg[RECV_MSG_SIZE];
    recv_msg *hdr = (recv_msg *)msg;
    hdr->type = RECV_REQ;
    hdr->src = (uint32_t)current_proc->pid;
    // Copy request payload from user space
    __memcpy(hdr->data, request, 56);

    // Enqueue to target's recv queue
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        return (uint64_t)EBUSY;  // queue full
    }
    __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
    target->recv_head = next;
    spin_unlock(&target->recv_lock);

    // Wake target if in WAIT_RECV
    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
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

    // Block caller on WAIT_REQ_REPLY
    proc_t *proc = current_proc;
    int cpu = proc->assigned_cpu;
    proc->state = BLOCKED;
    proc->wait_event = WAIT_REQ_REPLY;
    proc->wait_timed_out = 0;
    proc->wait_deadline = 0;
    proc->req_target_pid = target_pid;
    proc->req_reply_buf = reply;
    proc->req_result = 0;

    __atomic_add_fetch(&cpu_locals[cpu].run_count, -1, __ATOMIC_RELAXED);
    schedule();

    // Woken up by sys_resp or proc_reap
    if (proc->req_result != 0) return (uint64_t)proc->req_result;
    return 0;
}

// sys_resp(reply) — syscall 4 (回复当前 REQ 调用者)
// 返回: 0=成功, 正数=errno
uint64_t sys_resp(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    void *reply = (void *)arg1;

    // Validate user pointer
    uint64_t ptr = (uint64_t)reply;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    proc_t *proc = current_proc;
    pid_t caller_pid = proc->req_caller_pid;
    if (caller_pid < 0 || caller_pid >= MAX_PROC) return (uint64_t)EINVAL;

    proc_t *caller = &procs[caller_pid];
    if (caller->pid != caller_pid) return (uint64_t)ESRCH;

    // Copy reply data to caller's reply buffer (user space)
    // We must first copy to a kernel buffer under the server's CR3,
    // then switch to caller's CR3 to write to the caller's user-space buffer.
    uint8_t kbuf[RECV_MSG_SIZE];
    __memcpy(kbuf, reply, RECV_MSG_SIZE);

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)caller->cr3) : "memory");
    __memcpy(caller->req_reply_buf, kbuf, RECV_MSG_SIZE);
    __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

    // Wake caller
    int caller_cpu = caller->assigned_cpu;
    spin_lock(&cpu_locals[caller_cpu].scheduler_lock);
    if (caller->state == BLOCKED && caller->wait_event == WAIT_REQ_REPLY) {
        caller->state = READY;
        caller->wait_event = WAIT_NONE;
        caller->wait_timed_out = 0;
        caller->req_result = 0;
        list_push_back(&cpu_locals[caller_cpu].run_queue, &caller->run_node);
        cpu_locals[caller_cpu].run_count++;
    }
    spin_unlock(&cpu_locals[caller_cpu].scheduler_lock);

    proc->req_caller_pid = -1;  // clear
    return 0;
}

// sys_irq_bind(irq) — syscall 5 (绑定当前进程到指定 IRQ，自动 unmask I/O APIC)
// irq 参数为向量号（例如 IRQ14 → vector 46 = 32+14）
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)EINVAL;
    __atomic_store_n(&irq_owner[irq], current_proc->pid, __ATOMIC_RELEASE);

    // Auto-unmask I/O APIC for this IRQ (GSI = vector - 32)
    int gsi = irq - 32;
    if (gsi >= 0 && gsi < 24) {
        uint32_t bsp_apic_id = (uint32_t)(lapic_read(LAPIC_ID) >> 24);
        ioapic_set_irq(gsi, irq, bsp_apic_id, false);
    }

    return 0;
}

// sys_exit(exit_code) — syscall 6 (进程退出)
uint64_t sys_exit(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    proc_t *proc = current_proc;
    int32_t exit_code = (int32_t)arg1;
    proc->exit_code = exit_code;

    // Final CPU time accounting: ensure last running slice is recorded
    if (proc->last_sched != 0) {
        proc->cpu_time_ns += sched_clock() - proc->last_sched;
        proc->last_sched = 0;
    }

    // Orphan adoption: reparent children to init
    if (init_pid >= 0) {
        spin_lock(&procs_lock);
        for (int i = 0; i < MAX_PROC; i++) {
            if (procs[i].pid >= 0 && procs[i].parent_pid == proc->pid) {
                procs[i].parent_pid = init_pid;
            }
        }
        spin_unlock(&procs_lock);
    }

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
        // Notify parent via recv queue (for WAIT_CHILD matching)
        // Also try sys_notify semantics: enqueue RECV_NOTIFY message
        {
            proc_t *parent = &procs[proc->parent_pid];
            spin_lock(&parent->recv_lock);
            uint32_t next = (parent->recv_head + 1) % RECV_QUEUE_SIZE;
            if (next != parent->recv_tail) {
                recv_msg *slot = (recv_msg *)parent->recv_buf[parent->recv_head];
                slot->type = RECV_NOTIFY;
                slot->src = (uint32_t)proc->pid;
                parent->recv_head = next;
            }
            spin_unlock(&parent->recv_lock);

            // Wake parent if in WAIT_RECV or WAIT_CHILD
            int pcpu = parent->assigned_cpu;
            spin_lock(&cpu_locals[pcpu].scheduler_lock);
            if (parent->state == BLOCKED &&
                (parent->wait_event == WAIT_RECV ||
                 parent->wait_event == WAIT_CHILD)) {
                if (parent->wait_deadline != 0) {
                    timer_queue_remove(parent);
                    parent->wait_deadline = 0;
                }
                parent->state = READY;
                parent->wait_event = WAIT_NONE;
                parent->wait_timed_out = 0;
                list_push_back(&cpu_locals[pcpu].run_queue, &parent->run_node);
                cpu_locals[pcpu].run_count++;
            }
            spin_unlock(&cpu_locals[pcpu].scheduler_lock);
        }

        // Wake any processes waiting for our REQ reply
        for (int i = 0; i < MAX_PROC; i++) {
            proc_t *waiter = &procs[i];
            if (waiter->pid >= 0 &&
                waiter->state == BLOCKED &&
                waiter->wait_event == WAIT_REQ_REPLY &&
                waiter->req_target_pid == proc->pid) {
                int wcpu = waiter->assigned_cpu;
                spin_lock(&cpu_locals[wcpu].scheduler_lock);
                if (waiter->state == BLOCKED && waiter->wait_event == WAIT_REQ_REPLY) {
                    waiter->state = READY;
                    waiter->wait_event = WAIT_NONE;
                    waiter->req_result = ESRCH;
                    list_push_back(&cpu_locals[wcpu].run_queue, &waiter->run_node);
                    cpu_locals[wcpu].run_count++;
                }
                spin_unlock(&cpu_locals[wcpu].scheduler_lock);
            }
        }
    }

    schedule();  // never returns
    return 0;    // unreachable
}

// sys_waitpid(pid, exit_code_ptr) — syscall 7 (等待子进程退出)
// pid=-1: wait for any child
uint64_t sys_waitpid(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    pid_t pid = (pid_t)arg1;
    int32_t *exit_code_ptr = (int32_t *)arg2;

    if (pid == -1) {
        // Wait for any child to become ZOMBIE
        while (1) {
            // Scan for a ZOMBIE child
            spin_lock(&procs_lock);
            proc_t *zombie = nullptr;
            for (int i = 0; i < MAX_PROC; i++) {
                if (procs[i].pid >= 0 && procs[i].parent_pid == current_proc->pid &&
                    procs[i].state == ZOMBIE) {
                    zombie = &procs[i];
                    break;
                }
            }
            if (zombie) {
                int cpu = zombie->assigned_cpu;
                spin_unlock(&procs_lock);
                uint64_t flags;
                spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
                if (zombie->state == ZOMBIE) {
                    zombie->state = REAPING;
                    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
                } else {
                    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
                    continue;  // Race, retry
                }
                pid_t zpid = zombie->pid;
                if (exit_code_ptr) {
                    uint64_t ptr_val = (uint64_t)exit_code_ptr;
                    if (ptr_val < 0xFFFFFFFF80000000ULL && ptr_val &&
                        (ptr_val + sizeof(int32_t) - 1) < 0xFFFFFFFF80000000ULL)
                        *exit_code_ptr = zombie->exit_code;
                }
                proc_reap(zombie);
                return (uint64_t)zpid;
            }
            spin_unlock(&procs_lock);

            // No zombie child: block on WAIT_CHILD
            current_proc->wait_event = WAIT_CHILD;
            current_proc->state = BLOCKED;
            __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
            schedule();
        }
    }

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

// sys_spawn(elf_data, elf_size) — syscall 8 (创建子进程)
uint64_t sys_spawn(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    const uint8_t *elf_data_user = (const uint8_t *)arg1;
    uint64_t elf_size = arg2;

    // Basic parameter validation
    if (!elf_data_user || elf_size == 0)
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
    proc_t *child = process_create_elf(elf_buf, elf_size);
    kfree(elf_buf);
    if (!child) return (uint64_t)ENOMEM;

    child->parent_pid = current_proc->pid;

    // Inherit fd 0 and fd 1 from parent
    for (int fd = 0; fd <= 1; fd++) {
        if (current_proc->fd_table[fd].type != FD_NONE) {
            child->fd_table[fd] = current_proc->fd_table[fd];
            if (child->fd_table[fd].type == FD_PIPE) {
                child->fd_table[fd].pipe->ref_count++;
            }
        }
    }

    return (uint64_t)child->pid;
}

// sys_mmap(addr, size, prot, flags, offset) — syscall 9 (内存映射)
// MAP_PHYSICAL: map physical address range (no new page allocation)
// Otherwise: anonymous private mapping (allocates new pages)
// Returns: mapped address on success, 0 on failure
#define MAP_PHYSICAL_KERNEL 0x80000000

uint64_t sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    uint64_t addr = arg1;
    size_t size = (size_t)arg2;
    // int prot = (int)arg3;  // prot currently ignored
    int flags = (int)arg4;
    uint64_t offset = arg5;

    if (size == 0) return 0;

    size = ALIGN_UP(size, PAGE_SIZE);
    proc_t *proc = current_proc;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->cr3);

    if (flags & MAP_PHYSICAL_KERNEL) {
        // MAP_PHYSICAL: map physical address range to user address space
        uint64_t vaddr = proc->mmap_brk;
        uint64_t phys_start = ALIGN_DOWN(offset, PAGE_SIZE);
        uint64_t phys_end = ALIGN_UP(offset + size, PAGE_SIZE);
        size_t npages = (phys_end - phys_start) / PAGE_SIZE;
        uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

        for (size_t i = 0; i < npages; i++) {
            if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                      phys_start + i * PAGE_SIZE, pte_flags)) {
                // Rollback
                for (size_t j = 0; j < i; j++)
                    unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
                return 0;
            }
        }

        mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
        if (!region) {
            for (size_t i = 0; i < npages; i++)
                unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE, 1);
            return 0;
        }

        region->vaddr = vaddr;
        region->size = npages * PAGE_SIZE;
        region->phys = 0;
        region->next = proc->mmap_regions;
        proc->mmap_regions = region;
        proc->mmap_brk = vaddr + npages * PAGE_SIZE;

        return vaddr;
    }

    // Anonymous private mapping: allocate new pages
    uint64_t vaddr = proc->mmap_brk;
    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

    size_t npages = size / PAGE_SIZE;
    uint64_t *phys_pages = (uint64_t *)kmalloc(npages * sizeof(uint64_t));
    if (!phys_pages) return 0;

    size_t mapped = 0;
    for (size_t i = 0; i < npages; i++) {
        Page *page = bfc_alloc.alloc_page(1);
        if (!page) {
            for (size_t j = 0; j < mapped; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }
            kfree(phys_pages);
            return 0;
        }
        phys_pages[i] = page_to_phys(page);
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, phys_pages[i], pte_flags)) {
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

    mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!region) {
        for (size_t i = 0; i < npages; i++) {
            uint64_t va = vaddr + i * PAGE_SIZE;
            unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
        }
        kfree(phys_pages);
        return 0;
    }

    region->vaddr = vaddr;
    region->size = size;
    region->phys = 0;
    region->next = proc->mmap_regions;
    proc->mmap_regions = region;
    proc->mmap_brk = vaddr + size;

    kfree(phys_pages);
    return vaddr;
}

// sys_munmap(addr, size) — syscall 10 (解除内存映射)
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

// ===================== sys_block_read / sys_block_write =====================
uint64_t sys_block_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    uint32_t lba = (uint32_t)arg1;
    void *buf = (void *)arg2;
    uint32_t count = (uint32_t)arg3;

    uint64_t ptr = (uint64_t)buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    uint64_t end = ptr + (uint64_t)count * 512;
    if (end < ptr || end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    if (count == 0 || count > AHCI_MAX_SECTORS)
        return (uint64_t)EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    int rc = ahci_read_lba(lba, count, buf);
    spin_unlock_irqrestore(&ahci_lock, flags);

    return (uint64_t)(rc < 0 ? -rc : 0);
}

uint64_t sys_block_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    uint32_t lba = (uint32_t)arg1;
    const void *buf = (const void *)arg2;
    uint32_t count = (uint32_t)arg3;

    uint64_t ptr = (uint64_t)buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    uint64_t end = ptr + (uint64_t)count * 512;
    if (end < ptr || end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    if (count == 0 || count > AHCI_MAX_SECTORS)
        return (uint64_t)EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    int rc = ahci_write_lba(lba, count, buf);
    spin_unlock_irqrestore(&ahci_lock, flags);

    return (uint64_t)(rc < 0 ? -rc : 0);
}// sys_serial_write(buf, len) — syscall 11 (用户态串口输出)
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

// sys_fb_info(buf) — syscall 12 (获取 framebuffer 信息)
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

// sys_shm_create(size) — syscall 13 (创建共享内存)
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

// sys_shm_attach(target_pid) — syscall 14 (附加共享内存)
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

// ===================== Pipe / fd syscalls =====================

// Wake a process that is BLOCKED on WAIT_PIPE (pipe I/O)
// Note: we do NOT enqueue a RECV_NOTIFY message here. Pipe I/O wakeups
// only need to change the process state; the process resumes in its
// sys_read/sys_write loop which never checks the recv queue. Enqueuing
// RECV_NOTIFY would leave stale messages that sys_recv can mistakenly
// consume, causing IPC protocols to see false responses.
void wake_process(pid_t pid) {
    if (pid < 0 || pid >= MAX_PROC) return;
    proc_t *target = &procs[pid];

    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->pid == pid &&
        target->state == BLOCKED &&
        target->wait_event == WAIT_PIPE) {
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
}

// sys_pipe(fd_ptr) — syscall 15 (创建 pipe，写 [read_fd, write_fd] 到用户指针)
uint64_t sys_pipe(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int *fd_ptr = (int *)arg1;

    // Validate user pointer
    uint64_t ptr = (uint64_t)fd_ptr;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + 2 * sizeof(int) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    proc_t *proc = current_proc;

    // Find two free fd slots (skip 0/1, reserved for stdin/stdout)
    int read_fd = -1, write_fd = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            if (read_fd < 0) read_fd = i;
            else if (write_fd < 0) { write_fd = i; break; }
        }
    }
    if (read_fd < 0 || write_fd < 0) return (uint64_t)ENOMEM;

    // Allocate pipe buffer (1 page)
    uint8_t *buf = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!buf) return (uint64_t)ENOMEM;

    // Allocate pipe struct
    struct pipe *p = (struct pipe *)kmalloc(sizeof(struct pipe));
    if (!p) { kfree(buf); return (uint64_t)ENOMEM; }

    // Zero the buffer
    for (int i = 0; i < PIPE_BUF_SIZE; i++) buf[i] = 0;

    p->buf = buf;
    p->head = 0;
    p->tail = 0;
    p->read_pid = -1;
    p->write_pid = -1;
    p->ref_count = 2;  // read end + write end

    // Fill fd table entries
    proc->fd_table[read_fd].type = FD_PIPE;
    proc->fd_table[read_fd].flags = O_RDONLY;
    proc->fd_table[read_fd].pipe = p;

    proc->fd_table[write_fd].type = FD_PIPE;
    proc->fd_table[write_fd].flags = O_WRONLY;
    proc->fd_table[write_fd].pipe = p;

    // Write fd pair to user space
    fd_ptr[0] = read_fd;
    fd_ptr[1] = write_fd;

    return 0;
}

// sys_write(fd, buf, len) — syscall 16 (向 fd 写入数据)
uint64_t sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    int fd = (int)arg1;
    const char *buf = (const char *)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)EINVAL;
    if (current_proc->fd_table[fd].type == FD_NONE) return (uint64_t)EINVAL;
    if (!(current_proc->fd_table[fd].flags & (O_WRONLY | O_RDWR))) return (uint64_t)EINVAL;

    // Validate user buf pointer
    if (!buf) return (uint64_t)EFAULT;
    uint64_t ptr_start = (uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    struct pipe *p = current_proc->fd_table[fd].pipe;
    size_t written = 0;

    while (written < len) {
        // Check if pipe has space: full when head is one behind tail
        if ((p->head + 1) % PIPE_BUF_SIZE == p->tail) {
            // Pipe full: block
            p->write_pid = current_proc->pid;
            current_proc->state = BLOCKED;
            current_proc->wait_event = WAIT_PIPE;
            __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
            schedule();
            p->write_pid = -1;
            continue;
        }
        p->buf[p->head] = buf[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        written++;
    }

    // Wake reader if blocked
    if (p->read_pid >= 0) wake_process(p->read_pid);

    return (uint64_t)written;
}

// sys_read(fd, buf, len) — syscall 17 (从 fd 读数据，阻塞直到有数据)
uint64_t sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    int fd = (int)arg1;
    char *buf = (char *)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)EINVAL;
    if (current_proc->fd_table[fd].type == FD_NONE) return (uint64_t)EINVAL;
    // O_RDONLY=0, so check: must not be O_WRONLY only
    if ((current_proc->fd_table[fd].flags & O_WRONLY) && !(current_proc->fd_table[fd].flags & O_RDWR))
        return (uint64_t)EINVAL;

    // Validate user buf pointer
    if (!buf) return (uint64_t)EFAULT;
    uint64_t ptr_start = (uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    struct pipe *p = current_proc->fd_table[fd].pipe;

    // Block if pipe is empty
    while (p->head == p->tail) {
        // Check if write end is closed (all write fds gone)
        if (p->ref_count == 1) return 0;  // EOF: no writers left
        // Non-blocking: return EAGAIN-like indication (0 bytes read)
        if (current_proc->fd_table[fd].flags & O_NONBLOCK) return 0;
        p->read_pid = current_proc->pid;
        current_proc->state = BLOCKED;
        current_proc->wait_event = WAIT_PIPE;
        __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
        schedule();
        p->read_pid = -1;
    }

    // Read as much as available (up to len)
    size_t nread = 0;
    while (nread < len && p->head != p->tail) {
        buf[nread] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        nread++;
    }

    // Wake writer if blocked
    if (p->write_pid >= 0) wake_process(p->write_pid);

    return (uint64_t)nread;
}

// sys_close(fd) — syscall 18 (关闭 fd，pipe ref_count--)
uint64_t sys_close(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int fd = (int)arg1;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)EINVAL;
    if (current_proc->fd_table[fd].type == FD_NONE) return (uint64_t)EINVAL;

    struct pipe *p = current_proc->fd_table[fd].pipe;
    p->ref_count--;

    // Notify blocked peer
    if (current_proc->fd_table[fd].flags & (O_WRONLY | O_RDWR)) {
        // Closing write end: wake reader (it will see EOF if ref_count indicates no writers)
        if (p->read_pid >= 0) wake_process(p->read_pid);
    }
    if (current_proc->fd_table[fd].flags & (O_RDONLY | O_RDWR)) {
        // Closing read end: wake writer (it should get EPIPE)
        if (p->write_pid >= 0) wake_process(p->write_pid);
    }

    if (p->ref_count == 0) {
        kfree(p->buf);
        kfree(p);
    }

    current_proc->fd_table[fd].type = FD_NONE;
    current_proc->fd_table[fd].flags = 0;
    current_proc->fd_table[fd].pipe = nullptr;

    return 0;
}

// ===================== Device table syscalls =====================

// Kernel-internal: register a driver PID for a device type
int register_dev(int dev_type, pid_t pid) {
    if (dev_type <= DEV_NONE || dev_type >= DEV_TYPE_MAX) return EINVAL;
    if (dev_table[dev_type] != 0) return EEXIST;
    dev_table[dev_type] = pid;
    return 0;
}

// sys_load_dev(pid, dev_type) — syscall 19 (注册驱动)
// Returns: 0 on success, positive errno on failure
uint64_t sys_load_dev(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    pid_t pid = (pid_t)arg1;
    int dev_type = (int)arg2;
    return (uint64_t)register_dev(dev_type, pid);
}

// sys_lookup_dev(dev_type) — syscall 20 (查询驱动 PID)
// Returns: PID on success, 0 if not found
uint64_t sys_lookup_dev(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int dev_type = (int)arg1;
    if (dev_type <= DEV_NONE || dev_type >= DEV_TYPE_MAX) return 0;
    return (uint64_t)dev_table[dev_type];
}

// Remove a PID from dev_table (called by proc_reap)
void dev_table_cleanup(pid_t pid) {
    for (int i = 0; i < DEV_TYPE_MAX; i++) {
        if (dev_table[i] == pid) dev_table[i] = 0;
    }
}

// sys_notify(pid) — syscall 21 (异步通知：消息入队 + 唤醒)
// Returns: 0 on success, positive errno on failure
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    pid_t target_pid = (pid_t)arg1;
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)EINVAL;

    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) return (uint64_t)ESRCH;

    // Enqueue RECV_NOTIFY message
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        return (uint64_t)EBUSY;  // queue full
    }
    recv_msg *slot = (recv_msg *)target->recv_buf[target->recv_head];
    slot->type = RECV_NOTIFY;
    slot->src = (uint32_t)current_proc->pid;
    target->recv_head = next;
    spin_unlock(&target->recv_lock);

    // Wake target if in WAIT_RECV
    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
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

// sys_gettime() — syscall 22 (全局单调时钟，返回纳秒)
uint64_t sys_gettime(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return sched_clock();
}

// sys_clock() — syscall 23 (per-process CPU 时间，返回纳秒)
uint64_t sys_clock(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return current_proc->cpu_time_ns;
}

// sys_msg(target_pid, msg_buf, msg_len, reply_buf, reply_len) — syscall 24 (变长消息请求)
// 向目标发送变长数据，阻塞等待 reply
// 返回: 0=成功, 负errno
uint64_t sys_msg(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    pid_t target_pid = (pid_t)arg1;
    void *msg_buf = (void *)arg2;
    size_t msg_len = (size_t)arg3;
    void *reply_buf = (void *)arg4;
    size_t reply_len = (size_t)arg5;

    // Validate target PID
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)ESRCH;

    // Validate msg_len
    if (msg_len == 0 || msg_len > 65536) return (uint64_t)EINVAL;

    // Validate user pointers
    uint64_t msg_ptr = (uint64_t)msg_buf;
    if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
        msg_ptr + msg_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    uint64_t rep_ptr = (uint64_t)reply_buf;
    if (!rep_ptr || rep_ptr >= 0xFFFFFFFF80000000ULL ||
        rep_ptr + reply_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) return (uint64_t)ESRCH;

    // Allocate kernel buffer and copy message from user space
    void *kbuf = kmalloc(msg_len);
    if (!kbuf) return (uint64_t)ENOMEM;
    __memcpy(kbuf, msg_buf, msg_len);

    // Build RECV_MSG
    uint8_t msg[RECV_MSG_SIZE];
    recv_msg *hdr = (recv_msg *)msg;
    hdr->type = RECV_MSG;
    hdr->src = (uint32_t)current_proc->pid;
    hdr->msg.kmaddr = kbuf;
    hdr->msg.len = msg_len;

    // Enqueue to target's recv queue
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        kfree(kbuf);
        return (uint64_t)EBUSY;  // queue full
    }
    __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
    target->recv_head = next;
    spin_unlock(&target->recv_lock);

    // Wake target if in WAIT_RECV
    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
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

    // Block caller on WAIT_MSG_REPLY
    proc_t *proc = current_proc;
    int cpu = proc->assigned_cpu;
    proc->state = BLOCKED;
    proc->wait_event = WAIT_MSG_REPLY;
    proc->wait_timed_out = 0;
    proc->wait_deadline = 0;
    proc->msg_target_pid = target_pid;
    proc->msg_reply_buf = reply_buf;
    proc->msg_reply_len = reply_len;
    proc->msg_result = 0;

    __atomic_add_fetch(&cpu_locals[cpu].run_count, -1, __ATOMIC_RELAXED);
    schedule();

    // Woken up by sys_msg_resp or proc_reap
    if (proc->msg_result != 0) return (uint64_t)proc->msg_result;
    return 0;
}

// sys_msg_resp(resp_buf, resp_len) — syscall 25 (回复当前 MSG 调用者)
// 返回: 0=成功, 正数=errno
uint64_t sys_msg_resp(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    void *resp_buf = (void *)arg1;
    size_t resp_len = (size_t)arg2;

    // Validate user pointer
    uint64_t ptr = (uint64_t)resp_buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || resp_len == 0 ||
        ptr + resp_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    proc_t *proc = current_proc;
    pid_t caller_pid = proc->msg_caller_pid;
    if (caller_pid < 0 || caller_pid >= MAX_PROC) return (uint64_t)EINVAL;

    proc_t *caller = &procs[caller_pid];
    if (caller->pid != caller_pid) return (uint64_t)ESRCH;

    // Copy response data from server user space to kernel buffer
    void *kbuf = kmalloc(resp_len);
    if (!kbuf) return (uint64_t)ENOMEM;
    __memcpy(kbuf, resp_buf, resp_len);

    // Copy to caller's reply buffer under caller's CR3
    size_t copy_len = resp_len < caller->msg_reply_len ? resp_len : caller->msg_reply_len;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)caller->cr3) : "memory");
    __memcpy(caller->msg_reply_buf, kbuf, copy_len);
    __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

    kfree(kbuf);

    // Wake caller
    int caller_cpu = caller->assigned_cpu;
    spin_lock(&cpu_locals[caller_cpu].scheduler_lock);
    if (caller->state == BLOCKED && caller->wait_event == WAIT_MSG_REPLY) {
        caller->state = READY;
        caller->wait_event = WAIT_NONE;
        caller->wait_timed_out = 0;
        caller->msg_result = 0;
        list_push_back(&cpu_locals[caller_cpu].run_queue, &caller->run_node);
        cpu_locals[caller_cpu].run_count++;
    }
    spin_unlock(&cpu_locals[caller_cpu].scheduler_lock);

    proc->msg_caller_pid = -1;  // clear
    return 0;
}

// sys_ioperm(from, num, turn_on) — syscall 26 (I/O 端口权限)
uint64_t sys_ioperm(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    unsigned long from = (unsigned long)arg1;
    unsigned long num = (unsigned long)arg2;
    int turn_on = (int)arg3;

    if (from + num > 65536) return (uint64_t)EINVAL;

    proc_t *proc = current_proc;

    // Lazy-allocate IOPM if needed
    if (!proc->iopm) {
        uint8_t *iopm = (uint8_t *)kmalloc(IOPM_SIZE);
        if (!iopm) return (uint64_t)ENOMEM;
        // Initialize to deny all
        for (int i = 0; i < IOPM_SIZE; i++)
            iopm[i] = 0xFF;
        proc->iopm = iopm;
    }

    // Update IOPM bits: bit=0 means allow, bit=1 means deny
    for (unsigned long port = from; port < from + num; port++) {
        int byte_idx = port / 8;
        int bit_idx = port % 8;
        if (turn_on) {
            // Allow: clear bit
            proc->iopm[byte_idx] &= ~(1 << bit_idx);
        } else {
            // Deny: set bit
            proc->iopm[byte_idx] |= (1 << bit_idx);
        }
    }

    // Immediately update current CPU's TSS IOPM
    int cpu = proc->assigned_cpu;
    __memcpy(per_cpu_tss[cpu].iopm, proc->iopm, IOPM_SIZE);

    return 0;
}

// F_GETFL / F_SETFL for sys_fcntl
#define F_GETFL 1
#define F_SETFL 2

// sys_dup2(old_fd, new_fd) — syscall 27 (复制 fd)
uint64_t sys_dup2(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t) {
    int old_fd = (int)arg1;
    int new_fd = (int)arg2;

    if (old_fd < 0 || old_fd >= MAX_FD || new_fd < 0 || new_fd >= MAX_FD)
        return (uint64_t)EBADF;

    proc_t *proc = current_proc;
    if (proc->fd_table[old_fd].type == FD_NONE)
        return (uint64_t)EBADF;

    // Close new_fd if it's open
    if (proc->fd_table[new_fd].type != FD_NONE) {
        struct pipe *p = proc->fd_table[new_fd].pipe;
        if (p) {
            p->ref_count--;
            if (proc->fd_table[new_fd].flags & (O_WRONLY | O_RDWR)) {
                if (p->read_pid >= 0) wake_process(p->read_pid);
            }
            if (proc->fd_table[new_fd].flags & (O_RDONLY | O_RDWR)) {
                if (p->write_pid >= 0) wake_process(p->write_pid);
            }
            if (p->ref_count == 0) {
                kfree(p->buf);
                kfree(p);
            }
        }
        proc->fd_table[new_fd].type = FD_NONE;
        proc->fd_table[new_fd].flags = 0;
        proc->fd_table[new_fd].pipe = nullptr;
    }

    // Copy old_fd to new_fd
    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    if (proc->fd_table[new_fd].type == FD_PIPE) {
        proc->fd_table[new_fd].pipe->ref_count++;
    }

    return (uint64_t)new_fd;
}

// sys_fcntl(fd, cmd, arg) — syscall 28 (文件控制)
uint64_t sys_fcntl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    int fd = (int)arg1;
    int cmd = (int)arg2;
    int arg = (int)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)EBADF;

    proc_t *proc = current_proc;
    if (proc->fd_table[fd].type == FD_NONE) return (uint64_t)EBADF;

    switch (cmd) {
    case F_GETFL:
        return (uint64_t)proc->fd_table[fd].flags;
    case F_SETFL:
        proc->fd_table[fd].flags = arg;
        return 0;
    default:
        return (uint64_t)EINVAL;
    }
}

// Remove a PID from irq_owner[] (called by proc_reap)
void irq_owner_cleanup(pid_t pid) {
    for (int i = 0; i < MAX_IRQ_HANDLERS; i++) {
        if (irq_owner[i] == pid) {
            __atomic_store_n(&irq_owner[i], -1, __ATOMIC_RELEASE);
        }
    }
}

// sys_dma_alloc(size, vaddr_ptr, paddr_ptr) — syscall 29
// 分配物理连续、<4GB 的 DMA 缓冲区，映射到调用者地址空间
// Returns: 0 on success, positive errno on failure
uint64_t sys_dma_alloc(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    size_t size = (size_t)arg1;
    void **vaddr_ptr = (void **)arg2;
    uint64_t *paddr_ptr = (uint64_t *)arg3;

    if (size == 0) return (uint64_t)EINVAL;

    // Validate user pointers
    uint64_t vp = (uint64_t)vaddr_ptr;
    uint64_t pp = (uint64_t)paddr_ptr;
    if (!vp || vp >= 0xFFFFFFFF80000000ULL || vp + sizeof(void *) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;
    if (!pp || pp >= 0xFFFFFFFF80000000ULL || pp + sizeof(uint64_t) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)EFAULT;

    size = ALIGN_UP(size, PAGE_SIZE);
    size_t npages = size / PAGE_SIZE;

    // Allocate contiguous physical pages below 4GB
    Page *pages = bfc_alloc.alloc_page_low(npages);
    if (!pages) return (uint64_t)ENOMEM;

    uint64_t phys = page_to_phys(pages);
    proc_t *proc = current_proc;

    // Pick virtual address from mmap_brk
    uint64_t vaddr = proc->mmap_brk;
    uint64_t vaddr_end = vaddr + size;

    // Map pages into user address space using map_user_page_direct
    for (size_t i = 0; i < npages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_vaddr = vaddr + i * PAGE_SIZE;
        if (!map_user_page_direct((uint64_t *)phys_to_virt(proc->cr3), page_vaddr, page_phys,
                                  PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
            // Cleanup: unmap already-mapped pages
            if (i > 0) unmap_user_pages((uint64_t *)phys_to_virt(proc->cr3), vaddr, vaddr + i * PAGE_SIZE, i);
            bfc_alloc.free_page(pages, npages);
            return (uint64_t)ENOMEM;
        }
    }

    proc->mmap_brk = vaddr_end;

    // Track in mmap_regions for cleanup
    mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!region) {
        unmap_user_pages((uint64_t *)phys_to_virt(proc->cr3), vaddr, vaddr_end, npages);
        bfc_alloc.free_page(pages, npages);
        return (uint64_t)ENOMEM;
    }
    region->vaddr = vaddr;
    region->size = size;
    region->phys = phys;
    region->next = proc->mmap_regions;
    proc->mmap_regions = region;

    // Write results to user space
    *vaddr_ptr = (void *)vaddr;
    *paddr_ptr = phys;

    return 0;
}

// sys_dma_free(vaddr) — syscall 30
// 释放 DMA 缓冲区
// Returns: 0 on success, positive errno on failure
uint64_t sys_dma_free(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    uint64_t vaddr = (uint64_t)arg1;
    if (!vaddr) return (uint64_t)EINVAL;

    proc_t *proc = current_proc;

    // Find and remove from mmap_regions
    mmap_region **pp = &proc->mmap_regions;
    while (*pp) {
        mmap_region *r = *pp;
        if (r->vaddr == vaddr) {
            size_t npages = r->size / PAGE_SIZE;

            // Unmap from user address space
            unmap_user_pages((uint64_t *)phys_to_virt(proc->cr3), r->vaddr, r->vaddr + r->size, npages);

            // Free physical pages
            Page *page = BFCAllocator::frames + (r->phys / PAGE_SIZE);
            bfc_alloc.free_page(page, npages);

            // Remove from list
            *pp = r->next;
            kfree(r);
            return 0;
        }
        pp = &r->next;
    }

    return (uint64_t)EINVAL;
}
