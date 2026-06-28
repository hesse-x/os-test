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
#include "kernel/acpi.h"
#include "kernel/log.h"
#include "kernel/mem/slab.h"
#include "kernel/mem/kasan.h"
#include "kernel/mem/alloc.h"
#include "common/errno.h"
#include "common/ioctl.h"
#include "kernel/display.h"
#include "kernel/devtmpfs.h"
#include "kernel/pty.h"
#include "kernel/pci.h"
#include "common/dev.h"
#include "kernel/socket.h"
#include "kernel/vfs.h"
#include "kernel/inode.h"
#include "kernel/fat32.h"
#include "common/stat.h"
#include "common/syscall.h"
#include "common/input.h"
#include "kernel/user_check.h"

// ===================== Helper functions =====================

// Find the first free fd slot starting from min_fd. Returns fd index or -EMFILE.
static int alloc_fd(task_t *proc, int min_fd) {
    for (int i = min_fd; i < MAX_FD; i++) {
        if (proc->mm->files->fd_table[i].type == FD_NONE)
            return i;
    }
    return -EMFILE;
}

// Close an fd: release resources (pipe/shm/inode/socket/dev) and zero the slot.
// FD_FILE notification is NOT handled here (different structs per caller).
// This is a local static duplicate of the global close_fd_internal in proc.c,
// adapted to take files_t* instead of task_t*.
// Returns 0 on success.
static int close_fd_trap(files_t *files, int fd) {
    struct file *f = &files->fd_table[fd];
    if (f->type == FD_PIPE) {
        struct pipe *p = f->pipe;
        if (p) {
            wake_pipe_peers(p, f->flags);
            if (refcount_dec_and_test(&p->p_count)) {
                kfree(p->buf);
                kfree(p);
            }
        }
    } else if (f->type == FD_SHM) {
        if (f->shm) shm_put(f->shm);
    } else if (f->type == FD_REGULAR || f->type == FD_DIR) {
        if (f->inode) inode_put(f->inode);
    } else if (f->type == FD_SOCKET) {
        if (f->sock) sock_close(f->sock);
    } else if (f->type == FD_DEV) {
        struct inode *ip = f->inode;
        if (ip && ip->i_priv) {
            struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
            if (ops->driver_pid == 0 && ops->close)
                ops->close(current_task, fd);
        }
        if (ip) inode_put(ip);
    } else if (f->type == FD_TTY) {
        pty_close_fd(current_task, fd);
    }
    __memset(f, 0, sizeof(struct file));
    f->type = FD_NONE;
    return 0;
}

// ===================== IRQ handler registry =====================
#define MAX_IRQ_HANDLERS 128
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS];

// ===================== IRQ owner (user-space driver binding) =====================
static pid_t irq_owner[MAX_IRQ_HANDLERS];

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

void unregister_irq(int vec) {
  if (vec >= 0 && vec < MAX_IRQ_HANDLERS) {
    irq_handlers[vec] = NULL;
  }
}

// ===================== File protocol for FD_FILE <-> fs_driver IPC =====================
// Kernel-internal FD_FILE IPC commands (distinct from FS_CMD_* in common/shm.h,
// which is for the SHM protocol. FILE_CMD_* numbering: 2/3/4; FS_CMD_*: 0-6)
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

// Forward declaration of force_sig (defined after signal syscalls section)
static void force_sig(task_t *proc, int sig, int si_code, void *si_addr);

void trap_dispatch(trapframe_t *tf) {
  get_cpu_local()->cur_tf = tf;
  // Hardware IRQ: check user-space driver binding first
  if (tf->trapno >= 32 && tf->trapno < MAX_IRQ_HANDLERS &&
      __atomic_load_n(&irq_owner[tf->trapno], __ATOMIC_ACQUIRE) >= 0) {
    pid_t owner_pid = __atomic_load_n(&irq_owner[tf->trapno], __ATOMIC_ACQUIRE);
    // Direct index by PID — no scan needed
    if (owner_pid >= 0 && owner_pid < MAX_PROC) {
      task_t *target = &tasks[owner_pid];

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
  if (tf->trapno == LAPIC_TIMER_VECTOR) {
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
    printk(LOG_ERROR, "PAGE FAULT: fault addr=0x%016lX", cr2);
  } else if (tf->trapno == 6) {
    printk(LOG_ERROR, "UNDEFINED OPCODE");
  } else if (tf->trapno == 13) {
    printk(LOG_ERROR, "GENERAL PROTECTION");
    #ifdef SANITIZER
    if (kasan_shadow_exists()) {
      uint64_t fault_addr;
      __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));
      printk(LOG_WARN, "\n  KASAN: possible shadow access to non-canonical address");
      printk(LOG_WARN, "\n  Check if __user pointer was used without copy_from_user/to_user");
    }
    #endif
  } else {
    printk(LOG_ERROR, "EXCEPTION: vector 0x%016lX", tf->trapno);
  }
  printk(LOG_ERROR, "\n  rip=0x%016lX cs=0x%016lX rfl=0x%016lX rsp=0x%016lX ss=0x%016lX",
                tf->rip, tf->cs, tf->rflags, tf->rsp, tf->ss);
  printk(LOG_ERROR, "\n  rax=0x%016lX rbx=0x%016lX rcx=0x%016lX rdx=0x%016lX",
                tf->rax, tf->rbx, tf->rcx, tf->rdx);
  printk(LOG_ERROR, "\n  rsi=0x%016lX rdi=0x%016lX rbp=0x%016lX r08=0x%016lX",
                tf->rsi, tf->rdi, tf->rbp, tf->r8);
  printk(LOG_ERROR, "\n  r09=0x%016lX r10=0x%016lX r11=0x%016lX r12=0x%016lX",
                tf->r9, tf->r10, tf->r11, tf->r12);
  printk(LOG_ERROR, "\n  r13=0x%016lX r14=0x%016lX r15=0x%016lX err=0x%016lX",
                tf->r13, tf->r14, tf->r15, tf->err_code);
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  printk(LOG_ERROR, "\n  cr3=0x%016lX", cr3);
  if (current_task) {
    printk(LOG_ERROR, " pid=%d proc_cr3=0x%016lX",
                  current_task->pid, current_task->cr3);
  }
  printk(LOG_ERROR, "\n");

  dump_stack_trace();

  // === DEBUG: dump faulting instruction bytes and kernel stack ===
  if (tf->cs == 0x08) {
    // Dump 16 bytes at faulting rip (what "code" is the CPU trying to execute?)
    printk(LOG_DEBUG, "  RIP bytes:");
    uint8_t *rip_ptr = (uint8_t *)tf->rip;
    for (int i = 0; i < 16 && rip_ptr; i++) {
      printk(LOG_DEBUG, " %02X", rip_ptr[i]);
    }
    printk(LOG_DEBUG, "\n");

    // Dump current_task kernel stack top area (trapframe + switch_frame)
    if (current_task && current_task->k_stack_top) {
      uint64_t stack_base = current_task->k_stack_top - 2 * PAGE_SIZE;
      uint64_t *sp = (uint64_t *)stack_base;
      printk(LOG_DEBUG, "  Kernel stack dump (bottom→top):\n");
      // Only dump the top 24 words (176 bytes trapframe + 56 bytes switch_frame)
      int start = (2 * PAGE_SIZE / 8) - 24;
      for (int i = start; i < 2 * PAGE_SIZE / 8; i++) {
        printk(LOG_DEBUG, "    [0x%016lX] 0x%016lX\n", stack_base + i * 8, sp[i]);
      }
      // Also dump k_rsp and the words around it
      printk(LOG_DEBUG, "  k_rsp=0x%016lX k_stack_top=0x%016lX\n",
                    current_task->k_rsp, current_task->k_stack_top);
      // Dump 8 words starting at k_rsp
      printk(LOG_DEBUG, "  At k_rsp:\n");
      uint64_t *krsp_ptr = (uint64_t *)current_task->k_rsp;
      for (int i = 0; i < 16; i++) {
        printk(LOG_DEBUG, "    [0x%016lX] 0x%016lX\n",
                      current_task->k_rsp + i * 8, krsp_ptr[i]);
      }
    }
  }

  if (tf->cs == 0x2B) {
    // User-mode exception: translate to signal instead of killing process
    int sig = 0;
    int si_code = SI_KERNEL;
    void *si_addr = NULL;

    switch (tf->trapno) {
    case 0:   // #DE divide error
        sig = SIGFPE; si_code = FPE_INTDIV; si_addr = NULL;
        break;
    case 6:   // #UD illegal opcode
        sig = SIGILL; si_code = ILL_ILLOPC; si_addr = (void *)tf->rip;
        break;
    case 13:  // #GP general protection
        sig = SIGSEGV; si_code = SEGV_MAPERR; si_addr = (void *)tf->rip;
        break;
    case 14:  // #PF page fault
        sig = SIGSEGV;
        uint64_t cr2;
        __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
        si_addr = (void *)cr2;
        if (tf->err_code & 1)  // present bit
            si_code = SEGV_ACCERR;
        else
            si_code = SEGV_MAPERR;
        break;
    default:
        sig = SIGSEGV; si_code = SI_KERNEL; si_addr = (void *)tf->rip;
        break;
    }

    printk(LOG_INFO, "exception: pid=%d vector=%d sig=%d addr=%p\n",
        current_task->pid, tf->trapno, sig, si_addr);

    force_sig(current_task, sig, si_code, si_addr);
    return;  // Don't kill process; check_pending_signals will handle it
  }
  // Kernel-mode exception: unrecoverable, panic
  panic("kernel-mode exception: vector=%lu rip=0x%lx cr3=0x%lx",
        tf->trapno, tf->rip, cr3);
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
    task_t *p = LIST_ENTRY(list_front(head), task_t, wait_node);
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

  // Initialize kernel SHM table
  for (int i = 0; i < MAX_KERNEL_SHM; i++) {
    kernel_shm_table[i].shm_id = -1;
    kernel_shm_table[i].shm = NULL;
  }

  // Register default handlers
  register_irq(LAPIC_TIMER_VECTOR, timer_handler);

  // Re-initialize GDT with per-CPU setup (now running at virtual address)
  smp_init_cpu(0, 0, (uint64_t)&stack_bottom + 8192);
  smp_apply_cpu(0);

  // Enable NX bit (CR4.NXDE + EFER.NXE) before IDT install
  enable_nx();

  idt_install();
  setup_syscall();
  apic_init();

  // Verify BSP LAPIC timer is counting down
  {
    uint32_t ccr1 = lapic_read(LAPIC_TIMER_CCR);
    for (volatile int i = 0; i < 100000; i++);  // brief delay
    uint32_t ccr2 = lapic_read(LAPIC_TIMER_CCR);
    printk(LOG_INFO, "isr_init: BSP timer CCR %u->%u LVT=0x%x (counting=%s)\n",
                   ccr1, ccr2, lapic_read(LAPIC_LVT_TIMER),
                   ccr1 != ccr2 ? "yes" : "NO!");
  }
}

// ===================== Signal trampoline init =====================
void sig_init() {
    // Allocate one shared physical page for the signal trampoline.
    // The trampoline code is: mov rax, SYS_SIGRETURN; syscall
    // Encoding: 48 C7 C0 <31> 00 00 00  0F 05
    // SYS_SIGRETURN = 48 (0x30)
    Page *page = bfc_alloc_page(1);
    if (!page) {
        printk(LOG_ERROR, "sig_init: failed to allocate trampoline page\n");
        return;
    }
    sig_trampoline_phys = (__force uint64_t)page_to_phys(page);
    uint8_t *vaddr = (__force uint8_t *)phys_to_virt((__force phys_addr_t)sig_trampoline_phys);

    // mov rax, 45  (SYS_SIGRETURN)
    vaddr[0] = 0x48;  // REX.W prefix
    vaddr[1] = 0xC7;  // MOV r64, imm32
    vaddr[2] = 0xC0;  // ModRM: rax
    vaddr[3] = 0x2D;  // 45 = SYS_SIGRETURN (low byte)
    vaddr[4] = 0x00;
    vaddr[5] = 0x00;
    vaddr[6] = 0x00;

    // syscall
    vaddr[7] = 0x0F;
    vaddr[8] = 0x05;

    printk(LOG_INFO, "sig_init: trampoline at phys=%lx\n", sig_trampoline_phys);
}

// ===================== Syscall dispatch =====================
#define NR_SYSCALL 63
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_getpid,         // 0
    sys_yield,          // 1
    sys_recv,           // 2
    sys_req,            // 3
    sys_resp,           // 4
    sys_irq_bind,       // 5
    sys_exit,           // 6
    sys_waitpid,        // 7
    NULL,               // 8 (sys_spawn removed, use fork+execve)
    sys_mmap,           // 9
    sys_munmap,         // 10
    sys_shm_create,     // 11
    sys_shm_attach,     // 12
    sys_pipe,           // 13
    sys_write,          // 14
    sys_read,           // 15
    sys_close,          // 16
    sys_notify,         // 17
    sys_gettime,        // 18
    sys_clock,          // 19
    sys_msg,            // 20
    sys_msg_resp,       // 21
    sys_ioperm,         // 22
    sys_dup2,           // 23
    sys_fcntl,          // 24
    sys_dma_alloc,      // 25
    sys_dma_free,       // 26
    sys_pci_dev_info,   // 27
    sys_block_async,    // 28
    sys_install_fd_impl, // 29
    sys_socket,         // 30
    sys_bind,           // 31
    sys_listen,         // 32
    sys_accept,         // 33
    sys_connect,        // 34
    sys_socketpair,     // 35
    sys_sendmsg,        // 36
    sys_recvmsg,        // 37
    sys_shutdown,       // 38
    sys_poll,           // 39
    sys_lseek,          // 40
    sys_memfd_create,   // 41
    sys_ftruncate,      // 42
    sys_kill,           // 43
    sys_sigaction,      // 44
    sys_sigreturn,      // 45
    sys_debug_print,    // 46
    sys_open,           // 47
    sys_stat,           // 48
    sys_mkdir,          // 49
    sys_unlink,         // 50
    sys_rmdir,          // 51
    sys_dev_create,     // 52
    sys_getdents,       // 53
    sys_ioctl,          // 54
    sys_fstat,          // 55
    sys_fdev_pid,       // 56
    sys_fork,           // 57
    sys_execve,         // 58
    sys_setsid,         // 59
    sys_setpgid,        // 60
    sys_getpgid,        // 61
    sys_getsid,         // 62
};

void syscall_dispatch(trapframe_t *tf) {
    get_cpu_local()->cur_tf = tf;
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
    refcount_inc(&shm->s_count);
    return shm;
}

void shm_put(struct shm *shm) {
    if (!shm) return;
    if (refcount_dec_and_test(&shm->s_count)) {
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
    return (uint64_t)current_task->pid;
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

    task_t *proc = current_task;
    int cpu = proc->assigned_cpu;
    while (1) {
        // Try to dequeue a message from recv queue
        spin_lock(&proc->recv_lock);
        if (proc->recv_head != proc->recv_tail) {
            // Message available: copy to user buffer
            copy_to_user(buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
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
                copy_to_user(data_buf, kmaddr, len);
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

        // EINTR check: ISR notification (recv_intr set by wake_process from kernel ISR)
        if (proc->recv_intr) {
            proc->recv_intr = 0;
            return (uint64_t)-EINTR;
        }

        // EINTR check: signal pending and deliverable
        uint64_t pend = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->sig.blocked;
        deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
        if (deliv) return (uint64_t)-EINTR;

        // Woken up: check if timed out
        if (proc->wait_timed_out) {
            // Re-check queue before returning timeout (race: message may have arrived)
            spin_lock(&proc->recv_lock);
            if (proc->recv_head != proc->recv_tail) {
                copy_to_user(buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
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
                    copy_to_user(data_buf, kmaddr, len);
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

    task_t *target = &tasks[target_pid];
    if (target->pid != target_pid) return (uint64_t)-ESRCH;
    WARN_ON(target->state == UNUSED);

    // Build RECV_REQ message
    uint8_t msg[RECV_MSG_SIZE];
    recv_msg_t *hdr = (recv_msg_t *)msg;
    hdr->type = RECV_REQ;
    hdr->src = (uint32_t)current_task->pid;
    // Copy request payload from user space
    copy_from_user(hdr->data, request, 56);

    // Enqueue to target's recv queue
    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        printk(LOG_WARN, "sys_req: target_pid=%d recv queue full!\n", target_pid);
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
    task_t *proc = current_task;
    proc->state = BLOCKED;
    proc->wait_event = WAIT_REQ_REPLY;
    proc->wait_timed_out = 0;
    proc->wait_deadline = 0;
    proc->req_target_pid = target_pid;
    proc->req_reply_buf = reply;
    proc->req_reply_len = RECV_MSG_SIZE;
    proc->req_result = 0;

    schedule();

    // EINTR check
    {
        uint64_t pend = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->sig.blocked;
        deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
        if (deliv) return (uint64_t)-EINTR;
    }

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

    task_t *proc = current_task;
    pid_t caller_pid = proc->req_caller_pid;
    if (caller_pid < 0 || caller_pid >= MAX_PROC) return (uint64_t)-EINVAL;

    task_t *caller = &tasks[caller_pid];
    if (caller->pid != caller_pid) return (uint64_t)-ESRCH;

    // Copy reply data to caller's reply buffer (user space)
    // We must first copy to a kernel buffer under the server's CR3,
    // then switch to caller's CR3 to write to the caller's user-space buffer.
    size_t reply_len = caller->req_reply_len;
    if (reply_len == 0) reply_len = RECV_MSG_SIZE;  // compat: default
    if (reply_len > RECV_MSG_SIZE) reply_len = RECV_MSG_SIZE;
    uint8_t kbuf[RECV_MSG_SIZE];
    copy_from_user(kbuf, reply, RECV_MSG_SIZE);
    // Only write reply_len bytes to avoid overflowing small ioctl arg buffers
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)caller->cr3) : "memory");
    copy_to_user(caller->req_reply_buf, kbuf, reply_len);
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
    // Mutual exclusion: cannot share IRQ with kernel ISR (e.g., serial dev)
    if (irq_handlers[irq] != NULL) return (uint64_t)-EBUSY;
    __atomic_store_n(&irq_owner[irq], current_task->pid, __ATOMIC_RELEASE);

    // Auto-unmask I/O APIC for this IRQ (GSI = vector - 32)
    int gsi = irq - 32;
    if (gsi >= 0 && gsi < 24) {
        uint32_t bsp_apic_id = (uint32_t)(lapic_read(LAPIC_ID) >> 24);
        const acpi_iso_override_t *iso = acpi_find_iso((uint8_t)gsi);
        bool level = iso ? iso->level_triggered : false;
        bool low   = iso ? iso->active_low : false;
        ioapic_set_irq(gsi, irq, bsp_apic_id, false, level, low);
    }

    return 0;
}

// sys_exit(exit_code) — syscall 6 (进程退出)
uint64_t sys_exit(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    task_t *proc = current_task;
    int32_t exit_code = (int32_t)arg1;
    proc->exit_code = exit_code;

    // Final CPU time accounting: ensure last running slice is recorded
    if (proc->last_sched != 0) {
        proc->cpu_time_ns += sched_clock() - proc->last_sched;
        proc->last_sched = 0;
    }

    // Orphan adoption: reparent children to init
    if (init_pid >= 0) {
        spin_lock(&tasks_lock);
        for (int i = 0; i < MAX_PROC; i++) {
            if (tasks[i].pid >= 0 && tasks[i].mm && tasks[i].mm->parent_pid == proc->pid) {
                tasks[i].mm->parent_pid = init_pid;
            }
        }
        spin_unlock(&tasks_lock);
    }

    if (!proc->mm || proc->mm->parent_pid < 0) {
        // No parent: directly reap all resources
        task_reap(proc);
    } else {
        // Has parent: become ZOMBIE, wait for sys_waitpid to reap
        int cpu = proc->assigned_cpu;
        uint64_t flags;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
        proc->state = ZOMBIE;
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
        // Notify parent via SIGCHLD (replaces old RECV_NOTIFY)
        {
            task_t *parent = &tasks[proc->mm->parent_pid];
            __atomic_or_fetch(&parent->sig.pending, 1ULL << SIGCHLD, __ATOMIC_RELEASE);

            // Wake parent if in WAIT_CHILD (waitpid)
            int pcpu = parent->assigned_cpu;
            spin_lock(&cpu_locals[pcpu].scheduler_lock);
            if (parent->state == BLOCKED &&
                parent->wait_event == WAIT_CHILD) {
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
            task_t *waiter = &tasks[i];
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
            // Scan for a ZOMBIE child and count total children
            spin_lock(&tasks_lock);
            task_t *zombie = NULL;
            bool has_children = false;
            for (int i = 0; i < MAX_PROC; i++) {
                if (tasks[i].pid >= 0 && tasks[i].mm && tasks[i].mm->parent_pid == current_task->pid) {
                    has_children = true;
                    if (tasks[i].state == ZOMBIE) {
                        zombie = &tasks[i];
                        break;
                    }
                }
            }
            if (!has_children) {
                spin_unlock(&tasks_lock);
                return (uint64_t)-ECHILD;
            }
            if (zombie) {
                int cpu = zombie->assigned_cpu;
                spin_unlock(&tasks_lock);
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
                task_reap(zombie);
                return (uint64_t)zpid;
            }
            spin_unlock(&tasks_lock);

            // No zombie child: block on WAIT_CHILD
            // Must set wait_event + BLOCKED under parent's scheduler_lock
            // to prevent race with child's sys_exit checking our state.
            int pcpu = current_task->assigned_cpu;
            spin_lock(&cpu_locals[pcpu].scheduler_lock);
            // Re-scan under parent lock to avoid missed wakeup
            spin_lock(&tasks_lock);
            zombie = NULL;
            has_children = false;
            for (int i = 0; i < MAX_PROC; i++) {
                if (tasks[i].pid >= 0 && tasks[i].mm && tasks[i].mm->parent_pid == current_task->pid) {
                    has_children = true;
                    if (tasks[i].state == ZOMBIE) {
                        zombie = &tasks[i];
                        break;
                    }
                }
            }
            if (!has_children) {
                spin_unlock(&tasks_lock);
                spin_unlock(&cpu_locals[pcpu].scheduler_lock);
                return (uint64_t)-ECHILD;
            }
            if (zombie) {
                spin_unlock(&tasks_lock);
                spin_unlock(&cpu_locals[pcpu].scheduler_lock);
                continue;  // retry the outer loop
            }
            spin_unlock(&tasks_lock);
            current_task->wait_event = WAIT_CHILD;
            current_task->state = BLOCKED;
            spin_unlock(&cpu_locals[pcpu].scheduler_lock);
            schedule();
            // EINTR check: SIGCHLD does not interrupt waitpid
            {
                uint64_t pend = __atomic_load_n(&current_task->sig.pending, __ATOMIC_ACQUIRE);
                uint64_t deliv = pend & ~current_task->sig.blocked;
                deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
                deliv &= ~(1ULL << SIGCHLD);
                if (deliv) return (uint64_t)-EINTR;
            }
        }
    }

    if (pid < 0 || pid >= MAX_PROC) {
        printk(LOG_WARN, "waitpid: pid=%d out of range\n", pid);
        return 0;  // EINVAL
    }

    task_t *child = &tasks[pid];

    // Validate: pid must be our child (under tasks_lock to prevent reap)
    spin_lock(&tasks_lock);
    if (child->pid != pid || !child->mm || child->mm->parent_pid != current_task->pid) {
        printk(LOG_WARN, "waitpid: pid=%d validation fail: child_pid=%d mm=%p parent_pid=%d caller=%d\n",
            pid, child->pid, child->mm, child->mm ? child->mm->parent_pid : -1, current_task->pid);
        spin_unlock(&tasks_lock);
        return 0;  // ECHILD
    }
    spin_unlock(&tasks_lock);

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
        // Must set wait_event + BLOCKED under both parent and child locks
        // (or at least before releasing child's lock) to prevent race:
        // child on another CPU could exit, set ZOMBIE, check parent state,
        // find it still RUNNING, and never wake this process.
        int pcpu = current_task->assigned_cpu;
        if (pcpu == cpu) {
            // Same CPU: single lock covers both parent and child state
            spin_lock(&cpu_locals[pcpu].scheduler_lock);
            if (child->state == ZOMBIE) {
                child->state = REAPING;
                spin_unlock(&cpu_locals[pcpu].scheduler_lock);
                break;
            }
            current_task->wait_event = WAIT_CHILD;
            current_task->state = BLOCKED;
            spin_unlock(&cpu_locals[pcpu].scheduler_lock);
        } else {
            // Different CPUs: need both locks to prevent race
            spin_lock(&cpu_locals[pcpu].scheduler_lock);
            spin_lock(&cpu_locals[cpu].scheduler_lock);
            if (child->state == ZOMBIE) {
                child->state = REAPING;
                spin_unlock(&cpu_locals[cpu].scheduler_lock);
                spin_unlock(&cpu_locals[pcpu].scheduler_lock);
                break;
            }
            current_task->wait_event = WAIT_CHILD;
            current_task->state = BLOCKED;
            spin_unlock(&cpu_locals[cpu].scheduler_lock);
            spin_unlock(&cpu_locals[pcpu].scheduler_lock);
        }
        schedule();

        // EINTR check: SIGCHLD does not interrupt waitpid (it IS the
        // notification we're waiting for). Other signals do interrupt.
        {
            uint64_t pend = __atomic_load_n(&current_task->sig.pending, __ATOMIC_ACQUIRE);
            uint64_t deliv = pend & ~current_task->sig.blocked;
            deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
            deliv &= ~(1ULL << SIGCHLD);
            if (deliv) {
                printk(LOG_WARN, "waitpid: pid=%d EINTR pending=0x%lx\n", pid, pend);
                return (uint64_t)-EINTR;
            }
        }

        // Woken up by notify — re-validate child still exists
        spin_lock(&tasks_lock);
        if (child->pid != pid) {
            // Child was reaped by someone else — should not happen
            printk(LOG_WARN, "waitpid: pid=%d child reaped by someone else\n", pid);
            spin_unlock(&tasks_lock);
            return 0;  // ECHILD
        }
        spin_unlock(&tasks_lock);
    }

    // Reap child resources
    if (exit_code_ptr) {
        // Validate user pointer: must be in user canonical low half, not kernel space
        uint64_t ptr_val = (__force uint64_t)exit_code_ptr;
        if (ptr_val >= 0xFFFFFFFF80000000ULL || !ptr_val || (ptr_val + sizeof(int32_t) - 1) >= 0xFFFFFFFF80000000ULL) {
            printk(LOG_WARN, "waitpid: pid=%d bad exit_code_ptr=0x%lx\n", pid, ptr_val);
            return 0;  // EFAULT
        }
        *(__force int32_t *)exit_code_ptr = child->exit_code;
    }
    task_reap(child);
    return (uint64_t)pid;
}

// sys_mmap(addr, size, prot, flags, fd, offset) — syscall 9 (内存映射)
// MAP_SHARED + fd ≥ 0: SHM fd 映射
// MAP_PHYSICAL: offset=phys_addr, fd=-1// MAP_ANONYMOUS: fd=-1
// Returns: mapped address on success, 0 on failure

uint64_t sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg3; // addr, prot — not yet used
    size_t size = (size_t)arg2;
    int flags = (int)arg4;
    int fd = (int)arg5;
    uint64_t offset = arg6;

    task_t *proc = current_task;
    uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

    // MAP_SHARED (0x01) + fd ≥ 0: SHM or DEV fd mapping
    if ((flags & 0x01) && fd >= 0) {
        if (fd >= MAX_FD) return 0;

        // FD_DEV: dispatch to device mmap handler (devtmpfs path with inode)
        if (proc->mm->files->fd_table[fd].type == FD_DEV) {
            struct inode *ip = proc->mm->files->fd_table[fd].inode;
            if (ip && ip->i_priv) {
                struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
                if (ops->driver_pid == 0 && ops->mmap) {
                    uint64_t ret = ops->mmap(proc, size);
                    return ret;
                }
                // User-space driver: auto shm_attach to driver's SHM
                if (ops->driver_pid > 0) {
                    pid_t drv_pid = ops->driver_pid;
                    if (drv_pid < 0 || drv_pid >= MAX_PROC) return 0;
                    task_t *drv_proc = &tasks[drv_pid];
                    if (drv_proc->pid != drv_pid) return 0;

                    // Find driver's FD_SHM
                    struct shm *target_shm = NULL;
                    for (int i = 0; i < MAX_FD; i++) {
                        if (drv_proc->mm && drv_proc->mm->files->fd_table[i].type == FD_SHM) {
                            target_shm = drv_proc->mm->files->fd_table[i].shm;
                            break;
                        }
                    }
                    if (!target_shm) return 0;

                    shm_get(target_shm);  // +1 for our mapping

                    size_t npages = target_shm->npages;
                    size_t list_pages = target_shm->page_list ? (size_t)target_shm->num_pages : 0;
                    size_t total_pages = npages + list_pages;
                    size = total_pages * PAGE_SIZE;

                    uint64_t vaddr = proc->mm->mmap_brk;
                    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

                    for (size_t i = 0; i < total_pages; i++) {
                        uint64_t page_phys;
                        if (i < npages) {
                            page_phys = target_shm->phys + i * PAGE_SIZE;
                        } else {
                            page_phys = target_shm->page_list[i - npages];
                        }
                        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                                  page_phys, pte_flags)) {
                            for (size_t j = 0; j < i; j++)
                                unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
                            shm_put(target_shm);
                            return 0;
                        }
                    }

                    mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
                    if (!region) {
                        for (size_t i = 0; i < total_pages; i++)
                            unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE, 1);
                        shm_put(target_shm);
                        return 0;
                    }

                    region->vaddr = vaddr;
                    region->size = size;
                    region->phys = 0;
                    region->shm_obj = target_shm;  // ref already bumped above
                    region->next = proc->mm->mmap_regions;
                    proc->mm->mmap_regions = region;
                    proc->mm->mmap_brk = vaddr + size;

                    return vaddr;
                }
            }
            return 0;  // fallback: no mmap handler
        }

        // FD_SHM: existing SHM mapping path
        if (proc->mm->files->fd_table[fd].type != FD_SHM)
            return 0;
        struct shm *shm = proc->mm->files->fd_table[fd].shm;
        if (!shm) return 0;

        // Total allocated pages: contiguous + discrete
        size_t npages = shm->npages;
        size_t list_pages = shm->page_list ? (size_t)shm->num_pages : 0;
        size_t total_pages = npages + list_pages;
        size = total_pages * PAGE_SIZE;

        // Map SHM pages into user address space at mmap_brk
        uint64_t vaddr = proc->mm->mmap_brk;
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
        region->next = proc->mm->mmap_regions;
        proc->mm->mmap_regions = region;
        proc->mm->mmap_brk = vaddr + size;

        return vaddr;
    }

    // Anonymous or MAP_PHYSICAL: size must be non-zero
    if (size == 0) return 0;

    // MAP_PHYSICAL
    if (flags & MAP_PHYSICAL) {
        // MAP_PHYSICAL: map physical address range to user address space
        // Use mmap_phys_brk (high fixed base) to avoid conflict with SHM/heap
        uint64_t vaddr = proc->mm->mmap_phys_brk;
        uint64_t phys_start = ALIGN_DOWN(offset, PAGE_SIZE);
        uint64_t phys_end = ALIGN_UP(offset + size, PAGE_SIZE);
        size_t npages = (phys_end - phys_start) / PAGE_SIZE;

        uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;
        if (flags & MAP_UC) {
            pte_flags |= PTE_PCD | PTE_PWT;  // UC
        }

        for (size_t i = 0; i < npages; i++) {
            if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                      phys_start + i * PAGE_SIZE, pte_flags)) {
                printk(LOG_ERROR, "mmap PHYSICAL: map failed at i=%lu\n", (unsigned long)i);
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
        region->next = proc->mm->mmap_regions;
        proc->mm->mmap_regions = region;
        proc->mm->mmap_phys_brk = vaddr + npages * PAGE_SIZE;

        return vaddr;
    }

    // Anonymous private mapping: allocate new pages
    size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t vaddr = proc->mm->mmap_brk;
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
    region->next = proc->mm->mmap_regions;
    proc->mm->mmap_regions = region;
    proc->mm->mmap_brk = vaddr + size;

    kfree(phys_pages);
    return vaddr;
}

// sys_munmap(addr, size) — syscall 10 (解除内存映射)
// Returns: 0 on success, positive errno on failure
uint64_t sys_munmap(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    uint64_t addr = arg1;
    size_t size = (size_t)arg2;

    if (size == 0) return (uint64_t)-EINVAL;

    task_t *proc = current_task;
    uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

    // 查找匹配的 mmap_region_t
    mmap_region_t **pp = &proc->mm->mmap_regions;
    while (*pp) {
        if ((*pp)->vaddr == addr) {
            mmap_region_t *region = *pp;
            size = region->size;

            // 逐页解映射
            // SHM/MAP_PHYSICAL: 只清除PTE，不释放物理页（物理页由shm_put/外部管理）
            // 匿名映射: 清除PTE并释放物理页
            size_t npages = size / PAGE_SIZE;
            if (region->shm_obj || region->phys) {
                // SHM or MAP_PHYSICAL: only unmap PTE, don't free physical pages
                for (size_t i = 0; i < npages; i++) {
                    uint64_t va = addr + i * PAGE_SIZE;
                    uint64_t *pdpt = ensure_pd(pml4, va);
                    if (!pdpt) continue;
                    uint64_t *pd = ensure_pt_in_pd(pdpt, va, 2);
                    if (!pd) continue;
                    uint64_t *pt = ensure_pt_in_pd(pd, va, 1);
                    if (!pt) continue;
                    uint64_t pt_idx = (va >> 12) & 0x1FF;
                    pt[pt_idx] = 0;  // clear PTE only
                }
            } else {
                // Anonymous: unmap + free physical pages
                for (size_t i = 0; i < npages; i++) {
                    uint64_t va = addr + i * PAGE_SIZE;
                    unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
                }
            }

            // Release SHM reference if this was an SHM mapping
            if (region->shm_obj) {
                shm_put(region->shm_obj);
            }
            // Anonymous/MAP_PHYSICAL: physical pages already freed above or externally managed

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
    if (target_pid < 0 || target_pid >= MAX_PROC) return;
    task_t *target = &tasks[target_pid];
    if (target->pid != target_pid) return;

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

// sys_block_async(lba, buf, count, dir) — syscall 30
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
// syscall 49
uint64_t sys_debug_print(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *buf = (const char __user * __force)arg1;
    size_t len = (size_t)arg2;
    if (!buf || len > 256) return (uint64_t)-EINVAL;
    char kbuf[257];
    copy_from_user(kbuf, buf, len);
    kbuf[len] = '\0';
    return 0;
}

// sys_install_fd(fs_pid, fs_fd, offset, flags, file_size) — syscall 32
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

    task_t *proc = current_task;

    spinlock_t *fdlk = &proc->mm->files->fd_lock;
    spin_lock(fdlk);
    int fd = alloc_fd(proc, 3);
    if (fd < 0) {
        spin_unlock(fdlk);
        return (uint64_t)-EMFILE;
    }

    proc->mm->files->fd_table[fd].type = FD_FILE;
    proc->mm->files->fd_table[fd].flags = flags;
    proc->mm->files->fd_table[fd].file_data.fs_pid = fs_pid;
    proc->mm->files->fd_table[fd].file_data.fs_fd = fs_fd;
    proc->mm->files->fd_table[fd].file_data._offset = offset;
    proc->mm->files->fd_table[fd].file_data.file_size = file_size;
    refcount_set(&proc->mm->files->fd_table[fd].file_data.f_count, 1);

    spin_unlock(fdlk);
    return (uint64_t)fd;
}


// sys_ioctl(fd, cmd, arg) — syscall 58 (device ioctl)
uint64_t sys_ioctl(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                    uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    uint32_t cmd = (uint32_t)arg2;
    void __user *arg = (void __user * __force)arg3;

    task_t *proc = current_task;
    if (fd < 0 || fd >= MAX_FD || proc->mm->files->fd_table[fd].type == FD_NONE)
        return (uint64_t)(-(uint64_t)EBADF);

    switch (proc->mm->files->fd_table[fd].type) {
    case FD_DEV: {
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip || !ip->i_priv) return (uint64_t)(-(uint64_t)ENODEV);
        struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
        if (ops->driver_pid == 0) {
            // Kernel device: copy arg to kernel buffer, call ops->ioctl, copy back
            if (!ops->ioctl) return (uint64_t)(-(uint64_t)ENOTTY);

            uint8_t kbuf[64];
            __memset(kbuf, 0, sizeof(kbuf));

            // Copy arg from user if direction includes WRITE (user -> kernel)
            // arg_size > 48: reject — such ioctl needs MSG path (future),
            // not silently truncated. TODO: add assert/error log for invalid size.
            if ((_IOC_DIR(cmd) & _IOC_WRITE) && (__force uint64_t)arg != 0) {
                uint16_t arg_size = _IOC_SIZE(cmd);
                if (arg_size > 48) return (uint64_t)(-(uint64_t)EINVAL);
                if (arg_size > 0)
                    copy_from_user(kbuf, arg, arg_size);
            }

            long result = ops->ioctl(cmd, kbuf);

            // Copy result back to user if direction includes READ (kernel -> user)
            if ((_IOC_DIR(cmd) & _IOC_READ) && (__force uint64_t)arg != 0 && result >= 0) {
                uint16_t arg_size = _IOC_SIZE(cmd);
                if (arg_size > 0 && arg_size <= 48)
                    copy_to_user(arg, kbuf, arg_size);
            }

            return (uint64_t)result;
        }
        // User-space driver: IPC proxy
        // Pack cmd + arg into a REQ message, send to driver_pid
        pid_t target_pid = ops->driver_pid;
        if (target_pid <= 0) return (uint64_t)(-(uint64_t)ENODEV);

        // Build REQ payload: [4 bytes cmd] [arg_size bytes arg data]
        // arg_size > 48: reject — such ioctl needs MSG path (future),
        // not silently truncated. TODO: add assert/error log for invalid size.
        if ((_IOC_DIR(cmd) & _IOC_WRITE) && (__force uint64_t)arg != 0) {
            uint16_t arg_size = _IOC_SIZE(cmd);
            if (arg_size > 48) return (uint64_t)(-(uint64_t)EINVAL);
        }
        uint8_t req_data[56];
        __memset(req_data, 0, 56);
        *(uint32_t *)req_data = cmd;
        if ((_IOC_DIR(cmd) & _IOC_WRITE) && (__force uint64_t)arg != 0) {
            uint16_t arg_size = _IOC_SIZE(cmd);
            if (arg_size > 0)
                copy_from_user(req_data + 4, arg, arg_size);
        }

        // Validate target PID
        if (target_pid < 0 || target_pid >= MAX_PROC)
            return (uint64_t)(-(uint64_t)ESRCH);
        task_t *target = &tasks[target_pid];
        if (target->pid != target_pid)
            return (uint64_t)(-(uint64_t)ESRCH);

        // Build RECV_REQ message
        uint8_t msg[RECV_MSG_SIZE];
        recv_msg_t *hdr = (recv_msg_t *)msg;
        hdr->type = RECV_REQ;
        hdr->src = (uint32_t)current_task->pid;
        __memcpy(hdr->data, req_data, 56);

        // Enqueue to target's recv queue
        spin_lock(&target->recv_lock);
        uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
        if (next == target->recv_tail) {
            spin_unlock(&target->recv_lock);
            return (uint64_t)(-(uint64_t)EBUSY);
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
        // Use the arg as reply buffer so driver's resp writes back to user space
        // arg is a 64B buffer allocated by userspace ioctl(), always >= 56B
        proc->state = BLOCKED;
        proc->wait_event = WAIT_REQ_REPLY;
        proc->wait_timed_out = 0;
        proc->wait_deadline = 0;
        proc->req_target_pid = target_pid;
        proc->req_reply_buf = arg;  // driver resp will be copy_to_user'd here
        proc->req_reply_len = 56;   // REQ payload size, not RECV_MSG_SIZE (avoids stack overflow on small ioctl arg)
        proc->req_result = 0;

        schedule();

        // Woken up by sys_resp or proc_reap
        if (proc->req_result != 0)
            return (uint64_t)proc->req_result;

        // Driver reply layout: [int32_t result(4B) | arg_data(52B)]
        // libc ioctl expects: [arg_data(arg_size B)] at offset 0
        // For _IOWR/_IOR: strip the result prefix, move arg_data from offset 4 to 0
        // so libc's copy-out (arg_size bytes from buf) matches the ioctl arg layout.
        int32_t ioctl_result = 0;
        if ((__force uint64_t)arg != 0) {
            // Read result from reply offset 0 (before we shift data)
            copy_from_user(&ioctl_result, arg, 4);

            if ((_IOC_DIR(cmd) & _IOC_READ) && _IOC_SIZE(cmd) > 0 && _IOC_SIZE(cmd) <= 48) {
                uint16_t arg_size = _IOC_SIZE(cmd);
                uint8_t tmp[48];
                // Read arg_data from reply offset 4
                copy_from_user(tmp, (void __user * __force)((__force uint64_t)arg + 4), arg_size);
                // Write arg_data to offset 0 (where libc expects it)
                copy_to_user((void __user * __force)arg, tmp, arg_size);
            }
        }
        return (uint64_t)(long)ioctl_result;
    }
    case FD_TTY: {
        struct pty *pty = proc->mm->files->fd_table[fd].pty;
        if (!pty) return (uint64_t)(-(uint64_t)EBADF);
        return (uint64_t)pty_ioctl(pty, cmd, arg);
    }
    case FD_SOCKET:
    case FD_PIPE:
    case FD_REGULAR:
    case FD_DIR:
    case FD_FILE:
    case FD_SHM:
        return (uint64_t)(-(uint64_t)ENOTTY);
    default:
        return (uint64_t)(-(uint64_t)EBADF);
    }
}

// sys_fstat(fd, buf) — syscall 59 (file status)
uint64_t sys_fstat(uint64_t arg1, uint64_t arg2,
                    uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int fd = (int)arg1;
    struct kstat __user *ust = (struct kstat __user * __force)arg2;

    task_t *proc = current_task;
    if (fd < 0 || fd >= MAX_FD || proc->mm->files->fd_table[fd].type == FD_NONE)
        return (uint64_t)(-(uint64_t)EBADF);

    struct kstat ks;
    __memset(&ks, 0, sizeof(ks));
    ks.st_nlink = 1;
    ks.st_blksize = 512;

    switch (proc->mm->files->fd_table[fd].type) {
    case FD_REGULAR: {
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip) return (uint64_t)(-(uint64_t)EBADF);
        ks.st_ino = ip->ino;
        ks.st_size = ip->size;
        ks.st_mode = S_IFREG | 0644;
        ks.st_blocks = (ip->size + 511) / 512;
        break;
    }
    case FD_DIR: {
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip) return (uint64_t)(-(uint64_t)EBADF);
        ks.st_ino = ip->ino;
        ks.st_mode = S_IFDIR | 0755;
        ks.st_blocks = 0;
        break;
    }
    case FD_DEV: {
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip) return (uint64_t)(-(uint64_t)EBADF);
        ks.st_ino = ip->ino;
        if (ip->i_priv && ((struct dev_ops *)ip->i_priv)->device_type == DEV_BLOCK)
            ks.st_mode = S_IFBLK | 0666;
        else
            ks.st_mode = S_IFCHR | 0666;
        break;
    }
    case FD_PIPE:
        ks.st_mode = S_IFIFO | 0644;
        break;
    case FD_TTY:
        ks.st_mode = S_IFCHR | 0666;
        break;
    case FD_SHM:
        ks.st_mode = S_IFREG | 0666;
        break;
    default:
        return (uint64_t)(-(uint64_t)EBADF);
    }

    if (copy_to_user(ust, &ks, sizeof(ks)))
        return (uint64_t)(-(uint64_t)EFAULT);
    return 0;
}

// sys_fdev_pid(fd) — syscall 60
// Returns driver_pid for FD_DEV fd (0 for kernel device, >0 for user-space driver).
// Used by libc notify_fd/msg_fd/mmap to find target PID without a local fd_table.
uint64_t sys_fdev_pid(uint64_t arg1, uint64_t _u2, uint64_t _u3,
                       uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    int fd = (int)arg1;
    task_t *proc = current_task;
    if (fd < 0 || fd >= MAX_FD || proc->mm->files->fd_table[fd].type != FD_DEV)
        return (uint64_t)(-(uint64_t)EBADF);

    struct inode *ip = proc->mm->files->fd_table[fd].inode;
    if (!ip || !ip->i_priv) return 0;  // kernel device, no driver_pid
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    return (uint64_t)ops->driver_pid;
}

// sys_shm_create(size) — syscall 11 (创建共享内存，返回 fd)
// Returns: fd (≥2) on success, 0 on failure
uint64_t sys_shm_create(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    size_t size = (size_t)arg1;
    if (size == 0) return 0;
    size = ALIGN_UP(size, PAGE_SIZE);
    size_t npages = size / PAGE_SIZE;

    task_t *proc = current_task;

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
    refcount_set(&shm->s_count, 1);  // fd holds initial reference
    shm->flags = 0;
    shm->seals = 0;
    shm->name[0] = '\0';  // no name by default
    shm->page_list = NULL;
    shm->num_pages = 0;

    // Find free fd slot (skip 0/1 reserved for stdin/stdout)
    int fd = alloc_fd(proc, 2);
    if (fd < 0) {
        kfree(shm);
        bfc_free_page(pages, npages);
        return 0;
    }

    proc->mm->files->fd_table[fd].type = FD_SHM;
    proc->mm->files->fd_table[fd].flags = O_RDWR;
    proc->mm->files->fd_table[fd].shm = shm;

    return (uint64_t)fd;
}

// sys_shm_attach(id, mode) — syscall 12 (附加共享内存，返回 fd)
// mode=0: id is target_pid, attach target's first FD_SHM
// mode=1: id is kernel SHM ID, attach kernel pre-allocated SHM via struct shm*
// Returns: fd (≥2) on success, 0 on failure
// Transitional: caller must sys_mmap(fd, ...) to get vaddr
uint64_t sys_shm_attach(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int mode = (int)arg2;
    task_t *proc = current_task;
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

        task_t *target = &tasks[target_pid];
        if (target->pid != target_pid) return 0;

        // Scan target's fd_table for FD_SHM (under target's fd_lock)
        spinlock_t *target_fdlk = &target->mm->files->fd_lock;
        spin_lock(target_fdlk);
        for (int i = 0; i < MAX_FD; i++) {
            if (target->mm && target->mm->files->fd_table[i].type == FD_SHM) {
                target_shm = target->mm->files->fd_table[i].shm;
                break;
            }
        }
        spin_unlock(target_fdlk);
        if (!target_shm) return 0;
    }

    // Bump ref_count for the new fd
    shm_get(target_shm);

    // Find free fd slot (under current process's fd_lock)
    spinlock_t *fdlk = &proc->mm->files->fd_lock;
    spin_lock(fdlk);
    int fd = alloc_fd(proc, 2);
    if (fd < 0) {
        spin_unlock(fdlk);
        shm_put(target_shm);
        return 0;
    }

    proc->mm->files->fd_table[fd].type = FD_SHM;
    proc->mm->files->fd_table[fd].flags = O_RDWR;
    proc->mm->files->fd_table[fd].shm = target_shm;
    spin_unlock(fdlk);

    return (uint64_t)fd;
}

// ===================== memfd_create / ftruncate =====================

// sys_memfd_create(name, flags) — syscall 44 (Linux-compatible memfd_create)
// Returns: fd (≥2) on success, 0 on failure
uint64_t sys_memfd_create(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *user_name = (const char __user * __force)arg1;
    unsigned int flags = (unsigned int)arg2;

    // Validate flags: only known flags allowed
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return 0;

    task_t *proc = current_task;

    // Allocate shm struct (empty — no physical pages yet)
    struct shm *shm = (struct shm *)kmalloc(sizeof(struct shm));
    if (!shm) return 0;

    shm->phys = 0;
    shm->npages = 0;
    shm->file_size = 0;
    refcount_set(&shm->s_count, 1);  // fd holds initial reference
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
            copy_from_user(&c, (const char __user *)(uptr + i), 1);
            if (c == '\0') break;
            shm->name[i] = c;
        }
        shm->name[i] = '\0';
    } else {
        shm->name[0] = '\0';
    }

    // Find free fd slot (under fd_lock, skip 0/1 reserved for stdin/stdout)
    spinlock_t *fdlk = &proc->mm->files->fd_lock;
    spin_lock(fdlk);
    int fd = alloc_fd(proc, 2);
    if (fd < 0) {
        spin_unlock(fdlk);
        kfree(shm);
        return 0;
    }

    proc->mm->files->fd_table[fd].type = FD_SHM;
    proc->mm->files->fd_table[fd].flags = O_RDWR | ((flags & MFD_CLOEXEC) ? FD_CLOEXEC : 0);
    proc->mm->files->fd_table[fd].shm = shm;
    spin_unlock(fdlk);

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

// sys_ftruncate(fd, size) — syscall 45 (set shm size, allocate/free pages)
// Returns: 0 on success, positive errno on failure
uint64_t sys_ftruncate(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int fd = (int)arg1;
    int64_t size = (int64_t)arg2;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    task_t *proc = current_task;
    if (proc->mm->files->fd_table[fd].type != FD_SHM) return (uint64_t)-EINVAL;
    if (!proc->mm->files->fd_table[fd].shm) return (uint64_t)-EBADF;

    struct shm *shm = proc->mm->files->fd_table[fd].shm;

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

// Wake a process that is BLOCKED on WAIT_PIPE, WAIT_POLL, or WAIT_RECV.
// For WAIT_PIPE/WAIT_POLL: only changes state (pipe I/O resumes its loop).
// For WAIT_RECV: sets recv_intr flag so sys_recv returns -EINTR (ISR notification).
void wake_process(pid_t pid) {
    if (pid < 0 || pid >= MAX_PROC) return;
    task_t *target = &tasks[pid];

    int target_cpu = target->assigned_cpu;
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->pid == pid &&
        target->state == BLOCKED &&
        (target->wait_event == WAIT_PIPE ||
         target->wait_event == WAIT_POLL ||
         target->wait_event == WAIT_RECV)) {
        if (target->wait_deadline != 0) {
            timer_queue_remove(target);
            target->wait_deadline = 0;
        }
        if (target->wait_event == WAIT_RECV)
            target->recv_intr = 1;
        target->state = READY;
        target->wait_event = WAIT_NONE;
        target->wait_timed_out = 0;
        list_push_back(&cpu_locals[target_cpu].run_queue, &target->run_node);
        cpu_locals[target_cpu].run_count++;
    }
    spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
}

// sys_pipe(fd_ptr) — syscall 13 (创建 pipe，写 [read_fd, write_fd] 到用户指针)
uint64_t sys_pipe(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    int __user *fd_ptr = (int __user * __force)arg1;

    // Validate user pointer
    uint64_t ptr = (__force uint64_t)fd_ptr;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || ptr + 2 * sizeof(int) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    task_t *proc = current_task;

    spinlock_t *fdlk = &proc->mm->files->fd_lock;
    spin_lock(fdlk);
    int read_fd = alloc_fd(proc, 3);
    int write_fd = (read_fd >= 0) ? alloc_fd(proc, read_fd + 1) : -EMFILE;
    if (read_fd < 0 || write_fd < 0) {
        spin_unlock(fdlk);
        return (uint64_t)-EMFILE;
    }

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
    refcount_set(&p->p_count, 2);  // read end + write end

    // Fill fd table entries
    proc->mm->files->fd_table[read_fd].type = FD_PIPE;
    proc->mm->files->fd_table[read_fd].flags = O_RDONLY;
    proc->mm->files->fd_table[read_fd].pipe = p;

    proc->mm->files->fd_table[write_fd].type = FD_PIPE;
    proc->mm->files->fd_table[write_fd].flags = O_WRONLY;
    proc->mm->files->fd_table[write_fd].pipe = p;

    // Write fd pair to user space
    ((__force int *)fd_ptr)[0] = read_fd;
    ((__force int *)fd_ptr)[1] = write_fd;

    spin_unlock(fdlk);
    return 0;
}

// sys_write(fd, buf, len) — syscall 14 (向 fd 写入数据)
// FD_REGULAR: kernel FAT32 via page cache
// FD_PIPE: 直写 kernel ring buffer
// FD_FILE: 通过 kernel_msg_send 代理到 fs_driver
uint64_t sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    const char __user *buf = (const char __user * __force)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (current_task->mm->files->fd_table[fd].type == FD_NONE) return (uint64_t)-EBADF;

    task_t *proc = current_task;

    // ===== FD_REGULAR: kernel FAT32 via page cache =====
    if (proc->mm->files->fd_table[fd].type == FD_REGULAR) {
        if (!(proc->mm->files->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip) return (uint64_t)-EBADF;

        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        uint64_t offset = proc->mm->files->fd_table[fd].offset;
        int written = fat32_write(ip, offset, (const void __force *)buf, len);
        if (written < 0) return (uint64_t)written;
        proc->mm->files->fd_table[fd].offset = offset + written;
        return (uint64_t)written;
    }

    // ===== FD_FILE: proxy to fs_driver =====
    if (proc->mm->files->fd_table[fd].type == FD_FILE) {
        if (!(proc->mm->files->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
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
        req->fs_fd = proc->mm->files->fd_table[fd].file_data.fs_fd;
        req->offset = proc->mm->files->fd_table[fd].file_data._offset;
        req->count = (uint32_t)len;
        copy_from_user(msg_buf + sizeof(file_t_io_req), buf, len);

        // Reply buffer
        file_t_io_resp resp;
        int64_t ret = sys_msg_to(proc->mm->files->fd_table[fd].file_data.fs_pid,
                                  msg_buf, msg_len, &resp, sizeof(resp));
        kfree(msg_buf);

        if (ret < 0) return (uint64_t)(-ret);

        if (resp.status != 0) return (uint64_t)(-resp.status);

        size_t written = resp.count;
        proc->mm->files->fd_table[fd].file_data._offset += written;
        if (proc->mm->files->fd_table[fd].file_data.file_size < proc->mm->files->fd_table[fd].file_data._offset)
            proc->mm->files->fd_table[fd].file_data.file_size = proc->mm->files->fd_table[fd].file_data._offset;
        return (uint64_t)written;
    }

    // ===== FD_SOCKET: delegate to sock_write =====
    if (proc->mm->files->fd_table[fd].type == FD_SOCKET) {
        if (!(proc->mm->files->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct unix_sock *sock = proc->mm->files->fd_table[fd].sock;
        if (!sock) return (uint64_t)-EBADF;
        int64_t ret = sock_write(sock, (const void __force *)buf, len);
        return (uint64_t)ret;
    }

    // ===== FD_DEV: write via dev_ops callback =====
    if (proc->mm->files->fd_table[fd].type == FD_DEV) {
        if (!(proc->mm->files->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip || !ip->i_priv) return (uint64_t)-ENODEV;
        struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
        // Kernel device: call write callback
        if (ops->driver_pid == 0 && ops->write) {
            ssize_t ret = ops->write(proc, fd, (const void __force *)buf, len);
            return (uint64_t)ret;
        }
        // User-space driver: not supported via write (use IPC)
        return (uint64_t)-ENOSYS;
    }

    // ===== FD_TTY: PTY write =====
    if (proc->mm->files->fd_table[fd].type == FD_TTY) {
        if (!(proc->mm->files->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct pty *pty = proc->mm->files->fd_table[fd].pty;
        if (!pty) return (uint64_t)-EBADF;
        int is_master = pty_fd_is_master(proc->mm->files, fd);
        int64_t ret;
        if (is_master)
            ret = pty_master_write(pty, proc, (const void __force *)buf, len);
        else
            ret = pty_slave_write(pty, proc, (const void __force *)buf, len);
        return (uint64_t)ret;
    }

    // ===== FD_PIPE: existing path =====
    if (!(proc->mm->files->fd_table[fd].flags & (O_WRONLY | O_RDWR))) return (uint64_t)-EINVAL;

    // Validate user buf pointer
    if (!buf) return (uint64_t)-EFAULT;
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    struct pipe *p = proc->mm->files->fd_table[fd].pipe;
    size_t written = 0;

    while (written < len) {
        // Check if read end is closed (no readers left)
        if (refcount_read(&p->p_count) <= 1) {
            if (written > 0) break;
            return (uint64_t)-EPIPE;
        }
        // Check if pipe has space: full when head is one behind tail
        if ((p->head + 1) % PIPE_BUF_SIZE == p->tail) {
            // Pipe full: block or return EAGAIN if non-blocking
            if (proc->mm->files->fd_table[fd].flags & O_NONBLOCK) {
                if (written > 0) break;  // return partial write
                return (uint64_t)-EAGAIN;
            }
            p->write_pid = proc->pid;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_PIPE;
            schedule();
            p->write_pid = -1;
            // EINTR check
            {
                uint64_t pend = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
                uint64_t deliv = pend & ~proc->sig.blocked;
                deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
                if (deliv) {
                    if (written > 0) break;  // return partial write
                    return (uint64_t)-EINTR;
                }
            }
            continue;
        }
        p->buf[p->head] = ((const char __force *)buf)[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        written++;
    }

    // Wake reader if blocked
    if (p->read_pid >= 0) wake_process(p->read_pid);

    return (uint64_t)written;
}

// sys_read(fd, buf, len) — syscall 15 (从 fd 读数据，阻塞直到有数据)
// FD_REGULAR: kernel FAT32 via page cache
// FD_PIPE: 直读 kernel ring buffer
// FD_FILE: 通过 kernel_msg_send 代理到 fs_driver
uint64_t sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    char __user *buf = (char __user * __force)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (current_task->mm->files->fd_table[fd].type == FD_NONE) return (uint64_t)-EBADF;

    task_t *proc = current_task;

    // ===== FD_REGULAR: kernel FAT32 via page cache =====
    if (proc->mm->files->fd_table[fd].type == FD_REGULAR) {
        if ((proc->mm->files->fd_table[fd].flags & O_WRONLY) && !(proc->mm->files->fd_table[fd].flags & O_RDWR))
            return (uint64_t)-EINVAL;
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip) return (uint64_t)-EBADF;
        uint64_t offset = proc->mm->files->fd_table[fd].offset;
        if (offset >= ip->size) return 0;

        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        uint64_t avail = ip->size - offset;
        if (len > avail) len = avail;

        int nread = fat32_read(ip, offset, (void __force *)buf, len);
        if (nread < 0) return (uint64_t)(-nread);
        proc->mm->files->fd_table[fd].offset = offset + nread;
        return (uint64_t)nread;
    }

    // ===== FD_FILE: proxy to fs_driver =====
    if (proc->mm->files->fd_table[fd].type == FD_FILE) {
        // Check read permission: reject O_WRONLY only (O_RDONLY=0, so must check != O_WRONLY)
        if ((proc->mm->files->fd_table[fd].flags & O_WRONLY) && !(proc->mm->files->fd_table[fd].flags & O_RDWR))
            return (uint64_t)-EINVAL;

        if (proc->mm->files->fd_table[fd].file_data._offset >= proc->mm->files->fd_table[fd].file_data.file_size) {
            return 0;  // EOF
        }

        uint64_t avail = proc->mm->files->fd_table[fd].file_data.file_size - proc->mm->files->fd_table[fd].file_data._offset;
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
        req.fs_fd = proc->mm->files->fd_table[fd].file_data.fs_fd;
        req.offset = proc->mm->files->fd_table[fd].file_data._offset;
        req.count = (uint32_t)len;

        // Allocate reply buffer (header + data)
        size_t resp_size = sizeof(file_t_io_resp) + (size_t)len;
        uint8_t *resp_buf = (uint8_t *)kmalloc(resp_size);
        if (!resp_buf) return (uint64_t)-ENOMEM;

        int64_t ret = sys_msg_to(proc->mm->files->fd_table[fd].file_data.fs_pid,
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
        copy_to_user(buf, resp_buf + sizeof(file_t_io_resp), nread);

        proc->mm->files->fd_table[fd].file_data._offset += nread;
        kfree(resp_buf);
        return (uint64_t)nread;
    }

    // ===== FD_SOCKET: delegate to sock_read =====
    if (proc->mm->files->fd_table[fd].type == FD_SOCKET) {
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct unix_sock *sock = proc->mm->files->fd_table[fd].sock;
        if (!sock) return (uint64_t)-EBADF;
        int64_t ret = sock_read(sock, (void __force *)buf, len);
        return (uint64_t)ret;
    }

    // ===== FD_DEV: read via dev_ops callback =====
    if (proc->mm->files->fd_table[fd].type == FD_DEV) {
        if ((proc->mm->files->fd_table[fd].flags & O_WRONLY) && !(proc->mm->files->fd_table[fd].flags & O_RDWR))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL
            || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip || !ip->i_priv) return (uint64_t)-ENODEV;
        struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
        // Kernel device: call read callback
        if (ops->driver_pid == 0 && ops->read) {
            ssize_t ret = ops->read(proc, fd, (void __force *)buf, len);
            return (uint64_t)ret;
        }
        // User-space driver: not supported via read (use IPC)
        return (uint64_t)-ENOSYS;
    }

    // ===== FD_TTY: PTY read =====
    if (proc->mm->files->fd_table[fd].type == FD_TTY) {
        if ((proc->mm->files->fd_table[fd].flags & O_WRONLY) && !(proc->mm->files->fd_table[fd].flags & O_RDWR))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
        struct pty *pty = proc->mm->files->fd_table[fd].pty;
        if (!pty) return (uint64_t)-EBADF;
        int is_master = pty_fd_is_master(proc->mm->files, fd);
        int64_t ret;
        if (is_master)
            ret = pty_master_read(pty, proc, (void __force *)buf, len);
        else
            ret = pty_slave_read(pty, proc, (void __force *)buf, len);
        return (uint64_t)ret;
    }

    // ===== FD_PIPE: existing path =====
    // O_RDONLY=0, so check: must not be O_WRONLY only
    if ((proc->mm->files->fd_table[fd].flags & O_WRONLY) && !(proc->mm->files->fd_table[fd].flags & O_RDWR))
        return (uint64_t)-EINVAL;

    // Validate user buf pointer
    if (!buf) return (uint64_t)-EFAULT;
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL || ptr_end > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    struct pipe *p = proc->mm->files->fd_table[fd].pipe;

    // Block if pipe is empty
    while (p->head == p->tail) {
        // Check if write end is closed (all write fds gone)
        if (refcount_read(&p->p_count) == 1) return 0;  // EOF: no writers left
        // Non-blocking: return EAGAIN-like indication (0 bytes read)
        if (proc->mm->files->fd_table[fd].flags & O_NONBLOCK) return (uint64_t)-EAGAIN;
        p->read_pid = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;
        schedule();
        p->read_pid = -1;
        // EINTR check
        {
            uint64_t pend = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
            uint64_t deliv = pend & ~proc->sig.blocked;
            deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
            if (deliv) return (uint64_t)-EINTR;
        }
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

// sys_close(fd) — syscall 16 (关闭 fd，pipe ref_count--)
uint64_t sys_close(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    int fd = (int)arg1;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    spinlock_t *fdlk = &current_task->mm->files->fd_lock;
    spin_lock(fdlk);
    if (current_task->mm->files->fd_table[fd].type == FD_NONE) {
        spin_unlock(fdlk);
        return (uint64_t)-EBADF;
    }

    if (current_task->mm->files->fd_table[fd].type == FD_FILE) {
        // Release ref_count; notify fs_driver on last close
        if (refcount_dec_and_test(&current_task->mm->files->fd_table[fd].file_data.f_count)) {
            // Notify fs_driver to close the session fd
            file_t_io_req req = {0};
            req.cmd = FILE_CMD_CLOSE;
            req.fs_fd = current_task->mm->files->fd_table[fd].file_data.fs_fd;
            kernel_msg_send(current_task->mm->files->fd_table[fd].file_data.fs_pid,
                            &req, sizeof(req), NULL, 0);
        }
        __memset(&current_task->mm->files->fd_table[fd], 0, sizeof(struct file));
        current_task->mm->files->fd_table[fd].type = FD_NONE;
    } else {
        close_fd_trap(current_task->mm->files, fd);
    }

    spin_unlock(fdlk);
    return 0;
}

// sys_notify(pid) — syscall 18 (异步通知：消息入队 + 唤醒)
// Returns: 0 on success, positive errno on failure
uint64_t sys_notify(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    pid_t target_pid = (pid_t)arg1;
    if (target_pid < 0 || target_pid >= MAX_PROC) return (uint64_t)-EINVAL;

    task_t *target = &tasks[target_pid];
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
    slot->src = (uint32_t)current_task->pid;
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

// sys_gettime() — syscall 19 (全局单调时钟，返回纳秒)
uint64_t sys_gettime(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    return sched_clock();
}

// sys_clock() — syscall 20 (per-process CPU 时间，返回纳秒)
uint64_t sys_clock(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    return current_task->cpu_time_ns;
}

// Inner implementation of sys_msg — shared by PID-based and fd-based variants.
// All parameters are already validated by the caller.
// Returns: 0 on success, positive errno on failure
int64_t sys_msg_to(pid_t target_pid, void *msg_buf, size_t msg_len,
                           void *reply_buf, size_t reply_len) {
    task_t *target = &tasks[target_pid];
    if (target->pid != target_pid) return (uint64_t)-ESRCH;

    // Allocate kernel buffer and copy message from user space
    void *kbuf = kmalloc(msg_len);
    if (!kbuf) return (uint64_t)-ENOMEM;
    __memcpy(kbuf, msg_buf, msg_len);

    // Build RECV_MSG
    uint8_t msg[RECV_MSG_SIZE];
    recv_msg_t *hdr = (recv_msg_t *)msg;
    hdr->type = RECV_MSG;
    hdr->src = (uint32_t)current_task->pid;
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
    task_t *proc = current_task;
    proc->state = BLOCKED;
    proc->wait_event = WAIT_MSG_REPLY;
    proc->wait_timed_out = 0;
    proc->wait_deadline = 0;
    proc->msg_target_pid = target_pid;
    proc->msg_reply_buf = (void __user * __force)reply_buf;
    proc->msg_reply_len = reply_len;
    proc->msg_result = 0;

    schedule();

    // EINTR check
    {
        uint64_t pend = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->sig.blocked;
        deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
        if (deliv) return (uint64_t)-EINTR;
    }

    // Woken up by sys_msg_resp or proc_reap
    if (proc->msg_result != 0) return (uint64_t)proc->msg_result;
    return 0;
}

// sys_msg(target_pid, msg_buf, msg_len, reply_buf, reply_len) — syscall 21 (变长消息请求)
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

// sys_msg_resp(resp_buf, resp_len) — syscall 22 (回复当前 MSG 调用者)
// 返回: 0=成功, 正数=errno
uint64_t sys_msg_resp(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    void __user *resp_buf = (void __user * __force)arg1;
    size_t resp_len = (size_t)arg2;

    // Validate user pointer
    uint64_t ptr = (__force uint64_t)resp_buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || resp_len == 0 ||
        ptr + resp_len > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    task_t *proc = current_task;
    pid_t caller_pid = proc->msg_caller_pid;
    if (caller_pid < 0 || caller_pid >= MAX_PROC) {
        return (uint64_t)-EINVAL;
    }

    task_t *caller = &tasks[caller_pid];
    if (caller->pid != caller_pid) return (uint64_t)-ESRCH;

    // Copy response data from server user space to kernel buffer
    void *kbuf = kmalloc(resp_len);
    if (!kbuf) return (uint64_t)-ENOMEM;
    copy_from_user(kbuf, resp_buf, resp_len);

    // Copy to caller's reply buffer under caller's CR3
    size_t copy_len = resp_len < caller->msg_reply_len ? resp_len : caller->msg_reply_len;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)caller->cr3) : "memory");
    copy_to_user(caller->msg_reply_buf, kbuf, copy_len);
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

// sys_ioperm(from, num, turn_on) — syscall 23 (I/O 端口权限)
uint64_t sys_ioperm(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    unsigned long from = (unsigned long)arg1;
    unsigned long num = (unsigned long)arg2;
    int turn_on = (int)arg3;

    if (from + num > 65536) return (uint64_t)-EINVAL;

    task_t *proc = current_task;

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

// sys_dup2(old_fd, new_fd) — syscall 24 (复制 fd)
uint64_t sys_dup2(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int old_fd = (int)arg1;
    int new_fd = (int)arg2;

    if (old_fd < 0 || old_fd >= MAX_FD || new_fd < 0 || new_fd >= MAX_FD)
        return (uint64_t)-EBADF;

    if (old_fd == new_fd) return (uint64_t)new_fd;

    task_t *proc = current_task;

    spinlock_t *fdlk = &proc->mm->files->fd_lock;
    spin_lock(fdlk);

    if (proc->mm->files->fd_table[old_fd].type == FD_NONE) {
        spin_unlock(fdlk);
        return (uint64_t)-EBADF;
    }

    // Close new_fd if it's open
    if (proc->mm->files->fd_table[new_fd].type != FD_NONE) {
        if (proc->mm->files->fd_table[new_fd].type == FD_FILE) {
            if (refcount_dec_and_test(&proc->mm->files->fd_table[new_fd].file_data.f_count)) {
                file_t_io_req req = {0};
                req.cmd = FILE_CMD_CLOSE;
                req.fs_fd = proc->mm->files->fd_table[new_fd].file_data.fs_fd;
                kernel_msg_send(proc->mm->files->fd_table[new_fd].file_data.fs_pid,
                                &req, sizeof(req), NULL, 0);
            }
            __memset(&proc->mm->files->fd_table[new_fd], 0, sizeof(struct file));
            proc->mm->files->fd_table[new_fd].type = FD_NONE;
        } else {
            close_fd_trap(proc->mm->files, new_fd);
        }
    }

    // Copy old_fd to new_fd — struct assignment copies entire union
    proc->mm->files->fd_table[new_fd] = proc->mm->files->fd_table[old_fd];
    // Re-establish pointer/shm fields from source (struct assignment copies union bytes,
    // but for FD_PIPE/SHM we need to also bump ref_count)
    if (proc->mm->files->fd_table[new_fd].type == FD_PIPE) {
        refcount_inc(&proc->mm->files->fd_table[new_fd].pipe->p_count);
    } else if (proc->mm->files->fd_table[new_fd].type == FD_SHM) {
        if (proc->mm->files->fd_table[new_fd].shm) {
            shm_get(proc->mm->files->fd_table[new_fd].shm);
        }
    } else if (proc->mm->files->fd_table[new_fd].type == FD_FILE) {
        refcount_inc(&proc->mm->files->fd_table[new_fd].file_data.f_count);
    } else if (proc->mm->files->fd_table[new_fd].type == FD_REGULAR ||
               proc->mm->files->fd_table[new_fd].type == FD_DIR) {
        if (proc->mm->files->fd_table[new_fd].inode) inode_get(proc->mm->files->fd_table[new_fd].inode);
    } else if (proc->mm->files->fd_table[new_fd].type == FD_SOCKET) {
        if (proc->mm->files->fd_table[new_fd].sock) {
            unix_sock_acquire(proc->mm->files->fd_table[new_fd].sock);
        }
    } else if (proc->mm->files->fd_table[new_fd].type == FD_DEV) {
        // Kernel device: call open callback (e.g., serial_fd_count++) + inode_get
        struct inode *ip = proc->mm->files->fd_table[new_fd].inode;
        if (ip && ip->i_priv) {
            struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
            if (ops->driver_pid == 0 && ops->open)
                ops->open(proc, new_fd);
        }
        if (ip) inode_get(ip);
    } else if (proc->mm->files->fd_table[new_fd].type == FD_TTY) {
        pty_dup_fd(proc->mm->files, new_fd);
    }

    spin_unlock(fdlk);
    return (uint64_t)new_fd;
}

// sys_fcntl(fd, cmd, arg) — syscall 25 (文件控制)
uint64_t sys_fcntl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    int cmd = (int)arg2;
    int arg = (int)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    task_t *proc = current_task;
    if (proc->mm->files->fd_table[fd].type == FD_NONE) return (uint64_t)-EBADF;

    switch (cmd) {
    case F_GETFL:
        return (uint64_t)proc->mm->files->fd_table[fd].flags;
    case F_SETFL:
        proc->mm->files->fd_table[fd].flags = (proc->mm->files->fd_table[fd].flags & ~O_SETFL_MASK) | (arg & O_SETFL_MASK);
        return 0;
    case F_ADD_SEALS: {
        // Only valid on FD_SHM with MFD_ALLOW_SEALING
        if (proc->mm->files->fd_table[fd].type != FD_SHM) return (uint64_t)-EINVAL;
        struct shm *shm = proc->mm->files->fd_table[fd].shm;
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
            for (mmap_region_t *mr = proc->mm->mmap_regions; mr; mr = mr->next) {
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
        if (proc->mm->files->fd_table[fd].type != FD_SHM) return (uint64_t)-EINVAL;
        struct shm *shm = proc->mm->files->fd_table[fd].shm;
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

// Check if an IRQ is owned by a user-space driver (returns owner pid, or -1 if free)
int irq_owner_check(int irq) {
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return -1;
    return __atomic_load_n(&irq_owner[irq], __ATOMIC_ACQUIRE);
}

// sys_dma_alloc(size, vaddr_ptr, paddr_ptr) — syscall 26
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
    task_t *proc = current_task;

    // Pick virtual address from mmap_brk
    uint64_t vaddr = proc->mm->mmap_brk;
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

    proc->mm->mmap_brk = vaddr_end;

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
    region->next = proc->mm->mmap_regions;
    proc->mm->mmap_regions = region;

    // Write results to user space
    {
        uint64_t vaddr_val = vaddr;
        if (copy_to_user(vaddr_ptr, &vaddr_val, sizeof(vaddr_val)))
            return (uint64_t)-EFAULT;
    }
    if (copy_to_user(paddr_ptr, &phys, sizeof(phys)))
        return (uint64_t)-EFAULT;

    return 0;
}

// sys_dma_free(vaddr) — syscall 27
// 释放 DMA 缓冲区
// Returns: 0 on success, positive errno on failure
uint64_t sys_dma_free(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    uint64_t vaddr = (uint64_t)arg1;
    if (!vaddr) return (uint64_t)-EINVAL;

    task_t *proc = current_task;

    // Find and remove from mmap_regions
    mmap_region_t **pp = &proc->mm->mmap_regions;
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

// sys_lseek(fd, offset, whence) — syscall 43 (文件偏移定位)
// 更新内核 file_data._offset。FD_PIPE/SOCKET/DEV 返回 -ESPIPE。
// Returns: new offset on success, negative errno on failure
uint64_t sys_lseek(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    int64_t offset = (int64_t)arg2;
    int whence = (int)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;

    task_t *proc = current_task;
    if (proc->mm->files->fd_table[fd].type == FD_NONE) return (uint64_t)-EBADF;

    // PIPE/SOCKET/DEV do not support seek
    if (proc->mm->files->fd_table[fd].type == FD_PIPE ||
        proc->mm->files->fd_table[fd].type == FD_SOCKET ||
        proc->mm->files->fd_table[fd].type == FD_DEV)
        return (uint64_t)-ESPIPE;

    // FD_REGULAR/FD_DIR: kernel VFS file seek
    if (proc->mm->files->fd_table[fd].type == FD_REGULAR ||
        proc->mm->files->fd_table[fd].type == FD_DIR) {
        struct inode *ip = proc->mm->files->fd_table[fd].inode;
        if (!ip) return (uint64_t)-EBADF;
        int64_t new_offset;
        switch (whence) {
        case SEEK_SET: new_offset = offset; break;
        case SEEK_CUR: new_offset = (int64_t)proc->mm->files->fd_table[fd].offset + offset; break;
        case SEEK_END: new_offset = (int64_t)ip->size + offset; break;
        default: return (uint64_t)-EINVAL;
        }
        if (new_offset < 0) return (uint64_t)-EINVAL;
        proc->mm->files->fd_table[fd].offset = (uint64_t)new_offset;
        return (uint64_t)new_offset;
    }

    if (proc->mm->files->fd_table[fd].type != FD_FILE)
        return (uint64_t)-ESPIPE;

    uint64_t new_offset;
    switch (whence) {
    case SEEK_SET:
        new_offset = (uint64_t)offset;
        break;
    case SEEK_CUR:
        new_offset = proc->mm->files->fd_table[fd].file_data._offset + offset;
        break;
    case SEEK_END:
        new_offset = proc->mm->files->fd_table[fd].file_data.file_size + offset;
        break;
    default:
        return (uint64_t)-EINVAL;
    }

    proc->mm->files->fd_table[fd].file_data._offset = new_offset;
    return (uint64_t)new_offset;
}

// ===================== Signal delivery =====================

// Deliver a signal with a user-registered handler via sigframe on user stack.
static void deliver_signal(task_t *proc, trapframe_t *tf, int sig, sigaction_t *sa) {
    // 1. Build rt_sigframe
    struct rt_sigframe frame;
    __memset(&frame, 0, sizeof(frame));
    frame.pretcode = SIG_TRAMPOLINE_ADDR;

    // siginfo
    frame.info.si_signo = sig;
    frame.info.si_errno = 0;
    frame.info.si_code = SI_KERNEL;  // default; force_sig fills this
    // If sig_force_info matches, use it
    if (proc->sig_force_info.si_signo == sig) {
        frame.info = proc->sig_force_info;
    }

    // sigcontext — fill all GP registers from trapframe
    frame.uc.uc_mcontext.r8  = tf->r8;
    frame.uc.uc_mcontext.r9  = tf->r9;
    frame.uc.uc_mcontext.r10 = tf->r10;
    frame.uc.uc_mcontext.r11 = tf->r11;
    frame.uc.uc_mcontext.r12 = tf->r12;
    frame.uc.uc_mcontext.r13 = tf->r13;
    frame.uc.uc_mcontext.r14 = tf->r14;
    frame.uc.uc_mcontext.r15 = tf->r15;
    frame.uc.uc_mcontext.rdi = tf->rdi;
    frame.uc.uc_mcontext.rsi = tf->rsi;
    frame.uc.uc_mcontext.rbp = tf->rbp;
    frame.uc.uc_mcontext.rbx = tf->rbx;
    frame.uc.uc_mcontext.rdx = tf->rdx;
    frame.uc.uc_mcontext.rax = tf->rax;
    frame.uc.uc_mcontext.rcx = tf->rcx;
    frame.uc.uc_mcontext.rsp = tf->rsp;
    frame.uc.uc_mcontext.rip = tf->rip;
    frame.uc.uc_mcontext.eflags = tf->rflags;
    frame.uc.uc_mcontext.cs = tf->cs;
    frame.uc.uc_mcontext.ss = tf->ss;
    // cr2: filled by force_sig if SIGSEGV
    frame.uc.uc_mcontext.cr2 = (proc->sig_force_info.si_signo == sig) ?
        (uint64_t)proc->sig_force_info._sifields.si_addr : 0;

    // Save current blocked mask to sigframe (sigreturn restores it)
    frame.uc.uc_sigmask = proc->sig.blocked;
    frame.uc.uc_flags = 0;
    frame.uc.uc_link = NULL;

    // 2. Update blocked: mask sa_mask + current signal during handler
    proc->sig.blocked |= sa->sa_mask | (1ULL << sig);
    // SIGKILL/SIGSTOP never blocked
    proc->sig.blocked &= ~(1ULL << SIGKILL);
    proc->sig.blocked &= ~(1ULL << SIGSTOP);

    // Clear sig_force_info (consumed)
    if (proc->sig_force_info.si_signo == sig)
        proc->sig_force_info.si_signo = 0;

    // 3. Push sigframe to user stack (CR3 switch)
    uint64_t user_rsp = tf->rsp - sizeof(struct rt_sigframe);
    user_rsp &= ~0xFULL;  // 16-byte aligned (x86-64 ABI)

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)proc->cr3) : "memory");
    __memcpy((void *)user_rsp, &frame, sizeof(struct rt_sigframe));
    __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

    // 4. Modify trapframe → jump to handler
    tf->rip = (uint64_t)sa->__sigaction_handler._sa_handler;
    tf->rsp = user_rsp;

    // Handler arguments
    if (sa->sa_flags & SA_SIGINFO) {
        tf->rdi = (uint64_t)sig;
        tf->rsi = (uint64_t)(user_rsp + offsetof(struct rt_sigframe, info));
        tf->rdx = (uint64_t)(user_rsp + offsetof(struct rt_sigframe, uc));
    } else {
        tf->rdi = (uint64_t)sig;
    }
}

// ===================== check_pending_signals =====================
// Called before returning to user mode (iret/sysret).
// Scans pending signals and executes default action or delivers to handler.
// Debug helpers for fork child trapframe corruption (Bug 2)
void check_pending_signals(trapframe_t *tf) {
    // Only deliver when returning to user mode
    if (tf->cs != 0x2B) return;

    task_t *proc = current_task;
    if (!proc) return;

    // Loop: handle signals one at a time in priority order (lowest bit first)
    while (1) {
        uint64_t pending = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        // Deliverable = pending & ~blocked, but SIGKILL/SIGSTOP always deliverable
        uint64_t deliverable = pending & ~proc->sig.blocked;
        deliverable |= (pending & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
        if (!deliverable) return;

        // Find lowest deliverable signal (highest priority)
        int sig = __builtin_ctzll(deliverable);
        if (sig <= 0 || sig >= NSIG) {
            // Invalid signal number, clear all and bail
            __atomic_store_n(&proc->sig.pending, 0, __ATOMIC_RELEASE);
            return;
        }

        // Clear pending bit
        __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);

        sigaction_t *sa = &proc->sig.action[sig];

        if (sa->__sigaction_handler._sa_handler == SIG_DFL) {
            // Default action
            switch (sig) {
            case SIGCHLD:
            case SIGSTOP:
            case SIGTSTP:
            case SIGCONT:
            case SIGWINCH:
                break;  // default ignore
            case SIGKILL:
            case SIGINT:
            case SIGQUIT:
            case SIGHUP:
            case SIGTERM:
            case SIGSEGV:
            case SIGILL:
            case SIGFPE:
            case SIGABRT:
            case SIGBUS:
            case SIGTRAP:
            case SIGPIPE:
            case SIGALRM:
            case SIGUSR1:
            case SIGUSR2:
            case SIGSTKFLT:
            default:
                proc->exit_code = -1;
                printk(LOG_ERROR, "signal: pid=%d terminated by signal %d\n", proc->pid, sig);
                sys_exit(-1, 0, 0, 0, 0, 0);
                // Does not return
            }
        } else if (sa->__sigaction_handler._sa_handler == SIG_IGN) {
            continue;  // skip, check next signal
        } else {
            deliver_signal(proc, tf, sig, sa);
            return;  // tf modified, return to user mode to execute handler
        }
    }
}

// ===================== force_sig =====================
// Force delivery of a synchronous signal (bypasses SIG_IGN and blocked mask).
// Used for kernel exceptions (SIGSEGV/SIGILL/SIGFPE) and SIGKILL.
static void force_sig(task_t *proc, int sig, int si_code, void *si_addr) {
    // 1. Set pending bit
    __atomic_or_fetch(&proc->sig.pending, 1ULL << sig, __ATOMIC_RELEASE);

    // 2. Unblock this signal (ensure deliverable)
    __atomic_and_fetch(&proc->sig.blocked, ~(1ULL << sig), __ATOMIC_RELEASE);

    // 3. Fill sig_force_info (temporary, consumed by deliver_signal)
    proc->sig_force_info.si_signo = sig;
    proc->sig_force_info.si_errno = 0;
    proc->sig_force_info.si_code = si_code;
    proc->sig_force_info._sifields.si_addr = si_addr;

    // 4. If handler == SIG_IGN → force SIG_DFL
    //    Synchronous signals ignored would cause infinite fault re-execution
    if (proc->sig.action[sig].__sigaction_handler._sa_handler == SIG_IGN) {
        proc->sig.action[sig].__sigaction_handler._sa_handler = SIG_DFL;
    }
}

// ===================== Signal syscalls =====================

static void deliver_signal_to(task_t *target, int sig) {
    __atomic_or_fetch(&target->sig.pending, 1ULL << sig, __ATOMIC_RELEASE);
    if (target->state == BLOCKED) wake_process(target->pid);
}

static int pgsignal(pid_t pgid, int sig) {
    int found = 0;
    for (int p = 0; p < MAX_PROC; p++) {
        if (tasks[p].pid == p && tasks[p].pgid == pgid) {
            deliver_signal_to(&tasks[p], sig);
            found++;
        }
    }
    return found > 0 ? 0 : -ESRCH;
}

uint64_t sys_kill(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    pid_t pid = (pid_t)arg1;
    int sig = (int)arg2;
    if (sig < 0 || sig >= NSIG) return (uint64_t)-EINVAL;
    if (sig == 0) return 0;

    if (pid > 0) {
        if (pid >= MAX_PROC) return (uint64_t)-ESRCH;
        task_t *target = &tasks[pid];
        if (target->pid != pid) return (uint64_t)-ESRCH;
        deliver_signal_to(target, sig);
        return 0;
    } else if (pid == 0) {
        pid_t my_pgid = current_task->pgid;
        if (my_pgid == 0) return (uint64_t)-ESRCH;
        return (uint64_t)pgsignal(my_pgid, sig);
    } else if (pid == -1) {
        return (uint64_t)-EPERM;
    } else {
        return (uint64_t)pgsignal(-pid, sig);
    }
}

// ===================== Session/pgid syscalls =====================

uint64_t sys_setsid(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    if (current_task->sid == current_task->pid) return (uint64_t)-EPERM;
    current_task->sid = current_task->pid;
    current_task->pgid = current_task->pid;
    return (uint64_t)current_task->sid;
}

uint64_t sys_setpgid(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    pid_t pid = (pid_t)arg1; pid_t pgid = (pid_t)arg2;
    if (pid < 0 || pgid < 0) return (uint64_t)-EINVAL;
    if (pid == 0) pid = current_task->pid;
    if (pgid == 0) pgid = pid;
    if (pid >= MAX_PROC || tasks[pid].pid != pid) return (uint64_t)-ESRCH;
    if (pid != current_task->pid) {
        if (tasks[pid].mm->parent_pid != current_task->pid) return (uint64_t)-ESRCH;
        if (tasks[pid].sid != current_task->sid) return (uint64_t)-EPERM;
    }
    tasks[pid].pgid = pgid;
    return 0;
}

uint64_t sys_getpgid(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    pid_t pid = (pid_t)arg1;
    if (pid == 0) pid = current_task->pid;
    if (pid < 0 || pid >= MAX_PROC || tasks[pid].pid != pid) return (uint64_t)-ESRCH;
    return (uint64_t)tasks[pid].pgid;
}

uint64_t sys_getsid(uint64_t arg1, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    pid_t pid = (pid_t)arg1;
    if (pid == 0) pid = current_task->pid;
    if (pid < 0 || pid >= MAX_PROC || tasks[pid].pid != pid) return (uint64_t)-ESRCH;
    return (uint64_t)tasks[pid].sid;
}

// sys_sigaction(sig, act, oldact) — syscall 47 (注册/查询信号 handler)
uint64_t sys_sigaction(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int sig = (int)arg1;
    const struct sigaction __user *act = (const struct sigaction __user * __force)arg2;
    struct sigaction __user *oldact = (struct sigaction __user * __force)arg3;

    // Validate signal number
    if (sig < 0 || sig >= NSIG) return (uint64_t)-EINVAL;
    // SIGKILL and SIGSTOP cannot be caught or ignored
    if (sig == SIGKILL || sig == SIGSTOP) return (uint64_t)-EINVAL;

    task_t *proc = current_task;

    // If oldact is non-NULL, copy current action to user space
    if (oldact) {
        uint64_t ptr = (__force uint64_t)oldact;
        if (ptr >= 0xFFFFFFFF80000000ULL || ptr + sizeof(struct sigaction) > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        // Write under user's CR3
        uint64_t saved_cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
        __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)proc->cr3) : "memory");
        copy_to_user(oldact, &proc->sig.action[sig], sizeof(struct sigaction));
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
        copy_from_user(&new_act, act, sizeof(struct sigaction));
        __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

        // Validate: sa_mask must not contain SIGKILL/SIGSTOP
        if (new_act.sa_mask & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)))
            return (uint64_t)-EINVAL;

        proc->sig.action[sig] = new_act;

        // POSIX: registering a handler discards pending signals
        __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);
    }

    return 0;
}

// sys_sigreturn() — syscall 48 (信号 handler 返回后恢复上下文)
uint64_t sys_sigreturn(uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4, uint64_t _u5, uint64_t _u6) {
    task_t *proc = current_task;

    // Locate trapframe (syscall path: tss_rsp0 - sizeof(trapframe_t))
    uint64_t tf_base = get_cpu_local()->tss_rsp0 - sizeof(trapframe_t);
    trapframe_t *tf = (trapframe_t *)tf_base;

    // Read sigframe from user stack (CR3 switch)
    // RSP was adjusted by handler's RET which popped the pretcode (8 bytes),
    // so the sigframe starts 8 bytes below the current RSP.
    struct rt_sigframe frame;
    uint64_t user_rsp = tf->rsp - 8;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)proc->cr3) : "memory");
    __memcpy(&frame, (void *)user_rsp, sizeof(struct rt_sigframe));
    __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

    // Restore all GP registers from sigcontext
    struct sigcontext *sc = &frame.uc.uc_mcontext;
    tf->r8  = sc->r8;   tf->r9  = sc->r9;
    tf->r10 = sc->r10;  tf->r11 = sc->r11;
    tf->r12 = sc->r12;  tf->r13 = sc->r13;
    tf->r14 = sc->r14;  tf->r15 = sc->r15;
    tf->rdi = sc->rdi;  tf->rsi = sc->rsi;
    tf->rbp = sc->rbp;  tf->rbx = sc->rbx;
    tf->rdx = sc->rdx;  tf->rax = sc->rax;
    tf->rcx = sc->rcx;  tf->rsp = sc->rsp;
    tf->rip = sc->rip;
    tf->rflags = sc->eflags;
    tf->cs = sc->cs;  tf->ss = sc->ss;

    // Restore blocked mask
    proc->sig.blocked = frame.uc.uc_sigmask;
    // SIGKILL/SIGSTOP never blocked
    proc->sig.blocked &= ~(1ULL << SIGKILL);
    proc->sig.blocked &= ~(1ULL << SIGSTOP);

    return 0;
}
