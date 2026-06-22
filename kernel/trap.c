#include <stddef.h>
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
#include "kernel/socket.h"

// ===================== IRQ handler registry =====================
#define MAX_IRQ_HANDLERS 128
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS];

// ===================== IRQ owner (user-space driver binding) =====================
static pid_t irq_owner[MAX_IRQ_HANDLERS];

// ===================== Device table (dev_type → PID) =====================
static pid_t dev_table[DEV_TYPE_MAX];

// ===================== Kernel pre-allocated SHM table =====================
#define MAX_KERNEL_SHM 4
typedef struct kernel_shm_region {
    struct shm *shm;     // pointer to struct shm (NULL = unused)
    int      shm_id;     // identifier (-1 = unused)
} kernel_shm_region_t;
static kernel_shm_region_t kernel_shm_table[MAX_KERNEL_SHM];

// ===================== Signal trampoline =====================
uint64_t sig_trampoline_phys = 0;

void register_kernel_shm(int shm_id, struct shm *shm) {
    for (int i = 0; i < MAX_KERNEL_SHM; i++) {
        if (kernel_shm_table[i].shm_id == -1) {
            kernel_shm_table[i].shm_id = shm_id;
            kernel_shm_table[i].shm = shm;
            return;
        }
    }
}

void register_irq(int vec, irq_handler_t fn) {
  if (vec >= 0 && vec < MAX_IRQ_HANDLERS) {
    irq_handlers[vec] = fn;
  }
}

// Block I/O direction constants
#define BLOCK_DIR_READ  0
#define BLOCK_DIR_WRITE 1

// lseek whence constants
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ===================== File protocol for FD_FILE <-> fs_driver IPC =====================
// Structs must match driver/fs_driver.cc exactly (standard ABI, no packed)

#define FILE_CMD_READ      2
#define FILE_CMD_WRITE     3
#define FILE_CMD_CLOSE     4

typedef struct file_t_io_req {
    uint32_t cmd;              // FILE_CMD_READ / WRITE / CLOSE
    char     _path[256];       // unused for read/write/close (but must match layout)
    uint32_t _flags;           // unused
    uint32_t fs_fd;            // fs_driver session fd
    uint64_t offset;           // file offset for read/write
    uint32_t count;            // read/write length
    uint32_t _lba;
    uint32_t _readdir_offset;
    uint32_t _readdir_count;
} file_t_io_req;

typedef struct file_t_io_resp {
    int32_t  status;           // 0 = success, negative = errno
    uint32_t _fd;              // unused
    uint64_t file_size;        // file size (for write response)
    uint32_t count;            // bytes read/written
    uint32_t _total;           // unused
    // uint8_t data[] follows for READ responses
} file_t_io_resp;


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
        recv_msg_t *slot = (recv_msg_t *)target->recv_buf[target->recv_head];
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
      irq_handlers[tf->trapno] != NULL) {
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
  if (tf->trapno >= 32 && tf->trapno <= 127) {
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
    serial_puts("  #");
    serial_put_hex((uint64_t)i);
    serial_puts(" ");
    serial_put_hex((uint64_t)rbp[1]);
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
    sys_exit((uint64_t)-1, 0, 0, 0, 0, 0);
    // sys_exit does not return
  }
  // Kernel-mode exception: unrecoverable, halt
  halt();
}

// ===================== Timer IRQ handler =====================
static void timer_handler(trapframe_t *tf) {
  tick++;
  lapic_eoi();

  // Poll xHCI doorbell every ~10ms (every 10th tick at ~100Hz)
  // This ensures QEMU retries NAK'ed interrupt transfers
  if (tick % 10 == 0) xhci_poll();

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

  // Initialize kernel SHM table
  for (int i = 0; i < MAX_KERNEL_SHM; i++) {
    kernel_shm_table[i].shm_id = -1;
    kernel_shm_table[i].shm = NULL;
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

// ===================== Signal trampoline init =====================
void sig_init() {
    // Allocate one shared physical page for the signal trampoline.
    // The trampoline code is: mov rax, SYS_SIGRETURN; syscall
    // Encoding: 48 C7 C0 <31> 00 00 00  0F 05
    // SYS_SIGRETURN = 49 (0x31)
    Page *page = bfc_alloc_page(1);
    if (!page) {
        serial_puts("sig_init: failed to allocate trampoline page\n");
        return;
    }
    sig_trampoline_phys = (__force uint64_t)page_to_phys(page);
    uint8_t *vaddr = (__force uint8_t *)phys_to_virt((__force phys_addr_t)sig_trampoline_phys);

    // mov rax, 49  (SYS_SIGRETURN)
    vaddr[0] = 0x48;  // REX.W prefix
    vaddr[1] = 0xC7;  // MOV r64, imm32
    vaddr[2] = 0xC0;  // ModRM: rax
    vaddr[3] = 0x31;  // 49 = SYS_SIGRETURN (low byte)
    vaddr[4] = 0x00;
    vaddr[5] = 0x00;
    vaddr[6] = 0x00;

    // syscall
    vaddr[7] = 0x0F;
    vaddr[8] = 0x05;

    serial_printf("sig_init: trampoline at phys=%lx\n", sig_trampoline_phys);
}

// ===================== Syscall dispatch =====================
#define NR_SYSCALL 51
static uint64_t sys_debug_print(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
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
    sys_fb_info,        // 11
    sys_shm_create,     // 12
    sys_shm_attach,     // 13
    sys_pipe,           // 14
    sys_write,          // 15
    sys_read,           // 16
    sys_close,          // 17
    sys_load_dev,       // 18
    sys_notify,         // 19
    sys_gettime,        // 20
    sys_clock,          // 21
    sys_msg,            // 22
    sys_msg_resp,       // 23
    sys_ioperm,         // 24
    sys_dup2,           // 25
    sys_fcntl,          // 26
    sys_dma_alloc,      // 27
    sys_dma_free,       // 28
    sys_pci_dev_info,   // 29
    sys_block_io,       // 30
    sys_block_async,    // 31
    sys_open_dev,       // 32
    sys_install_fd_impl, // 33
    sys_socket,         // 34
    sys_bind,           // 35
    sys_listen,         // 36
    sys_accept,         // 37
    sys_connect,        // 38
    sys_socketpair,     // 39
    sys_sendmsg,        // 40
    sys_recvmsg,        // 41
    sys_shutdown,       // 42
    sys_poll,           // 43
    sys_lseek,          // 44
    sys_memfd_create,   // 45
    sys_ftruncate,      // 46
    sys_kill,           // 47
    sys_sigaction,      // 48
    sys_sigreturn,      // 49
    sys_debug_print,    // 50
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->rax < NR_SYSCALL) {
        tf->rax = syscall_table[tf->rax](
            tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
    } else {
        tf->rax = (uint64_t)ENOSYS;
    }
}

// ===================== SHM reference counting =====================
struct shm *shm_get(struct shm *shm) {
    if (!shm) return NULL;
    __atomic_add_fetch(&shm->ref_count, 1, __ATOMIC_RELAXED);
    return shm;
}

void shm_put(struct shm *shm) {
    if (!shm) return;
    int old = __atomic_fetch_sub(&shm->ref_count, 1, __ATOMIC_RELAXED);
    if (old == 1) {
        // Last reference released
        if (!(shm->flags & SHM_KERNEL)) {
            if (shm->page_list) {
                // Discrete pages: free each page individually
                for (int i = 0; i < shm->num_pages; i++) {
                    Page *p = &bfc_frames[PHY_TO_PAGE(shm->page_list[i])];
                    bfc_free_page(p, 1);
                }
                kfree(shm->page_list);
            } else if (shm->phys != 0 && shm->npages > 0) {
                // Contiguous pages
                Page *page = &bfc_frames[PHY_TO_PAGE(shm->phys)];
                bfc_free_page(page, shm->npages);
            }
        } else if (shm->page_list) {
            // SHM_KERNEL with page_list: free the page_list array only (not the pages)
            kfree(shm->page_list);
        }
        kfree(shm);
    }
}

// sys_getpid() — syscall 0
uint64_t sys_getpid(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    return (uint64_t)current_proc->pid;
}

// sys_yield() — syscall 1
uint64_t sys_yield(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    schedule();
    return 0;
}

// sys_recv(buf, data_buf, data_buf_len, timeout_ms) — syscall 2 (统一事件接收：IRQ/REQ/notify/MSG)
// timeout_ms=0: 无限等待; >0: 超时后唤醒
// 返回: 0=成功, 正数=errno
uint64_t sys_recv(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t _u1, uint64_t _u2) {
    void __user *buf = (void __user * __force)arg1;
    void __user *data_buf = (void __user * __force)arg2;
    size_t data_buf_len = (size_t)arg3;
    uint32_t timeout_ms = (uint32_t)arg4;

    // Validate user pointer
    uint64_t ptr = (__force uint64_t)buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    // Validate data_buf if provided
    if (data_buf && (data_buf_len == 0 ||
        (__force uint64_t)data_buf >= 0xFFFFFFFF80000000ULL ||
        (__force uint64_t)data_buf + data_buf_len > 0xFFFFFFFF80000000ULL))
        return (uint64_t)-EFAULT;

    proc_t *proc = current_proc;
    int cpu = proc->assigned_cpu;

    while (1) {
        // Try to dequeue a message from recv queue
        spin_lock(&proc->recv_lock);
        if (proc->recv_head != proc->recv_tail) {
            // Message available: copy to user buffer
            __memcpy((void __force *)buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
            // If this is an REQ request, record the caller PID for sys_resp
            recv_msg_t *msg = (recv_msg_t *)proc->recv_buf[proc->recv_tail];
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
                    recv_msg_t *umsg = (recv_msg_t __force *)buf;
                    umsg->msg.kmaddr = NULL;
                    umsg->msg.len = len;
                    proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
                    spin_unlock(&proc->recv_lock);
                    return (uint64_t)-EINVAL;
                }

                // Copy data to user buffer under current CR3
                __memcpy((void __force *)data_buf, kmaddr, len);
                kfree(kmaddr);

                // Rewrite recv_msg_t for user: put len in data field
                recv_msg_t *umsg = (recv_msg_t __force *)buf;
                umsg->msg.kmaddr = NULL;
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

        schedule();

        // Woken up: check if timed out
        if (proc->wait_timed_out) {
            // Re-check queue before returning timeout (race: message may have arrived)
            spin_lock(&proc->recv_lock);
            if (proc->recv_head != proc->recv_tail) {
                __memcpy((void __force *)buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
                recv_msg_t *msg = (recv_msg_t *)proc->recv_buf[proc->recv_tail];
                if (msg->type == RECV_REQ) {
                    proc->req_caller_pid = (pid_t)msg->src;
                }
                if (msg->type == RECV_MSG) {
                    void *kmaddr = msg->msg.kmaddr;
                    size_t len = msg->msg.len;
                    proc->msg_caller_pid = (pid_t)msg->src;

                    if (!data_buf || data_buf_len < len) {
                        kfree(kmaddr);
                        recv_msg_t *umsg = (recv_msg_t __force *)buf;
                        umsg->msg.kmaddr = NULL;
                        umsg->msg.len = len;
                        proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
                        spin_unlock(&proc->recv_lock);
                        return (uint64_t)-EINVAL;
                    }
                    __memcpy((void __force *)data_buf, kmaddr, len);
                    kfree(kmaddr);
                    recv_msg_t *umsg = (recv_msg_t __force *)buf;
                    umsg->msg.kmaddr = NULL;
                    umsg->msg.len = len;
                }
                proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
                spin_unlock(&proc->recv_lock);
                return 0;
            }
            spin_unlock(&proc->recv_lock);
            return (uint64_t)-ETIMEDOUT;
        }
        // Non-timeout wakeup: a message was enqueued, loop back to dequeue it
    }
}

// sys_req(pid, request, reply) — syscall 3 (同步 REQ)
// 向目标发送 64 字节请求，阻塞等待 reply
// 返回: 0=成功, 正数=errno
uint64_t sys_req(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    pid_t target_pid = (pid_t)arg1;
    void __user *request = (void __user * __force)arg2;
    void __user *reply = (void __user * __force)arg3;

    // Validate target PID
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)-ESRCH;

    // Validate user pointers
    uint64_t req_ptr = (__force uint64_t)request;
    uint64_t rep_ptr = (__force uint64_t)reply;
    if (!req_ptr || req_ptr >= 0xFFFFFFFF80000000ULL ||
        req_ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;
    if (!rep_ptr || rep_ptr >= 0xFFFFFFFF80000000ULL ||
        rep_ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) return (uint64_t)-ESRCH;

    // Build RECV_REQ message
    uint8_t msg[RECV_MSG_SIZE];
    recv_msg_t *hdr = (recv_msg_t *)msg;
    hdr->type = RECV_REQ;
    hdr->src = (uint32_t)current_proc->pid;
    // Copy request payload from user space
    __memcpy(hdr->data, (const void __force *)request, 56);

    // Enqueue to target's recv queue
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        return (uint64_t)-EBUSY;  // queue full
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
    proc->state = BLOCKED;
    proc->wait_event = WAIT_REQ_REPLY;
    proc->wait_timed_out = 0;
    proc->wait_deadline = 0;
    proc->req_target_pid = target_pid;
    proc->req_reply_buf = reply;
    proc->req_result = 0;

    schedule();

    // Woken up by sys_resp or proc_reap
    if (proc->req_result != 0) return (uint64_t)proc->req_result;
    return 0;
}

// sys_resp(reply) — syscall 4 (回复当前 REQ 调用者)
// 返回: 0=成功, 正数=errno
uint64_t sys_resp(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    void __user *reply = (void __user * __force)arg1;

    // Validate user pointer
    uint64_t ptr = (__force uint64_t)reply;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    proc_t *proc = current_proc;
    pid_t caller_pid = proc->req_caller_pid;
    if (caller_pid < 0 || caller_pid >= MAX_PROC) return (uint64_t)-EINVAL;

    proc_t *caller = &procs[caller_pid];
    if (caller->pid != caller_pid) return (uint64_t)-ESRCH;

    // Copy reply data to caller's reply buffer (user space)
    // We must first copy to a kernel buffer under the server's CR3,
    // then switch to caller's CR3 to write to the caller's user-space buffer.
    uint8_t kbuf[RECV_MSG_SIZE];
    __memcpy(kbuf, (const void __force *)reply, RECV_MSG_SIZE);

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)caller->cr3) : "memory");
    __memcpy((void __force *)caller->req_reply_buf, kbuf, RECV_MSG_SIZE);
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
uint64_t sys_irq_bind(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)-EINVAL;
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
uint64_t sys_exit(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
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
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
        // Notify parent via SIGCHLD (replaces old RECV_NOTIFY)
        {
            proc_t *parent = &procs[proc->parent_pid];
            __atomic_or_fetch(&parent->sig.pending, 1ULL << SIGCHLD, __ATOMIC_RELEASE);

            // Wake parent if in WAIT_CHILD (waitpid still works)
            int pcpu = parent->assigned_cpu;
            spin_lock(&cpu_locals[pcpu].scheduler_lock);
            if (parent->state == BLOCKED &&
                (parent->wait_event == WAIT_CHILD ||
                 parent->wait_event == WAIT_RECV)) {
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

uint64_t sys_waitpid(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    pid_t pid = (pid_t)arg1;
    int32_t __user *exit_code_ptr = (int32_t __user * __force)arg2;

    if (pid == -1) {
        // Wait for any child to become ZOMBIE
        while (1) {
            // Scan for a ZOMBIE child
            spin_lock(&procs_lock);
            proc_t *zombie = NULL;
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
                    uint64_t ptr_val = (__force uint64_t)exit_code_ptr;
                    if (ptr_val < 0xFFFFFFFF80000000ULL && ptr_val &&
                        (ptr_val + sizeof(int32_t) - 1) < 0xFFFFFFFF80000000ULL)
                        *(__force int32_t *)exit_code_ptr = zombie->exit_code;
                }
                proc_reap(zombie);
                return (uint64_t)zpid;
            }
            spin_unlock(&procs_lock);

            // No zombie child: block on WAIT_CHILD
            current_proc->wait_event = WAIT_CHILD;
            current_proc->state = BLOCKED;
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
        uint64_t ptr_val = (__force uint64_t)exit_code_ptr;
        if (ptr_val >= 0xFFFFFFFF80000000ULL || !ptr_val || (ptr_val + sizeof(int32_t) - 1) >= 0xFFFFFFFF80000000ULL)
            return 0;  // EFAULT
        *(__force int32_t *)exit_code_ptr = child->exit_code;
    }
    proc_reap(child);
    return (uint64_t)pid;
}

uint64_t sys_spawn(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const uint8_t __user *elf_data_user = (const uint8_t __user * __force)arg1;
    uint64_t elf_size = arg2;

    // Basic parameter validation
    if (!elf_data_user || elf_size == 0)
        return (uint64_t)-EINVAL;

    // Validate user pointer range
    uint64_t ptr_start = (__force uint64_t)elf_data_user;
    uint64_t ptr_end = ptr_start + elf_size;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    // Copy ELF data from user space to kernel buffer to prevent TOCTOU
    uint8_t *elf_buf = (uint8_t *)kmalloc(elf_size);
    if (!elf_buf) return (uint64_t)-ENOMEM;
    __memcpy(elf_buf, (const void __force *)elf_data_user, elf_size);

    // Create child process from ELF
    proc_t *child = process_create_elf(elf_buf, elf_size);
    kfree(elf_buf);
    if (!child) return (uint64_t)-ENOMEM;

    child->parent_pid = current_proc->pid;

    // Inherit all open fds from parent (including fd 0/1 for pipe stdin/stdout)
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (current_proc->fd_table[fd].type != FD_NONE) {
            child->fd_table[fd] = current_proc->fd_table[fd];
            if (child->fd_table[fd].type == FD_PIPE) {
                child->fd_table[fd].pipe->ref_count++;
            } else if (child->fd_table[fd].type == FD_FILE) {
                child->fd_table[fd].file_data.ref_count++;
            }
            // FD_DEV/SHM: copied by struct assignment, no ref_count needed
        }
    }

    return (uint64_t)child->pid;
}

// sys_mmap(addr, size, prot, flags, fd, offset) — syscall 9 (内存映射)
// MAP_SHARED + fd ≥ 0: SHM fd 映射
// MAP_PHYSICAL: offset=phys_addr, fd=-1
// MAP_ANONYMOUS: fd=-1
// Returns: mapped address on success, 0 on failure
#define MAP_PHYSICAL_KERNEL 0x80000000

uint64_t sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg3; // addr, prot — not yet used
    size_t size = (size_t)arg2;
    int flags = (int)arg4;
    int fd = (int)arg5;
    uint64_t offset = arg6;

    proc_t *proc = current_proc;
    uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

    // MAP_SHARED (0x01) + fd ≥ 0: SHM fd mapping
    if ((flags & 0x01) && fd >= 0) {
        // Validate fd
        if (fd >= MAX_FD || proc->fd_table[fd].type != FD_SHM)
            return 0;
        struct shm *shm = proc->fd_table[fd].shm;
        if (!shm) return 0;

        // Total allocated pages: contiguous + discrete
        size_t npages = shm->npages;
        size_t list_pages = shm->page_list ? (size_t)shm->num_pages : 0;
        size_t total_pages = npages + list_pages;
        size = total_pages * PAGE_SIZE;

        // Map SHM pages into user address space at mmap_brk
        uint64_t vaddr = proc->mmap_brk;
        uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

        for (size_t i = 0; i < total_pages; i++) {
            uint64_t page_phys;
            if (i < npages) {
                // From contiguous phys region
                page_phys = shm->phys + i * PAGE_SIZE;
            } else {
                // From discrete page_list
                page_phys = shm->page_list[i - npages];
            }
            if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                      page_phys, pte_flags)) {
                for (size_t j = 0; j < i; j++)
                    unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
                return 0;
            }
        }

        mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
        if (!region) {
            for (size_t i = 0; i < total_pages; i++)
                unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE, 1);
            return 0;
        }

        region->vaddr = vaddr;
        region->size = size;
        region->phys = 0;
        region->shm_obj = shm_get(shm);  // +1 ref for mmap
        region->next = proc->mmap_regions;
        proc->mmap_regions = region;
        proc->mmap_brk = vaddr + size;

        return vaddr;
    }

    // Anonymous or MAP_PHYSICAL: size must be non-zero
    if (size == 0) return 0;

    // MAP_PHYSICAL
    if (flags & MAP_PHYSICAL_KERNEL) {
        // MAP_PHYSICAL: map physical address range to user address space
        // Use mmap_phys_brk (high fixed base) to avoid conflict with SHM/heap
        uint64_t vaddr = proc->mmap_phys_brk;
        uint64_t phys_start = ALIGN_DOWN(offset, PAGE_SIZE);
        uint64_t phys_end = ALIGN_UP(offset + size, PAGE_SIZE);
        size_t npages = (phys_end - phys_start) / PAGE_SIZE;

        uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

        for (size_t i = 0; i < npages; i++) {
            if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                      phys_start + i * PAGE_SIZE, pte_flags)) {
                serial_puts("mmap PHYSICAL: map failed at i=");
                serial_put_hex((uint64_t)i);
                serial_puts("\n");
                for (size_t j = 0; j < i; j++)
                    unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
                return 0;
            }
        }

        mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
        if (!region) {
            for (size_t i = 0; i < npages; i++)
                unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE, 1);
            return 0;
        }

        region->vaddr = vaddr;
        region->size = npages * PAGE_SIZE;
        region->phys = phys_start;  // non-zero = MAP_PHYSICAL, don't free in proc_reap
        region->shm_obj = NULL;
        region->next = proc->mmap_regions;
        proc->mmap_regions = region;
        proc->mmap_phys_brk = vaddr + npages * PAGE_SIZE;

        return vaddr;
    }

    // Anonymous private mapping: allocate new pages
    size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t vaddr = proc->mmap_brk;
    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

    size_t npages = size / PAGE_SIZE;
    uint64_t *phys_pages = (uint64_t *)kmalloc(npages * sizeof(uint64_t));
    if (!phys_pages) return 0;

    size_t mapped = 0;
    for (size_t i = 0; i < npages; i++) {
        Page *page = bfc_alloc_page(1);
        if (!page) {
            for (size_t j = 0; j < mapped; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }
            kfree(phys_pages);
            return 0;
        }
        phys_pages[i] = (__force uint64_t)page_to_phys(page);
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, phys_pages[i], pte_flags)) {
            bfc_free_page(&bfc_frames[PHY_TO_PAGE(phys_pages[i])], 1);
            for (size_t j = 0; j < mapped; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }
            kfree(phys_pages);
            return 0;
        }
        mapped++;
    }

    mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
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
    region->shm_obj = NULL;
    region->next = proc->mmap_regions;
    proc->mmap_regions = region;
    proc->mmap_brk = vaddr + size;

    kfree(phys_pages);
    return vaddr;
}

// sys_munmap(addr, size) — syscall 10 (解除内存映射)
// Returns: 0 on success, positive errno on failure
uint64_t sys_munmap(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    uint64_t addr = arg1;
    size_t size = (size_t)arg2;

    if (size == 0) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;
    uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

    // 查找匹配的 mmap_region_t
    mmap_region_t **pp = &proc->mmap_regions;
    while (*pp) {
        if ((*pp)->vaddr == addr) {
            mmap_region_t *region = *pp;
            size = region->size;

            // 逐页解映射（不释放物理页 — SHM页由shm_put管理，匿名页在下面释放）
            size_t npages = size / PAGE_SIZE;
            for (size_t i = 0; i < npages; i++) {
                uint64_t va = addr + i * PAGE_SIZE;
                unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
            }

            // Release SHM reference if this was an SHM mapping
            if (region->shm_obj) {
                shm_put(region->shm_obj);
            }
            // else: anonymous/MAP_PHYSICAL — physical pages already freed by PML4 walk in proc_reap
            // (for munmap, anonymous pages are leaked intentionally — proc_reap handles them)

            // 从链表删除
            *pp = region->next;
            kfree(region);
            return 0;
        }
        pp = &(*pp)->next;
    }

    return (uint64_t)-EINVAL;
}

// ===================== notify_and_wake =====================
// Shared notification helper: enqueue recv_msg_t and wake target if WAIT_RECV.
// Used by AHCI IRQ completion, sys_notify, sys_req, sys_msg, IRQ dispatch.
void notify_and_wake(pid_t target_pid, recv_msg_t *msg) {
    if (target_pid < 0 || target_pid >= MAX_PROC) {
        serial_printf("notify_and_wake: bad pid %d\n", target_pid);
        return;
    }
    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) {
        serial_printf("notify_and_wake: pid mismatch %d vs %d\n", target_pid, target->pid);
        return;
    }

    // Enqueue message
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        // Queue full — drop message
        spin_unlock(&target->recv_lock);
        return;
    }
    __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
    target->recv_head = next;
    spin_unlock(&target->recv_lock);

    // Wake target if blocked on WAIT_RECV
    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->pid == target_pid &&
        target->state == BLOCKED &&
        target->wait_event == WAIT_RECV) {
        if (target->wait_deadline != 0) {
            // Remove from timer queue
            list_remove(&target->wait_node);
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

// Forward declaration of sys_msg_to (defined later in this file)
int64_t sys_msg_to(pid_t target_pid, void *msg_buf, size_t msg_len,
                   void *reply_buf, size_t reply_len);

// ===================== kernel_msg_send =====================
// Kernel-internal: send a msg to a user process and wait for reply.
// req may point to kernel stack or kmalloc memory; resp may be NULL (fire-and-forget).
// Returns: 0 on success, negative errno on error.
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                     void *resp, size_t resp_len) {
    // Use a dummy buffer if resp is NULL (e.g. CLOSE notifications)
    uint8_t dummy[64];
    if (!resp) {
        resp = dummy;
        resp_len = sizeof(dummy);
    }
    return (int)sys_msg_to(target_pid, (void*)req, req_len, resp, resp_len);
}

// ===================== sys_block_io =====================
// Unified block I/O: dir=0 read, dir=1 write. Synchronous polling path.
// Returns EBUSY if async request is active.
uint64_t sys_block_io(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t _u1, uint64_t _u2) {
    uint32_t lba = (uint32_t)arg1;
    void __user *buf = (void __user * __force)arg2;
    uint32_t count = (uint32_t)arg3;
    uint8_t dir = (uint8_t)arg4;

    uint64_t ptr = (__force uint64_t)buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    uint64_t end = ptr + (uint64_t)count * 512;
    if (end < ptr || end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    if (count == 0 || count > AHCI_MAX_SECTORS)
        return (uint64_t)-EINVAL;

    // Safety: refuse if async request is active (would deadlock with polling + IRQ)
    if (ahci_is_busy())
        return (uint64_t)-EBUSY;

    uint64_t flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    int rc;
    if (dir == BLOCK_DIR_WRITE) {
        rc = ahci_write_lba(lba, count, (const void __force *)buf);
    } else {
        rc = ahci_read_lba(lba, count, (void __force *)buf);
    }
    spin_unlock_irqrestore(&ahci_lock, flags);

    return (uint64_t)(rc < 0 ? -rc : 0);
}

// sys_block_async(lba, buf, count, dir) — syscall 33
// Async block I/O: returns cookie (>0) on success, positive errno on error.
// Completion delivered via RECV_NOTIFY with cookie+result+lba+count in data.
uint64_t sys_block_async(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t _u1, uint64_t _u2) {
    uint32_t lba = (uint32_t)arg1;
    void __user *buf = (void __user * __force)arg2;
    uint32_t count = (uint32_t)arg3;
    uint8_t dir = (uint8_t)arg4;

    int ret = ahci_submit_async(lba, (void __force *)buf, count, dir);
    return (uint64_t)ret;
}

// sys_debug_print(buf, len) — temporary debug: print user string to serial
// syscall 50
uint64_t sys_debug_print(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *buf = (const char __user * __force)arg1;
    size_t len = (size_t)arg2;
    if (!buf || len > 256) return (uint64_t)-EINVAL;
    char kbuf[257];
    __memcpy(kbuf, (const void __force *)buf, len);
    kbuf[len] = '\0';
    serial_printf("DBG:%s", kbuf);
    return 0;
}

// sys_open_dev(dev_type) — syscall 34 (open device node, returns FD_DEV fd)
// Returns: (fd | target_pid << 32) on success, negative errno on failure
// Caller extracts: fd = (int32_t)(result & 0xFFFFFFFF), pid = (pid_t)(result >> 32)
uint64_t sys_open_dev(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    int dev_type = (int)arg1;

    if (dev_type <= DEV_NONE || dev_type >= DEV_TYPE_MAX)
        return (uint64_t)-EINVAL;

    pid_t target_pid = dev_table[dev_type];
    if (target_pid <= 0) {
        return (uint64_t)-ENOENT;
    }

    proc_t *proc = current_proc;

    // Find free fd slot (skip 0/1 reserved for stdin/stdout)
    int fd = -1;
    for (int i = 2; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return (uint64_t)-EMFILE;

    proc->fd_table[fd].type = FD_DEV;
    proc->fd_table[fd].flags = O_RDWR;
    proc->fd_table[fd].pipe = NULL;
    proc->fd_table[fd].target_pid = target_pid;

    // Pack fd and PID into one return value — eliminates need for sys_lookup_dev
    return (uint64_t)fd | ((uint64_t)target_pid << 32);
}

// sys_install_fd(fs_pid, fs_fd, offset, flags, file_size) — syscall 35
// Register an FD_FILE fd in the kernel fd_table.
// Returns: fd (>=3) on success, negative errno on failure
// Note: named sys_install_fd_impl to avoid conflict with static inline wrapper in common/syscall.h
uint64_t sys_install_fd_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t _u1) {
    pid_t fs_pid = (pid_t)arg1;
    int32_t fs_fd = (int32_t)arg2;
    uint64_t offset = arg3;
    int flags = (int)arg4;
    uint64_t file_size = arg5;

    if (fs_pid < 0 || fs_pid >= MAX_PROC) return (uint64_t)-EINVAL;
    if (fs_fd < 0) return (uint64_t)-EINVAL;
    if (flags & ~(O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK)) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;

    // Find free fd slot (start from 3 to reserve 0/1/2 for stdin/stdout/stderr)
    int fd = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return (uint64_t)-EMFILE;

    proc->fd_table[fd].type = FD_FILE;
    proc->fd_table[fd].flags = flags;
    proc->fd_table[fd].file_data.fs_pid = fs_pid;
    proc->fd_table[fd].file_data.fs_fd = fs_fd;
    proc->fd_table[fd].file_data.offset = offset;
    proc->fd_table[fd].file_data.file_size = file_size;
    proc->fd_table[fd].file_data.ref_count = 1;

    return (uint64_t)fd;
}

// sys_fb_info(buf) — syscall 11 (获取 framebuffer 信息)
// Returns: 0 on success, positive errno on failure
uint64_t sys_fb_info(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    void __user *user_buf = (void __user * __force)arg1;
    if (!user_buf) return (uint64_t)-EINVAL;

    // Validate user pointer range
    uint64_t ptr = (__force uint64_t)user_buf;
    if (ptr >= 0xFFFFFFFF80000000ULL || ptr + sizeof(kms_fb_info_t) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    serial_puts("sys_fb_info: fb_phys=");
    serial_put_hex(g_fb_info.fb_phys);
    serial_puts(" fb_size=");
    serial_put_hex(g_fb_info.fb_size);
    serial_puts("\n");
    __memcpy((void __force *)user_buf, &g_fb_info, sizeof(kms_fb_info_t));
    return 0;
}

// sys_shm_create(size) — syscall 12 (创建共享内存，返回 fd)
// Returns: fd (≥2) on success, 0 on failure
uint64_t sys_shm_create(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    size_t size = (size_t)arg1;
    if (size == 0) return 0;
    size = ALIGN_UP(size, PAGE_SIZE);
    size_t npages = size / PAGE_SIZE;

    proc_t *proc = current_proc;

    // Allocate physical pages
    Page *pages = bfc_alloc_page(npages);
    if (!pages) return 0;
    uint64_t phys = (__force uint64_t)page_to_phys(pages);

    // Zero the pages
    uint8_t *vptr = (__force uint8_t *)phys_to_virt((__force phys_addr_t)phys);
    __memset(vptr, 0, size);

    // Allocate shm struct
    struct shm *shm = (struct shm *)kmalloc(sizeof(struct shm));
    if (!shm) {
        bfc_free_page(pages, npages);
        return 0;
    }
    shm->phys = phys;
    shm->npages = npages;
    shm->file_size = size;  // logical size = allocated size
    shm->ref_count = 1;  // fd holds initial reference
    shm->flags = 0;
    shm->seals = 0;
    shm->name[0] = '\0';  // no name by default
    shm->page_list = NULL;
    shm->num_pages = 0;

    // Find free fd slot (skip 0/1 reserved for stdin/stdout)
    int fd = -1;
    for (int i = 2; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        kfree(shm);
        bfc_free_page(pages, npages);
        return 0;
    }

    proc->fd_table[fd].type = FD_SHM;
    proc->fd_table[fd].flags = O_RDWR;
    proc->fd_table[fd].shm = shm;

    return (uint64_t)fd;
}

// sys_shm_attach(id, mode) — syscall 13 (附加共享内存，返回 fd)
// mode=0: id is target_pid, attach target's first FD_SHM
// mode=1: id is kernel SHM ID, attach kernel pre-allocated SHM via struct shm*
// Returns: fd (≥2) on success, 0 on failure
// Transitional: caller must sys_mmap(fd, ...) to get vaddr
uint64_t sys_shm_attach(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int mode = (int)arg2;
    proc_t *proc = current_proc;
    struct shm *target_shm = NULL;

    if (mode == 1) {
        // Kernel SHM mode: arg1 is kernel SHM ID
        int shm_id = (int)arg1;
        for (int i = 0; i < MAX_KERNEL_SHM; i++) {
            if (kernel_shm_table[i].shm_id == shm_id) {
                target_shm = kernel_shm_table[i].shm;
                break;
            }
        }
        if (!target_shm) return 0;
    } else {
        // mode=0: find target process's first FD_SHM
        pid_t target_pid = (pid_t)arg1;
        if (target_pid < 0 || target_pid >= MAX_PROC) return 0;

        proc_t *target = &procs[target_pid];
        if (target->pid != target_pid) return 0;

        // Scan target's fd_table for FD_SHM
        for (int i = 0; i < MAX_FD; i++) {
            if (target->fd_table[i].type == FD_SHM) {
                target_shm = target->fd_table[i].shm;
                break;
            }
        }
        if (!target_shm) return 0;
    }

    // Bump ref_count for the new fd
    shm_get(target_shm);

    // Find free fd slot (skip 0/1 reserved for stdin/stdout)
    int fd = -1;
    for (int i = 2; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        shm_put(target_shm);
        return 0;
    }

    proc->fd_table[fd].type = FD_SHM;
    proc->fd_table[fd].flags = O_RDWR;
    proc->fd_table[fd].shm = target_shm;

    return (uint64_t)fd;
}

// ===================== memfd_create / ftruncate =====================

// sys_memfd_create(name, flags) — syscall 45 (Linux-compatible memfd_create)
// Returns: fd (≥2) on success, 0 on failure
uint64_t sys_memfd_create(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *user_name = (const char __user * __force)arg1;
    unsigned int flags = (unsigned int)arg2;

    // Validate flags: only known flags allowed
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return 0;

    proc_t *proc = current_proc;

    // Allocate shm struct (empty — no physical pages yet)
    struct shm *shm = (struct shm *)kmalloc(sizeof(struct shm));
    if (!shm) return 0;

    shm->phys = 0;
    shm->npages = 0;
    shm->file_size = 0;
    shm->ref_count = 1;  // fd holds initial reference
    shm->flags = (flags & MFD_ALLOW_SEALING) ? SHM_SEALED : 0;
    shm->seals = 0;
    shm->page_list = NULL;
    shm->num_pages = 0;

    // Copy name from user space (up to 31 chars + null)
    if (user_name) {
        uint64_t uptr = (__force uint64_t)user_name;
        if (uptr >= 0xFFFFFFFF80000000ULL) {
            kfree(shm);
            return 0;  // -EFAULT
        }
        // Read name byte by byte to avoid crossing page boundary issues
        int i;
        for (i = 0; i < 31; i++) {
            char c;
            __memcpy(&c, (const void __force *)(uptr + i), 1);
            if (c == '\0') break;
            shm->name[i] = c;
        }
        shm->name[i] = '\0';
    } else {
        shm->name[0] = '\0';
    }

    // Find free fd slot (skip 0/1 reserved for stdin/stdout)
    int fd = -1;
    for (int i = 2; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        kfree(shm);
        return 0;
    }

    proc->fd_table[fd].type = FD_SHM;
    proc->fd_table[fd].flags = O_RDWR | ((flags & MFD_CLOEXEC) ? FD_CLOEXEC : 0);
    proc->fd_table[fd].shm = shm;

    return (uint64_t)fd;
}

// Helper: allocate one page and add to shm's page_list
// Returns physical address on success, 0 on failure
static uint64_t shm_add_page(struct shm *shm) {
    Page *page = bfc_alloc_page(1);
    if (!page) return 0;
    uint64_t phys = (__force uint64_t)page_to_phys(page);

    // Zero the page
    __memset((__force void *)phys_to_virt((__force phys_addr_t)phys), 0, PAGE_SIZE);

    if (!shm->page_list) {
        // First discrete page: allocate page_list array
        // Start with room for 16 pages, grow as needed
        int initial_cap = 16;
        shm->page_list = (uint64_t *)kmalloc((size_t)initial_cap * sizeof(uint64_t));
        if (!shm->page_list) {
            bfc_free_page(page, 1);
            return 0;
        }
        shm->num_pages = 0;  // will be incremented after storing
        // Don't store yet — caller will use shm->num_pages as index
    }

    // Check if we need to grow the page_list array
    // Simple growth: realloc when full (check if num_pages is a multiple of 16)
    if (shm->num_pages > 0 && (shm->num_pages % 16 == 0)) {
        int new_cap = shm->num_pages + 16;
        uint64_t *new_list = (uint64_t *)kmalloc((size_t)new_cap * sizeof(uint64_t));
        if (!new_list) {
            bfc_free_page(page, 1);
            return 0;
        }
        __memcpy(new_list, shm->page_list, (size_t)shm->num_pages * sizeof(uint64_t));
        kfree(shm->page_list);
        shm->page_list = new_list;
    }

    return phys;
}

// sys_ftruncate(fd, size) — syscall 46 (set shm size, allocate/free pages)
// Returns: 0 on success, positive errno on failure
uint64_t sys_ftruncate(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int fd = (int)arg1;
    int64_t size = (int64_t)arg2;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    proc_t *proc = current_proc;
    if (proc->fd_table[fd].type != FD_SHM) return (uint64_t)-EINVAL;
    if (!proc->fd_table[fd].shm) return (uint64_t)-EBADF;

    struct shm *shm = proc->fd_table[fd].shm;

    if (size < 0) return (uint64_t)-EINVAL;

    size_t new_size = (size_t)size;
    size_t new_npages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t old_total = shm->page_list ? (size_t)shm->num_pages : shm->npages;

    if (new_npages > old_total) {
        // === EXPAND ===
        if (shm->seals & F_SEAL_GROW)
            return (uint64_t)-EPERM;

        size_t extra = new_npages - old_total;

        if (!shm->page_list && shm->npages == 0) {
            // First allocation: try contiguous
            Page *pages = bfc_alloc_page(new_npages);
            if (pages) {
                uint64_t phys = (__force uint64_t)page_to_phys(pages);
                __memset((__force void *)phys_to_virt((__force phys_addr_t)phys), 0, new_npages * PAGE_SIZE);
                shm->phys = phys;
                shm->npages = new_npages;
                shm->file_size = new_size;
                return 0;
            }
            // Contiguous failed — fall through to page_list
        }

        if (!shm->page_list && shm->npages > 0) {
            // Switching from contiguous to page_list mode:
            // Allocate page_list, copy existing phys pages into it
            size_t total = shm->npages + extra;
            int list_cap = (int)((total + 15) / 16 * 16);  // round up to multiple of 16
            if (list_cap < 16) list_cap = 16;
            shm->page_list = (uint64_t *)kmalloc((size_t)list_cap * sizeof(uint64_t));
            if (!shm->page_list) return (uint64_t)-ENOMEM;

            for (size_t i = 0; i < shm->npages; i++) {
                shm->page_list[i] = shm->phys + i * PAGE_SIZE;
            }
            shm->num_pages = (int)shm->npages;
            shm->phys = 0;
            shm->npages = 0;

            // Add extra pages
            for (size_t i = 0; i < extra; i++) {
                uint64_t pphys = shm_add_page(shm);
                if (!pphys) {
                    // Cleanup: free pages we already allocated in this batch
                    for (size_t j = 0; j < i; j++) {
                        Page *p = &bfc_frames[PHY_TO_PAGE(shm->page_list[shm->num_pages - 1 - j])];
                        bfc_free_page(p, 1);
                    }
                    kfree(shm->page_list);
                    shm->page_list = NULL;
                    shm->num_pages = 0;
                    return (uint64_t)-ENOMEM;
                }
                shm->page_list[shm->num_pages] = pphys;
                shm->num_pages++;
            }
        } else if (shm->page_list) {
            // Already in page_list mode: add more discrete pages
            for (size_t i = 0; i < extra; i++) {
                uint64_t pphys = shm_add_page(shm);
                if (!pphys) {
                    // Cleanup partial
                    for (size_t j = 0; j < i; j++) {
                        Page *p = &bfc_frames[PHY_TO_PAGE(shm->page_list[--shm->num_pages])];
                        bfc_free_page(p, 1);
                    }
                    return (uint64_t)-ENOMEM;
                }
                // shm_add_page reallocs page_list if needed, but we need to
                // re-get the pointer since kmalloc may move it
                // We store at shm->page_list[shm->num_pages]
                shm->page_list[shm->num_pages] = pphys;
                shm->num_pages++;
            }
        }

        shm->file_size = new_size;

    } else if (new_npages < old_total) {
        // === SHRINK ===
        if (shm->seals & F_SEAL_SHRINK)
            return (uint64_t)-EPERM;

        if (shm->page_list) {
            // Free trailing pages from page_list
            int free_start = (int)new_npages;
            for (int i = free_start; i < shm->num_pages; i++) {
                Page *p = &bfc_frames[PHY_TO_PAGE(shm->page_list[i])];
                bfc_free_page(p, 1);
            }
            shm->num_pages = (int)new_npages;
            if (shm->num_pages == 0) {
                kfree(shm->page_list);
                shm->page_list = NULL;
            }
        } else {
            // Free contiguous pages from the end
            uint64_t free_phys = shm->phys + new_npages * PAGE_SIZE;
            size_t free_npages = shm->npages - new_npages;
            Page *page = &bfc_frames[PHY_TO_PAGE(free_phys)];
            bfc_free_page(page, free_npages);
            shm->npages = new_npages;
        }

        shm->file_size = new_size;
    } else {
        // Same number of pages — just update file_size
        shm->file_size = new_size;
    }

    return 0;
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
        (target->wait_event == WAIT_PIPE || target->wait_event == WAIT_POLL)) {
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

// sys_pipe(fd_ptr) — syscall 14 (创建 pipe，写 [read_fd, write_fd] 到用户指针)
uint64_t sys_pipe(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    int __user *fd_ptr = (int __user * __force)arg1;

    // Validate user pointer
    uint64_t ptr = (__force uint64_t)fd_ptr;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + 2 * sizeof(int) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    proc_t *proc = current_proc;

    // Find two free fd slots (skip 0/1, reserved for stdin/stdout)
    int read_fd = -1, write_fd = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            if (read_fd < 0) read_fd = i;
            else if (write_fd < 0) { write_fd = i; break; }
        }
    }
    if (read_fd < 0 || write_fd < 0) return (uint64_t)-ENOMEM;

    // Allocate pipe buffer (1 page)
    uint8_t *buf = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!buf) return (uint64_t)-ENOMEM;

    // Allocate pipe struct
    struct pipe *p = (struct pipe *)kmalloc(sizeof(struct pipe));
    if (!p) { kfree(buf); return (uint64_t)-ENOMEM; }

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
    ((__force int *)fd_ptr)[0] = read_fd;
    ((__force int *)fd_ptr)[1] = write_fd;

    return 0;
}

// sys_write(fd, buf, len) — syscall 15 (向 fd 写入数据)
// FD_PIPE: 直写 kernel ring buffer
// FD_FILE: 通过 kernel_msg_send 代理到 fs_driver
uint64_t sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    const char __user *buf = (const char __user * __force)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EINVAL;
    if (current_proc->fd_table[fd].type == FD_NONE) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;

    // ===== FD_FILE: proxy to fs_driver =====
    if (proc->fd_table[fd].type == FD_FILE) {
        if (!(proc->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;

        // Validate user buf pointer
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        // Clamp to msg size limit (64KB total msg, minus req header)
        size_t max_data = 65536 - sizeof(file_t_io_req);
        if (len > max_data) len = max_data;
        if (len == 0) return 0;

        // Build write request: file_t_io_req + inline data
        size_t msg_len = sizeof(file_t_io_req) + len;
        uint8_t *msg_buf = (uint8_t *)kmalloc(msg_len);
        if (!msg_buf) return (uint64_t)-ENOMEM;

        file_t_io_req *req = (file_t_io_req *)msg_buf;
        req->cmd = FILE_CMD_WRITE;
        req->fs_fd = proc->fd_table[fd].file_data.fs_fd;
        req->offset = proc->fd_table[fd].file_data.offset;
        req->count = (uint32_t)len;
        __memcpy(msg_buf + sizeof(file_t_io_req), (const void __force *)buf, len);

        // Reply buffer
        file_t_io_resp resp;
        int64_t ret = sys_msg_to(proc->fd_table[fd].file_data.fs_pid,
                                  msg_buf, msg_len, &resp, sizeof(resp));
        kfree(msg_buf);

        if (ret < 0) return (uint64_t)(-ret);

        if (resp.status != 0) return (uint64_t)(-resp.status);

        size_t written = resp.count;
        proc->fd_table[fd].file_data.offset += written;
        if (proc->fd_table[fd].file_data.file_size < proc->fd_table[fd].file_data.offset)
            proc->fd_table[fd].file_data.file_size = proc->fd_table[fd].file_data.offset;
        return (uint64_t)written;
    }

    // ===== FD_SOCKET: delegate to sock_write =====
    if (proc->fd_table[fd].type == FD_SOCKET) {
        if (!(proc->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct unix_sock *sock = proc->fd_table[fd].sock;
        if (!sock) return (uint64_t)-EBADF;
        int64_t ret = sock_write(sock, (const void __force *)buf, len);
        return (uint64_t)ret;
    }

    // ===== FD_PIPE: existing path =====
    if (!(proc->fd_table[fd].flags & (O_WRONLY | O_RDWR))) return (uint64_t)-EINVAL;

    // Validate user buf pointer
    if (!buf) return (uint64_t)-EFAULT;
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    struct pipe *p = proc->fd_table[fd].pipe;
    size_t written = 0;

    while (written < len) {
        // Check if pipe has space: full when head is one behind tail
        if ((p->head + 1) % PIPE_BUF_SIZE == p->tail) {
            // Pipe full: block or return EAGAIN if non-blocking
            if (proc->fd_table[fd].flags & O_NONBLOCK) {
                if (written > 0) break;  // return partial write
                return (uint64_t)-EAGAIN;
            }
            p->write_pid = proc->pid;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_PIPE;
            schedule();
            p->write_pid = -1;
            continue;
        }
        p->buf[p->head] = ((const char __force *)buf)[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        written++;
    }

    // Wake reader if blocked
    if (p->read_pid >= 0) wake_process(p->read_pid);

    // Mirror pipe output to serial port (replaces old SYS_SERIAL_WRITE)
    for (size_t i = 0; i < written; i++)
        serial_putc(((const char __force *)buf)[i]);

    return (uint64_t)written;
}

// sys_read(fd, buf, len) — syscall 16 (从 fd 读数据，阻塞直到有数据)
// FD_PIPE: 直读 kernel ring buffer
// FD_FILE: 通过 kernel_msg_send 代理到 fs_driver
uint64_t sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    char __user *buf = (char __user * __force)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EINVAL;
    if (current_proc->fd_table[fd].type == FD_NONE) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;

    // ===== FD_FILE: proxy to fs_driver =====
    if (proc->fd_table[fd].type == FD_FILE) {
        // Check read permission: reject O_WRONLY only (O_RDONLY=0, so must check != O_WRONLY)
        if ((proc->fd_table[fd].flags & O_WRONLY) && !(proc->fd_table[fd].flags & O_RDWR))
            return (uint64_t)-EINVAL;

        if (proc->fd_table[fd].file_data.offset >= proc->fd_table[fd].file_data.file_size) {
            return 0;  // EOF
        }

        uint64_t avail = proc->fd_table[fd].file_data.file_size - proc->fd_table[fd].file_data.offset;
        if (len > avail) len = avail;
        size_t max_data = 65536 - sizeof(file_t_io_resp);
        if (len > max_data) len = max_data;
        if (len == 0) return 0;

        // Validate user buf pointer
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        // Build READ request
        file_t_io_req req = {0};
        req.cmd = FILE_CMD_READ;
        req.fs_fd = proc->fd_table[fd].file_data.fs_fd;
        req.offset = proc->fd_table[fd].file_data.offset;
        req.count = (uint32_t)len;

        // Allocate reply buffer (header + data)
        size_t resp_size = sizeof(file_t_io_resp) + (size_t)len;
        uint8_t *resp_buf = (uint8_t *)kmalloc(resp_size);
        if (!resp_buf) return (uint64_t)-ENOMEM;

        int64_t ret = sys_msg_to(proc->fd_table[fd].file_data.fs_pid,
                                  &req, sizeof(req), resp_buf, resp_size);
        if (ret < 0) {
            kfree(resp_buf);
            return (uint64_t)(-ret);  // sys_msg_to returns negative errno on failure
        }

        file_t_io_resp *resp = (file_t_io_resp *)resp_buf;
        if (resp->status != 0) {
            kfree(resp_buf);
            return (uint64_t)(-resp->status);
        }

        size_t nread = resp->count;
        if (nread > len) nread = len;
        __memcpy((void __force *)buf, resp_buf + sizeof(file_t_io_resp), nread);

        proc->fd_table[fd].file_data.offset += nread;
        kfree(resp_buf);
        return (uint64_t)nread;
    }

    // ===== FD_SOCKET: delegate to sock_read =====
    if (proc->fd_table[fd].type == FD_SOCKET) {
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct unix_sock *sock = proc->fd_table[fd].sock;
        if (!sock) return (uint64_t)-EBADF;
        int64_t ret = sock_read(sock, (void __force *)buf, len);
        return (uint64_t)ret;
    }

    // ===== FD_PIPE: existing path =====
    // O_RDONLY=0, so check: must not be O_WRONLY only
    if ((proc->fd_table[fd].flags & O_WRONLY) && !(proc->fd_table[fd].flags & O_RDWR))
        return (uint64_t)-EINVAL;

    // Validate user buf pointer
    if (!buf) return (uint64_t)-EFAULT;
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    struct pipe *p = proc->fd_table[fd].pipe;

    // Block if pipe is empty
    while (p->head == p->tail) {
        // Check if write end is closed (all write fds gone)
        if (p->ref_count == 1) return 0;  // EOF: no writers left
        // Non-blocking: return EAGAIN-like indication (0 bytes read)
        if (proc->fd_table[fd].flags & O_NONBLOCK) return 0;
        p->read_pid = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;
        schedule();
        p->read_pid = -1;
    }

    // Read as much as available (up to len)
    size_t nread = 0;
    while (nread < len && p->head != p->tail) {
        ((char __force *)buf)[nread] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        nread++;
    }

    // Wake writer if blocked
    if (p->write_pid >= 0) wake_process(p->write_pid);

    return (uint64_t)nread;

    // FD_DEV and FD_SHM return ENOSYS (never reached via FD_FILE/pipe check above)
}

// sys_close(fd) — syscall 17 (关闭 fd，pipe ref_count--)
uint64_t sys_close(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    int fd = (int)arg1;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EINVAL;
    if (current_proc->fd_table[fd].type == FD_NONE) return (uint64_t)-EINVAL;

    if (current_proc->fd_table[fd].type == FD_PIPE) {
        struct pipe *p = current_proc->fd_table[fd].pipe;
        p->ref_count--;

        // Notify blocked peer
        if (current_proc->fd_table[fd].flags & (O_WRONLY | O_RDWR)) {
            if (p->read_pid >= 0) wake_process(p->read_pid);
        }
        if (current_proc->fd_table[fd].flags & (O_RDONLY | O_RDWR)) {
            if (p->write_pid >= 0) wake_process(p->write_pid);
        }

        if (p->ref_count == 0) {
            kfree(p->buf);
            kfree(p);
        }
    } else if (current_proc->fd_table[fd].type == FD_SHM) {
        // Release SHM reference held by this fd
        if (current_proc->fd_table[fd].shm) {
            shm_put(current_proc->fd_table[fd].shm);
        }
    } else if (current_proc->fd_table[fd].type == FD_FILE) {
        // Release ref_count; notify fs_driver on last close
        current_proc->fd_table[fd].file_data.ref_count--;
        if (current_proc->fd_table[fd].file_data.ref_count == 0) {
            // Notify fs_driver to close the session fd
            file_t_io_req req = {0};
            req.cmd = FILE_CMD_CLOSE;
            req.fs_fd = current_proc->fd_table[fd].file_data.fs_fd;
            kernel_msg_send(current_proc->fd_table[fd].file_data.fs_pid,
                            &req, sizeof(req), NULL, 0);
        }
    } else if (current_proc->fd_table[fd].type == FD_SOCKET) {
        struct unix_sock *sock = current_proc->fd_table[fd].sock;
        if (sock) {
            sock_close(sock);
        }
    }
    // FD_DEV: no dynamic resources to free

    // Clear fd_table entry (use memset for union safety)
    __memset(&current_proc->fd_table[fd], 0, sizeof(struct file));
    current_proc->fd_table[fd].type = FD_NONE;  // re-assert after memset

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

// sys_load_dev(pid, dev_type) — syscall 18 (注册驱动)
// Returns: 0 on success, positive errno on failure
uint64_t sys_load_dev(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    pid_t pid = (pid_t)arg1;
    int dev_type = (int)arg2;
    return (uint64_t)register_dev(dev_type, pid);
}


// Remove a PID from dev_table (called by proc_reap)
void dev_table_cleanup(pid_t pid) {
    for (int i = 0; i < DEV_TYPE_MAX; i++) {
        if (dev_table[i] == pid) dev_table[i] = 0;
    }
}

// Look up device driver PID (kernel-internal, used by xHCI ISR)
pid_t lookup_dev(int dev_type) {
    if (dev_type <= DEV_NONE || dev_type >= DEV_TYPE_MAX) return 0;
    return dev_table[dev_type];
}

// sys_notify(pid) — syscall 20 (异步通知：消息入队 + 唤醒)
// Returns: 0 on success, positive errno on failure
uint64_t sys_notify(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    pid_t target_pid = (pid_t)arg1;
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)-EINVAL;

    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) return (uint64_t)-ESRCH;

    // Enqueue RECV_NOTIFY message
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        return (uint64_t)-EBUSY;  // queue full
    }
    recv_msg_t *slot = (recv_msg_t *)target->recv_buf[target->recv_head];
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

// sys_gettime() — syscall 21 (全局单调时钟，返回纳秒)
uint64_t sys_gettime(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    return sched_clock();
}

// sys_clock() — syscall 22 (per-process CPU 时间，返回纳秒)
uint64_t sys_clock(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    return current_proc->cpu_time_ns;
}

// Inner implementation of sys_msg — shared by PID-based and fd-based variants.
// All parameters are already validated by the caller.
// Returns: 0 on success, positive errno on failure
int64_t sys_msg_to(pid_t target_pid, void *msg_buf, size_t msg_len,
                           void *reply_buf, size_t reply_len) {
    proc_t *target = &procs[target_pid];
    if (target->pid != target_pid) return (uint64_t)-ESRCH;

    // Allocate kernel buffer and copy message from user space
    void *kbuf = kmalloc(msg_len);
    if (!kbuf) return (uint64_t)-ENOMEM;
    __memcpy(kbuf, msg_buf, msg_len);

    // Build RECV_MSG
    uint8_t msg[RECV_MSG_SIZE];
    recv_msg_t *hdr = (recv_msg_t *)msg;
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
        return (uint64_t)-EBUSY;  // queue full
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
    proc->state = BLOCKED;
    proc->wait_event = WAIT_MSG_REPLY;
    proc->wait_timed_out = 0;
    proc->wait_deadline = 0;
    proc->msg_target_pid = target_pid;
    proc->msg_reply_buf = (void __user * __force)reply_buf;
    proc->msg_reply_len = reply_len;
    proc->msg_result = 0;

    schedule();

    // Woken up by sys_msg_resp or proc_reap
    if (proc->msg_result != 0) return (uint64_t)proc->msg_result;
    return 0;
}

// sys_msg(target_pid, msg_buf, msg_len, reply_buf, reply_len) — syscall 22 (变长消息请求)
// PID-based variant.
// 返回: 0=成功, 负errno
uint64_t sys_msg(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t _u1) {
    pid_t target_pid = (pid_t)arg1;
    void __user *msg_buf = (void __user * __force)arg2;
    size_t msg_len = (size_t)arg3;
    void __user *reply_buf = (void __user * __force)arg4;
    size_t reply_len = (size_t)arg5;

    // Validate target PID
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)-ESRCH;

    // Validate msg_len
    if (msg_len == 0 || msg_len > 65536) return (uint64_t)-EINVAL;

    // Validate user pointers
    uint64_t msg_ptr = (__force uint64_t)msg_buf;
    if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
        msg_ptr + msg_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    uint64_t rep_ptr = (__force uint64_t)reply_buf;
    if (!rep_ptr || rep_ptr >= 0xFFFFFFFF80000000ULL ||
        rep_ptr + reply_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    return sys_msg_to(target_pid, (void __force *)msg_buf, msg_len, (void __force *)reply_buf, reply_len);
}

// sys_msg_resp(resp_buf, resp_len) — syscall 23 (回复当前 MSG 调用者)
// 返回: 0=成功, 正数=errno
uint64_t sys_msg_resp(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    void __user *resp_buf = (void __user * __force)arg1;
    size_t resp_len = (size_t)arg2;

    // Validate user pointer
    uint64_t ptr = (__force uint64_t)resp_buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || resp_len == 0 ||
        ptr + resp_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    proc_t *proc = current_proc;
    pid_t caller_pid = proc->msg_caller_pid;
    if (caller_pid < 0 || caller_pid >= MAX_PROC) {
        return (uint64_t)-EINVAL;
    }

    proc_t *caller = &procs[caller_pid];
    if (caller->pid != caller_pid) return (uint64_t)-ESRCH;

    // Copy response data from server user space to kernel buffer
    void *kbuf = kmalloc(resp_len);
    if (!kbuf) return (uint64_t)-ENOMEM;
    __memcpy(kbuf, (const void __force *)resp_buf, resp_len);

    // Copy to caller's reply buffer under caller's CR3
    size_t copy_len = resp_len < caller->msg_reply_len ? resp_len : caller->msg_reply_len;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)caller->cr3) : "memory");
    __memcpy((void __force *)caller->msg_reply_buf, kbuf, copy_len);
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

// sys_ioperm(from, num, turn_on) — syscall 25 (I/O 端口权限)
uint64_t sys_ioperm(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    unsigned long from = (unsigned long)arg1;
    unsigned long num = (unsigned long)arg2;
    int turn_on = (int)arg3;

    if (from + num > 65536) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;

    // Lazy-allocate IOPM if needed
    if (!proc->iopm) {
        uint8_t *iopm = (uint8_t *)kmalloc(IOPM_SIZE);
        if (!iopm) return (uint64_t)-ENOMEM;
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

// sys_dup2(old_fd, new_fd) — syscall 26 (复制 fd)
uint64_t sys_dup2(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int old_fd = (int)arg1;
    int new_fd = (int)arg2;

    if (old_fd < 0 || old_fd >= MAX_FD || new_fd < 0 || new_fd >= MAX_FD)
        return (uint64_t)-EBADF;

    proc_t *proc = current_proc;
    if (proc->fd_table[old_fd].type == FD_NONE)
        return (uint64_t)-EBADF;

    // Close new_fd if it's open
    if (proc->fd_table[new_fd].type != FD_NONE) {
        if (proc->fd_table[new_fd].type == FD_PIPE) {
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
        } else if (proc->fd_table[new_fd].type == FD_SHM) {
            if (proc->fd_table[new_fd].shm) {
                shm_put(proc->fd_table[new_fd].shm);
            }
        } else if (proc->fd_table[new_fd].type == FD_FILE) {
            proc->fd_table[new_fd].file_data.ref_count--;
            if (proc->fd_table[new_fd].file_data.ref_count == 0) {
                file_t_io_req req = {0};
                req.cmd = FILE_CMD_CLOSE;
                req.fs_fd = proc->fd_table[new_fd].file_data.fs_fd;
                kernel_msg_send(proc->fd_table[new_fd].file_data.fs_pid,
                                &req, sizeof(req), NULL, 0);
            }
        } else if (proc->fd_table[new_fd].type == FD_SOCKET) {
            if (proc->fd_table[new_fd].sock) {
                sock_close(proc->fd_table[new_fd].sock);
            }
        }
        // FD_DEV: no dynamic resources to free
        __memset(&proc->fd_table[new_fd], 0, sizeof(struct file));
        proc->fd_table[new_fd].type = FD_NONE;
    }

    // Copy old_fd to new_fd — struct assignment copies entire union
    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    // Re-establish pointer/shm fields from source (struct assignment copies union bytes,
    // but for FD_PIPE/SHM we need to also bump ref_count)
    if (proc->fd_table[new_fd].type == FD_PIPE) {
        proc->fd_table[new_fd].pipe->ref_count++;
    } else if (proc->fd_table[new_fd].type == FD_SHM) {
        if (proc->fd_table[new_fd].shm) {
            shm_get(proc->fd_table[new_fd].shm);
        }
    } else if (proc->fd_table[new_fd].type == FD_FILE) {
        proc->fd_table[new_fd].file_data.ref_count++;
    } else if (proc->fd_table[new_fd].type == FD_SOCKET) {
        if (proc->fd_table[new_fd].sock) {
            unix_sock_acquire(proc->fd_table[new_fd].sock);
        }
    }
    // FD_DEV: target_pid copied by struct assignment, no ref_count needed

    return (uint64_t)new_fd;
}

// sys_fcntl(fd, cmd, arg) — syscall 27 (文件控制)
uint64_t sys_fcntl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    int cmd = (int)arg2;
    int arg = (int)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    proc_t *proc = current_proc;
    if (proc->fd_table[fd].type == FD_NONE) return (uint64_t)-EBADF;

    switch (cmd) {
    case F_GETFL:
        return (uint64_t)proc->fd_table[fd].flags;
    case F_SETFL:
        proc->fd_table[fd].flags = arg;
        return 0;
    case F_ADD_SEALS: {
        // Only valid on FD_SHM with MFD_ALLOW_SEALING
        if (proc->fd_table[fd].type != FD_SHM) return (uint64_t)-EINVAL;
        struct shm *shm = proc->fd_table[fd].shm;
        if (!shm) return (uint64_t)-EBADF;
        if (!(shm->flags & SHM_SEALED)) return (uint64_t)-EPERM;
        if (shm->seals & F_SEAL_SEAL) return (uint64_t)-EPERM;

        unsigned int new_seals = (unsigned int)arg;
        // Only valid seal bits
        if (new_seals & ~(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
            return (uint64_t)-EINVAL;

        // F_SEAL_WRITE check: if any writable mmap exists, refuse
        if (new_seals & F_SEAL_WRITE) {
            // Walk mmap_regions to check for writable SHM mappings
            for (mmap_region_t *mr = proc->mmap_regions; mr; mr = mr->next) {
                if (mr->shm_obj == shm) {
                    // Found a mapping of this shm — if it has write permission
                    // (we can't easily determine prot from mmap_region_t, so
                    // conservatively allow — mmap(PROT_WRITE) will be blocked
                    // on future calls)
                    break;
                }
            }
        }

        shm->seals |= new_seals;
        return 0;
    }
    case F_GET_SEALS: {
        if (proc->fd_table[fd].type != FD_SHM) return (uint64_t)-EINVAL;
        struct shm *shm = proc->fd_table[fd].shm;
        if (!shm) return (uint64_t)-EBADF;
        return (uint64_t)shm->seals;
    }
    default:
        return (uint64_t)-EINVAL;
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

// sys_dma_alloc(size, vaddr_ptr, paddr_ptr) — syscall 28
// 分配物理连续、<4GB 的 DMA 缓冲区，映射到调用者地址空间
// Returns: 0 on success, positive errno on failure
uint64_t sys_dma_alloc(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    size_t size = (size_t)arg1;
    void __user * __user *vaddr_ptr = (void __user * __user * __force)arg2;
    uint64_t __user *paddr_ptr = (uint64_t __user * __force)arg3;

    if (size == 0) return (uint64_t)-EINVAL;

    // Validate user pointers
    uint64_t vp = (__force uint64_t)vaddr_ptr;
    uint64_t pp = (__force uint64_t)paddr_ptr;
    if (!vp || vp >= 0xFFFFFFFF80000000ULL || vp + sizeof(void *) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;
    if (!pp || pp >= 0xFFFFFFFF80000000ULL || pp + sizeof(uint64_t) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    size = ALIGN_UP(size, PAGE_SIZE);
    size_t npages = size / PAGE_SIZE;

    // Allocate contiguous physical pages below 4GB
    Page *pages = bfc_alloc_page_low(npages);
    if (!pages) return (uint64_t)-ENOMEM;

    uint64_t phys = (__force uint64_t)page_to_phys(pages);
    proc_t *proc = current_proc;

    // Pick virtual address from mmap_brk
    uint64_t vaddr = proc->mmap_brk;
    uint64_t vaddr_end = vaddr + size;

    // Map pages into user address space using map_user_page_direct
    for (size_t i = 0; i < npages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_vaddr = vaddr + i * PAGE_SIZE;
        if (!map_user_page_direct((__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3), page_vaddr, page_phys,
                                  PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
            // Cleanup: unmap already-mapped pages
            if (i > 0) unmap_user_pages((__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3), vaddr, vaddr + i * PAGE_SIZE, i);
            bfc_free_page(pages, npages);
            return (uint64_t)-ENOMEM;
        }
    }

    proc->mmap_brk = vaddr_end;

    // Track in mmap_regions for cleanup
    mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
    if (!region) {
        unmap_user_pages((__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3), vaddr, vaddr_end, npages);
        bfc_free_page(pages, npages);
        return (uint64_t)-ENOMEM;
    }
    region->vaddr = vaddr;
    region->size = size;
    region->phys = phys;
    region->next = proc->mmap_regions;
    proc->mmap_regions = region;

    // Write results to user space
    *(__force void __user **)vaddr_ptr = (void __user * __force)vaddr;
    *(__force uint64_t *)paddr_ptr = phys;

    return 0;
}

// sys_dma_free(vaddr) — syscall 29
// 释放 DMA 缓冲区
// Returns: 0 on success, positive errno on failure
uint64_t sys_dma_free(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    uint64_t vaddr = (uint64_t)arg1;
    if (!vaddr) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;

    // Find and remove from mmap_regions
    mmap_region_t **pp = &proc->mmap_regions;
    while (*pp) {
        mmap_region_t *r = *pp;
        if (r->vaddr == vaddr) {
            size_t npages = r->size / PAGE_SIZE;

            // Unmap from user address space
            unmap_user_pages((__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3), r->vaddr, r->vaddr + r->size, npages);

            // Free physical pages
            Page *page = bfc_frames + (r->phys / PAGE_SIZE);
            bfc_free_page(page, npages);

            // Remove from list
            *pp = r->next;
            kfree(r);
            return 0;
        }
        pp = &r->next;
    }

    return (uint64_t)-EINVAL;
}

// sys_lseek(fd, offset, whence) — syscall 44 (文件偏移定位)
// 更新内核 file_data.offset。FD_PIPE/SOCKET/DEV 返回 -ESPIPE。
// Returns: new offset on success, negative errno on failure
uint64_t sys_lseek(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    int64_t offset = (int64_t)arg2;
    int whence = (int)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    proc_t *proc = current_proc;
    if (proc->fd_table[fd].type == FD_NONE) return (uint64_t)-EBADF;

    // PIPE/SOCKET/DEV do not support seek
    if (proc->fd_table[fd].type == FD_PIPE ||
        proc->fd_table[fd].type == FD_SOCKET ||
        proc->fd_table[fd].type == FD_DEV)
        return (uint64_t)-ESPIPE;

    if (proc->fd_table[fd].type != FD_FILE)
        return (uint64_t)-ESPIPE;

    uint64_t new_offset;
    switch (whence) {
    case SEEK_SET:
        new_offset = (uint64_t)offset;
        break;
    case SEEK_CUR:
        new_offset = proc->fd_table[fd].file_data.offset + offset;
        break;
    case SEEK_END:
        new_offset = proc->fd_table[fd].file_data.file_size + offset;
        break;
    default:
        return (uint64_t)-EINVAL;
    }

    proc->fd_table[fd].file_data.offset = new_offset;
    return (uint64_t)new_offset;
}

// ===================== Signal delivery =====================

// Deliver a signal with a user-registered handler.
// Saves the interrupted context in proc->sig and modifies the trapframe
// to redirect execution to the handler via the sig_trampoline.
static void deliver_signal(proc_t *proc, trapframe_t *tf, int sig, void (*handler)(int)) {
    // 1. Save current execution context
    proc->sig.saved_rip = tf->rip;
    proc->sig.saved_rsp = tf->rsp;
    proc->sig.saved_rflags = tf->rflags;
    proc->sig.have_handler = 1;

    // 2. Push trampoline return address onto user stack
    //    (handler's 'ret' will jump to SIG_TRAMPOLINE_ADDR)
    uint64_t user_rsp = tf->rsp;
    user_rsp -= 8;

    // Write to user stack via CR3 switch
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)proc->cr3) : "memory");
    *(uint64_t *)user_rsp = SIG_TRAMPOLINE_ADDR;
    __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

    // 3. Modify trapframe: iret to handler(arg=sig)
    tf->rip = (uint64_t)handler;
    tf->rdi = (uint64_t)(int64_t)sig;  // sign-extend int to 64-bit
    tf->rsp = user_rsp;
}

// ===================== check_pending_signals =====================
// Called before returning to user mode (iret/sysret).
// Scans pending signals and executes default action or delivers to handler.
void check_pending_signals(trapframe_t *tf) {
    // Only deliver when returning to user mode
    if (tf->cs != 0x2B) return;

    proc_t *proc = current_proc;
    if (!proc) return;

    // Loop: handle signals one at a time in priority order (lowest bit first)
    while (1) {
        uint64_t pending = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        if (!pending) return;

        // Find lowest pending signal (highest priority)
        int sig = __builtin_ctzll(pending);
        if (sig <= 0 || sig >= NSIG) {
            // Invalid signal number, clear all and bail
            __atomic_store_n(&proc->sig.pending, 0, __ATOMIC_RELEASE);
            return;
        }

        // Clear pending bit
        __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);

        void (*handler)(int) = proc->sig.action[sig].sa_handler;

        if (handler == SIG_DFL) {
            // Default action
            switch (sig) {
            case SIGCHLD:
                // Ignore — waitpid still works via WAIT_CHILD
                break;
            case SIGSTOP:
            case SIGTSTP:
            case SIGCONT:
                // Not implemented — ignore for now
                break;
            case SIGINT:
            case SIGTERM:
            case SIGKILL:
            default:
                // Terminate — processes that die from a signal exit with -1
                proc->exit_code = -1;
                serial_printf("signal: pid=%d terminated by signal %d\n", proc->pid, sig);
                sys_exit(-1, 0, 0, 0, 0, 0);
                // Does not return
            }
        } else if (handler == SIG_IGN) {
            // Ignore — do nothing
        } else {
            // User-registered handler
            deliver_signal(proc, tf, sig, handler);
            return;  // tf modified, return to user mode to execute handler
        }
        // Continue checking more signals
    }
}

// ===================== Signal syscalls =====================

// sys_kill(pid, sig) — syscall 47 (发送信号)
uint64_t sys_kill(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    pid_t pid = (pid_t)arg1;
    int sig = (int)arg2;

    // Validate signal number
    if (sig < 0 || sig >= NSIG) return (uint64_t)-EINVAL;
    // signal 0 is a null check (existence test) — always succeed
    if (sig == 0) return 0;

    // Validate target PID
    if (pid < 0 || pid >= MAX_PROC) return (uint64_t)-ESRCH;

    proc_t *target = &procs[pid];
    if (target->pid != pid) return (uint64_t)-ESRCH;

    // Set pending bit (atomic, no lock needed for a single bit)
    __atomic_or_fetch(&target->sig.pending, 1ULL << sig, __ATOMIC_RELEASE);

    // If target is blocked in WAIT_RECV, wake it so it can check signals
    // (But without EINTR support, waking here would just cause sys_recv to
    //  loop back — the signal will be handled on next iret.)
    // For SIGCHLD, the WAIT_CHILD wake is already done by sys_exit.

    return 0;
}

// sys_sigaction(sig, act, oldact) — syscall 48 (注册/查询信号 handler)
uint64_t sys_sigaction(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int sig = (int)arg1;
    const struct sigaction __user *act = (const struct sigaction __user * __force)arg2;
    struct sigaction __user *oldact = (struct sigaction __user * __force)arg3;

    // Validate signal number
    if (sig < 0 || sig >= NSIG) return (uint64_t)-EINVAL;
    // SIGKILL and SIGSTOP cannot be caught or ignored
    if (sig == SIGKILL || sig == SIGSTOP) return (uint64_t)-EINVAL;

    proc_t *proc = current_proc;

    // If oldact is non-NULL, copy current action to user space
    if (oldact) {
        uint64_t ptr = (__force uint64_t)oldact;
        if (ptr >= 0xFFFFFFFF80000000ULL || ptr + sizeof(struct sigaction) > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        // Write under user's CR3
        uint64_t saved_cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
        __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)proc->cr3) : "memory");
        __memcpy((void __force *)oldact, &proc->sig.action[sig], sizeof(struct sigaction));
        __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");
    }

    // If act is non-NULL, copy from user space
    if (act) {
        uint64_t ptr = (__force uint64_t)act;
        if (ptr >= 0xFFFFFFFF80000000ULL || ptr + sizeof(struct sigaction) > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        // Read from user space under user's CR3
        struct sigaction new_act;
        uint64_t saved_cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
        __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)proc->cr3) : "memory");
        __memcpy(&new_act, (const void __force *)act, sizeof(struct sigaction));
        __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

        proc->sig.action[sig] = new_act;

        // POSIX: registering a handler discards pending signals
        __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);
    }

    return 0;
}

// sys_sigreturn() — syscall 49 (信号 handler 返回后恢复上下文)
uint64_t sys_sigreturn(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    proc_t *proc = current_proc;

    if (!proc->sig.have_handler) {
        serial_printf("sys_sigreturn: no saved context (pid=%d)\n", proc->pid);
        return (uint64_t)-EINVAL;
    }

    // Restore saved context into trapframe
    // We're called from syscall context: the trapframe is on the kernel stack.
    // Modify GP registers saved in the current syscall's trapframe so that
    // when syscall_fast_entry does SYSRET, it returns to the saved context.
    //
    // However, we need direct access to the trapframe. In our syscall calling
    // convention, syscall is dispatched via syscall_dispatch(tf) where tf is
    // the trapframe on the kernel stack. But syscall_dispatch is called from
    // assembly, and we don't have the trapframe pointer here.
    //
    // Solution: modify current_proc's sig state and the per-CPU saved trapframe.
    // The assembly code in syscall_fast_entry will be modified after calling
    // syscall_dispatch to check sig_return_pending and reload from saved_* fields.
    //
    // Cleaner approach: just overwrite the syscall-specific state.
    // The trapframe is on the current kernel stack. We can find it by looking
    // at the stack layout. But this is fragile.
    //
    // Instead, we'll modify the response in the syscall entry point assembly.
    // For now, we save the restore state and let the assembly handle it.
    //
    // Actually, the simplest approach: the trampoline sets up a fake trap return.
    // Let's just modify the current_proc state and leave the trapframe alone.
    // The assembly code in __trapret will check for pending signal sigreturn
    // and reload saved_rip/rsp/rflags if have_handler is set.

    // Clear handler flag
    proc->sig.have_handler = 0;

    // Set up the saved state in a way that the trampoline can find it.
    // We need to restore rip/rsp/rflags to the caller.
    // The user-space trampoline called us via syscall — the current trapframe
    // has rip/rsp from the trampoline call. We overwrite them with saved values.
    // But the trapframe is on the kernel stack and we don't have its pointer!

    // Simplest working approach: the trampoline issue is that after sys_sigreturn,
    // the SYSRET goes back to the trampoline's caller. We need to redirect.
    //
    // Fix: Use a different approach. In check_pending_signals -> deliver_signal,
    // we push the trampoline address on the user stack. But instead of having
    // the trampoline call sys_sigreturn, we can make the trampoline use a
    // different mechanism.
    //
    // ACTUAL SOLUTION: Instead of a trampoline, we modify the trapframe directly
    // in deliver_signal so that the handler frame is set up as a nested function
    // call frame. The handler returns normally (to the trampoline), the trampoline
    // calls sys_sigreturn, and sys_sigreturn modifies the CURRENT process's
    // trapframe (which is the trampoline's syscall frame) to restore the original
    // rip/rsp/rflags.

    // We DO have access to the trapframe! The syscall trapframe is on the kernel
    // stack. Since we're in syscall context, RSP points into the kernel stack
    // after the C ABI frame setup. The trapframe is below current RSP.
    //
    // Actually, the syscall_fast_entry builds a trapframe at the bottom of the
    // kernel stack, then calls syscall_dispatch. Inside syscall_dispatch, the
    // stack has: [trapframe] [return addr] [saved rbx/rbp/...] [locals].
    // The trapframe is at a known offset from the kernel stack top.
    //
    // Even simpler: find the trapframe via the per-CPU saved kernel stack base.
    // The trapframe base is at: tss_rsp0 - 176 (sizeof trapframe).
    uint64_t tf_base = get_cpu_local()->tss_rsp0 - 176;
    trapframe_t *tf = (trapframe_t *)tf_base;

    tf->rip = proc->sig.saved_rip;
    tf->rsp = proc->sig.saved_rsp;
    tf->rflags = proc->sig.saved_rflags;

    return 0;
}
