/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/syscall.c — BSD syscall implementations
// Extracted from kernel/trap.c (phase 3 step 3.3)

#include "kernel/bsd/syscall.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "boot/boot.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/evdev_broker.h"
#include "kernel/bsd/eventfd.h"
#include "kernel/bsd/eventpoll.h"
#include "kernel/bsd/fat32.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/futex.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/ipcfd.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/pty.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/signalfd.h"
#include "kernel/bsd/socket.h"
#include "kernel/bsd/timerfd.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/vfs.h"
#include "kernel/driver/ahci.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h" // copy_*/strncpy_from_user declarations
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"
#include "utils/macro.h"

#include <xos/confname.h> // _SC_* (shared with user-side sysconf)
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/ioctl.h>
#include <xos/mman.h>
#include <xos/page.h>
#include <xos/signal.h>
#include <xos/socket.h>
#include <xos/stat.h>
#include <xos/syscall.h>
#include <xos/syscall_nums.h>
#include <xos/time.h>
#include <xos/utsname.h>

/* DRM 主号（仅 stat 设备号用，与 virtio_gpu.c DRM_MAJOR 同值；226 是 DRM 语义、
 * 不属于 devtmpfs 通用层，故各 .c 顶部各自定义而非放公共头）。 */
#define DRM_MAJOR_FOR_STAT 226

// OS-unique mmap flags (not in uapi mman.h, collision-free in 1024+ namespace)
#define MAP_PHYSICAL 0x80000000
#define MAP_UC 0x08 /* Map as uncacheable (device MMIO) */

// ===================== File protocol for FD_FILE <-> fs_driver IPC
// =====================
#define FILE_CMD_READ 2
#define FILE_CMD_WRITE 3
#define FILE_CMD_CLOSE 4

typedef struct file_t_io_req {
  uint32_t cmd;
  char _path[256];
  uint32_t _flags;
  uint32_t fs_fd;
  uint64_t offset;
  uint32_t count;
  uint32_t _lba;
  uint32_t _readdir_offset;
  uint32_t _readdir_count;
} file_t_io_req;

typedef struct file_t_io_resp {
  int32_t status;
  uint32_t _fd;
  uint64_t file_size;
  uint32_t count;
  uint32_t _total;
} file_t_io_resp;

// ===================== BSD syscall: exit =====================
// Key safety: all proc/signal reads happen BEFORE setting ZOMBIE (stored in
// locals). After ZOMBIE, do_exit only uses locals + xtask array fields —
// never proc->proc or sig, which may be kfree'd by concurrent sched_task_reap
// on another CPU. do_exit does NOT do mm_put/files_put/signal_put —
// sched_task_reap/ proc_reap owns all resource freeing (original design; 3b
// will revisit).
// ===================== BSD syscall: exit =====================
// Key safety: all proc/signal reads happen BEFORE setting ZOMBIE (stored in
// locals). After ZOMBIE, do_exit only uses locals + xtask array fields —
// never proc->proc or sig, which may be kfree'd by concurrent sched_task_reap
// on another CPU. do_exit does NOT do mm_put/files_put/signal_put —
// sched_task_reap/ proc_reap owns all resource freeing (original design; 3b
// will revisit).
//
// D13: exit_code is stored encoded as a Linux wait status. This function
// receives an **already-encoded** exit_code (normal exit = (code & 0xff) << 8;
// death by signal = sig & 0x7f). The two entry points sys_exit and
// do_exit_with_code encode separately:
//   - sys_exit(code): user-space exit/_exit entry point, encodes
//     (code & 0xff) << 8.
//   - do_exit_with_code(encoded): internal entry point such as death by
//     signal, passes the already-encoded value directly (signal.c passes
//     sig & 0x7f to avoid sys_exit's code<<8 misplacing the signal number
//     into the exit status bits). The status the parent gets from waitpid
//     can be fed directly to the standard WIFEXITED/WEXITSTATUS macros
//     (user/include/sys/wait.h).
int64_t do_exit_with_code(int32_t encoded_exit_code) {
  xtask *proc = current_task;
  int32_t exit_code = encoded_exit_code;
  proc->exit_code = exit_code;       // xtask (UAF-safe for waitpid)
  proc->proc->exit_code = exit_code; // proc (legacy, waitpid now reads xtask)
  printk(LOG_INFO, "do_exit: pid=%d tid=%d exit_code=%d\n", proc->tgid,
         proc->pid, exit_code);

  // 2. CPU time accounting
  if (proc->last_sched != 0) {
    proc->cpu_time_ns += sched_clock() - proc->last_sched;
    proc->last_sched = 0;
  }

  // 3. Orphan adoption: reparent all children whose mm->parent_pid matches
  //    the dying process.  Update BOTH mm->parent_pid (used by waitpid/child
  //    scan) AND signal->parent_pid (used by do_exit's SIGCHLD notification and
  //    sys_getppid).  The old code only updated mm->parent_pid, leaving
  //    signal->parent_pid pointing at a dead/reused PID, causing SIGCHLD to hit
  //    the wrong task (guarded but lost) and getppid to return garbage for
  //    orphans.
  if (init_pid >= 0) {
    spin_lock(&tasks_lock);
    for (int i = 0; i < MAX_PROC; i++) {
      if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->mm &&
          tasks[i]->mm->parent_pid == proc->pid) {
        tasks[i]->mm->parent_pid = init_pid;
        if (tasks[i]->proc && tasks[i]->proc->signal)
          tasks[i]->proc->signal->parent_pid = init_pid;
      }
    }
    spin_unlock(&tasks_lock);
  }

  // 4. clear_tid_addr: write 0 + futex_wake (pthread_join relies on this
  //    wakeup) BEFORE ZOMBIE — proc is alive, no concurrent sched_task_reap
  //    possible.
  if (proc->proc->clear_tid_addr) {
    // Invariant: before clear, *clear_tid_addr must equal this thread's tid
    // (written by CLONE_CHILD_SETTID before the child is scheduled). If it is
    // 0, the parent thread had not yet let the kernel write tid after clone
    // returned when the child exited — this is the timing-race signature of
    // bug.md Bug 2. Panic on hit to avoid the silent deadlock where join
    // never sees 0. Pure check, does not change semantics.
    // __attribute__((unused)): in release builds ASSERT is a no-op and
    // cur_tid_val is unused; in debug builds ASSERT consumes it.
    pid_t cur_tid_val __attribute__((unused)) =
        *((pid_t *)(uintptr_t)proc->proc->clear_tid_addr);
    ASSERT(cur_tid_val == proc->pid);
    *((pid_t *)(uintptr_t)proc->proc->clear_tid_addr) = 0;
    sys_futex((int64_t)proc->proc->clear_tid_addr, (int64_t)FUTEX_WAKE, 1, 0, 0,
              0);
  }

  // 5. Thread-group bookkeeping BEFORE ZOMBIE (proc/signal alive).
  //    Read signal fields into locals — after ZOMBIE we must not touch sig.
  struct signal_struct *sig = proc->proc->signal;
  pid_t ppid = sig->parent_pid;
  atomic_dec(&sig->live_count);
  int notify_parent = atomic_dec_and_test(&sig->thread_count);

  // 6. Set ZOMBIE
  //    GATE: after this, sched_task_reap/proc_reap on another CPU may kfree
  //    proc and signal_put signal_struct. Do NOT dereference proc->proc or sig.
  int cpu = proc->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  proc->state = ZOMBIE;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

  // 7. Notify parent (last thread) — uses local ppid, not sig->parent_pid
  if (notify_parent) {
    if (ppid >= 0 && ppid < MAX_PROC && task_get(ppid)->pid == ppid) {
      xtask *parent = task_get(ppid);
      // parent->proc is NULL for non-POSIX tasks (idle processes, see
      // xtask.h "NULL = idle/task without POSIX semantics"). A child whose
      // sig->parent_pid resolves to such a slot (orphan whose parent died and
      // whose pid slot was reused, or a reparented task whose parent_pid
      // never got rewritten — only mm->parent_pid is reparented in step 3)
      // must not dereference it. Guard matches the pty SIGHUP path
      // (proc.c pty_close_file).
      if (parent->proc) {
        __atomic_or_fetch(&parent->proc->sig_pending, 1ULL << SIGCHLD,
                          __ATOMIC_RELEASE);
        int pcpu = parent->assigned_cpu;
        uint64_t pflags;
        spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
        if (parent->state == BLOCKED && parent->wait_event == WAIT_CHILD) {
          wake_from_wait(parent);
        }
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
      }
    }
  }

  // 8. Wake processes waiting on this thread's REQ/MSG reply — xtask fields
  //     only, no proc
  for (int i = 0; i < MAX_PROC; i++) {
    if (!tasks[i])
      continue;
    xtask *waiter = task_get(i);
    if (waiter->pid >= 0 && waiter->state == BLOCKED &&
        waiter->wait_event == WAIT_REQ_REPLY &&
        waiter->req_target_pid == proc->pid) {
      int wcpu = waiter->assigned_cpu;
      uint64_t wflags;
      spin_lock_irqsave(&cpu_locals[wcpu].scheduler_lock, &wflags);
      if (waiter->state == BLOCKED && waiter->wait_event == WAIT_REQ_REPLY) {
        waiter->req_result = ESRCH;
        wake_from_wait(waiter);
      }
      spin_unlock_irqrestore(&cpu_locals[wcpu].scheduler_lock, wflags);
    }
  }

  // 9. schedule() — never returns.
  //    do_exit does NOT do mm_put/files_put/signal_put —
  //    sched_task_reap/proc_reap owns all freeing. 3b will revisit do_exit
  //    ownership with proper proc lifetime (RCU or per-task reap lock).
  schedule();
  return 0;
}

// sys_exit: user-space exit/_exit syscall entry point. Encodes
// (code & 0xff) << 8 and passes it to do_exit_with_code. D13.
int64_t sys_exit(int64_t arg1, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4, int64_t unused5) {
  int32_t encoded = ((int32_t)arg1 & 0xff) << 8;
  return do_exit_with_code(encoded);
}

// ===================== BSD syscall: exit_group =====================
int64_t sys_exit_group(int64_t arg1, int64_t unused1, int64_t unused2,
                       int64_t unused3, int64_t unused4, int64_t unused5) {
  xtask *current = current_task;
  struct signal_struct *sig = current->proc->signal;
  int32_t status = (int32_t)arg1;

  // 1. Set group_exit flag
  spin_lock(&sig->sig_lock);
  sig->group_exit = 1;
  sig->group_exit_code = status;
  spin_unlock(&sig->sig_lock);

  // 2. Scan tasks[], wake BLOCKED threads with the same tgid and != current
  for (int i = 0; i < MAX_PROC; i++) {
    if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->tgid == current->tgid &&
        tasks[i]->pid != current->pid) {
      int tcpu = tasks[i]->assigned_cpu;
      uint64_t tflags;
      spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &tflags);
      if (tasks[i]->state == BLOCKED) {
        sched_timer_queue_cancel(tasks[i]);
        tasks[i]->state = READY;
        tasks[i]->wait_event = WAIT_NONE;
        tasks[i]->wait_timed_out = 0;
        run_queue_push(tcpu, tasks[i]);
      }
      spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, tflags);
    }
  }

  // 3. Current thread exits
  return sys_exit(status, 0, 0, 0, 0, 0);
}

// ===================== BSD syscall: waitpid =====================
// Mirror of user/include/sys/wait.h options. Only WNOHANG is honored today;
// WUNTRACED/WCONTINUED require stopped-state reporting (see
// doc/design/todo.md).
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4

int64_t sys_waitpid(int64_t arg1, int64_t arg2, int64_t options,
                    int64_t unused2, int64_t unused3, int64_t unused4) {
  pid_t pid = (pid_t)arg1;
  int32_t __user *exit_code_ptr = (int32_t __user * __force) arg2;
  int nohang = (int)options & WNOHANG;

  if (pid == -1) {
    while (1) {
      spin_lock(&tasks_lock);
      xtask *zombie = NULL;
      bool has_children = false;
      for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->mm &&
            tasks[i]->mm->parent_pid == current_task->pid) {
          has_children = true;
          if (tasks[i]->state == ZOMBIE) {
            zombie = tasks[i];
            break;
          }
        }
      }
      if (!has_children) {
        spin_unlock(&tasks_lock);
        return (int64_t)-ECHILD;
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
          continue;
        }
        pid_t zpid = zombie->pid;
        if (exit_code_ptr) {
          uint64_t ptr_val = (__force uint64_t)exit_code_ptr;
          if (ptr_val < KERNEL_VMA_BOUNDARY && ptr_val &&
              (ptr_val + sizeof(int32_t) - 1) < KERNEL_VMA_BOUNDARY)
            *(__force int32_t *)exit_code_ptr = zombie->exit_code;
        }
        sched_task_reap(zombie);
        return (int64_t)zpid;
      }
      spin_unlock(&tasks_lock);

      // WNOHANG: no zombie ready, don't block — return 0 immediately.
      if (nohang)
        return 0;

      int pcpu = current_task->assigned_cpu;
      uint64_t pflags;
      spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
      spin_lock(&tasks_lock);
      zombie = NULL;
      has_children = false;
      for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->mm &&
            tasks[i]->mm->parent_pid == current_task->pid) {
          has_children = true;
          if (tasks[i]->state == ZOMBIE) {
            zombie = tasks[i];
            break;
          }
        }
      }
      if (!has_children) {
        spin_unlock(&tasks_lock);
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
        return (int64_t)-ECHILD;
      }
      if (zombie) {
        spin_unlock(&tasks_lock);
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
        continue;
      }
      spin_unlock(&tasks_lock);
      current_task->wait_event = WAIT_CHILD;
      current_task->state = BLOCKED;
      spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
      schedule();
      {
        uint64_t pend =
            __atomic_load_n(&current_proc->sig_pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~current_proc->sig_blocked;
        deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
        deliv &= ~(1ULL << SIGCHLD);
        if (deliv)
          return (int64_t)-EINTR;
      }
    }
  }

  if (pid < 0 || pid >= MAX_PROC) {
    printk(LOG_WARN, "waitpid: pid=%d out of range\n", pid);
    return -EINVAL;
  }

  xtask *child = task_get(pid);

  spin_lock(&tasks_lock);
  if (child->pid != pid || !child->mm ||
      child->mm->parent_pid != current_task->pid) {
    printk(LOG_WARN,
           "waitpid: pid=%d validation fail: child_pid=%d mm=%p parent_pid=%d "
           "caller=%d\n",
           pid, child->pid, child->mm, child->mm ? child->mm->parent_pid : -1,
           current_task->pid);
    spin_unlock(&tasks_lock);
    return -ECHILD;
  }
  spin_unlock(&tasks_lock);

  while (1) {
    int cpu = child->assigned_cpu;
    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
    if (child->state == ZOMBIE) {
      child->state = REAPING;
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
      break;
    }
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

    // WNOHANG: child not a zombie yet, don't block — return 0.
    if (nohang)
      return 0;

    int pcpu = current_task->assigned_cpu;
    if (pcpu == cpu) {
      uint64_t pflags;
      spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
      if (child->state == ZOMBIE) {
        child->state = REAPING;
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
        break;
      }
      current_task->wait_event = WAIT_CHILD;
      current_task->state = BLOCKED;
      spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
    } else {
      uint64_t pflags, cflags;
      spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &cflags);
      if (child->state == ZOMBIE) {
        child->state = REAPING;
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, cflags);
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
        break;
      }
      current_task->wait_event = WAIT_CHILD;
      current_task->state = BLOCKED;
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, cflags);
      spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
    }
    schedule();

    {
      uint64_t pend =
          __atomic_load_n(&current_proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~current_proc->sig_blocked;
      deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
      deliv &= ~(1ULL << SIGCHLD);
      if (deliv) {
        printk(LOG_WARN, "waitpid: pid=%d EINTR pending=0x%lx\n", pid, pend);
        return (int64_t)-EINTR;
      }
    }

    spin_lock(&tasks_lock);
    if (child->pid != pid) {
      printk(LOG_WARN, "waitpid: pid=%d child reaped by someone else\n", pid);
      spin_unlock(&tasks_lock);
      return -ECHILD;
    }
    spin_unlock(&tasks_lock);
  }

  // exit_code lives in xtask (static array) — safe to read without proc
  // ref.
  if (exit_code_ptr) {
    uint64_t ptr_val = (__force uint64_t)exit_code_ptr;
    if (ptr_val >= KERNEL_VMA_BOUNDARY || !ptr_val ||
        (ptr_val + sizeof(int32_t) - 1) >= KERNEL_VMA_BOUNDARY) {
      printk(LOG_WARN, "waitpid: pid=%d bad exit_code_ptr=0x%lx\n", pid,
             ptr_val);
      return -EFAULT;
    }
    *(__force int32_t *)exit_code_ptr = child->exit_code;
  }
  sched_task_reap(child);
  return (int64_t)pid;
}

// 薄封装 §4.3:wait4(pid,wstatus,options,NULL) ≡ waitpid;不实现 rusage。
int64_t sys_wait4(int64_t pid, int64_t wstatus, int64_t options, int64_t rusage,
                  int64_t unused1, int64_t unused2) {
  if (rusage != 0)
    return -ENOSYS; // 不实现 rusage(Linux 允许 NULL)
  return sys_waitpid(pid, wstatus, options, 0, 0, 0);
}

// ===================== BSD syscall: mmap =====================
int64_t sys_mmap(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                 int64_t arg5, int64_t arg6) {
  (void)arg1;
  size_t size = (size_t)arg2;
  uint32_t prot = (uint32_t)arg3;
  int flags = (int)arg4;
  int fd = (int)arg5;
  uint64_t offset = arg6;

  if (size == 0 && ((flags & MAP_SHARED) == 0 || fd < 0))
    return -EINVAL;
  if (size > 128 * 1024 * 1024)
    return -EINVAL;

  xtask *proc = current_task;
  uint64_t mmap_flags;
  spin_lock_irqsave(&proc->mm->mmap_lock, &mmap_flags);
  printk(LOG_DEBUG, "sys_mmap: pid=%d size=%zu flags=%d fd=%d offset=%llu\n",
         proc->pid, size, flags, fd, (unsigned long long)offset);
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

  // MAP_SHARED + fd >= 0: SHM or DEV fd mapping
  if ((flags & MAP_SHARED) && fd >= 0) {
    if (fd >= MAX_FD) {
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -EBADF;
    }

    rcu_read_lock();
    struct file *f = fd_lookup(proc->proc->files, fd);
    if (f)
      file_get(f);
    rcu_read_unlock();
    if (f && f->f_op && f->f_op->mmap) {
      uint64_t ret = f->f_op->mmap(proc, f, size);
      // -ENOSYS: f_op doesn't handle mmap → fall through to FD_DEV SHM path.
      // -EPERM: hard rejection (e.g. a char f_op blocks mmap for non-driver
      //         consumers).
      // Other negatives: hard error.
      if (ret != (uint64_t)-ENOSYS) {
        file_put(f);
        spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
        return ret;
      }
    }
    if (f && f->type == FD_DEV) {
      struct inode *ip = f->inode;
      if (ip && ip->i_priv) {
        struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
        if (ops->driver_pid == 0 && ops->mmap) {
          uint64_t ret = ops->mmap(proc, size, offset);
          file_put(f);
          spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
          return ret;
        }
        if (ip->shm) {
          struct shm *target_shm = ip->shm;
          shm_get(target_shm);

          size_t npages = target_shm->npages;
          size_t list_pages =
              target_shm->page_list ? (size_t)target_shm->num_pages : 0;
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
            if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys,
                                      pte_flags)) {
              for (size_t j = 0; j < i; j++)
                unmap_user_pages(pml4, vaddr + j * PAGE_SIZE,
                                 vaddr + (j + 1) * PAGE_SIZE, 1);
              shm_put(target_shm);
              file_put(f);
              spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
              return -ENOMEM;
            }
          }

          mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
          if (!region) {
            for (size_t i = 0; i < total_pages; i++)
              unmap_user_pages(pml4, vaddr + i * PAGE_SIZE,
                               vaddr + (i + 1) * PAGE_SIZE, 1);
            shm_put(target_shm);
            file_put(f);
            spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
            return -ENOMEM;
          }

          region->vaddr = vaddr;
          region->size = size;
          region->phys = 0;
          region->shm_obj = target_shm;
          region->next = proc->mm->mmap_regions;
          proc->mm->mmap_regions = region;
          proc->mm->mmap_brk = vaddr + size;

          file_put(f);
          spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
          return vaddr;
        }
      }
      file_put(f);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -ENODEV;
    }

    if (!f || f->type != FD_SHM) {
      if (f)
        file_put(f);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -EINVAL;
    }
    struct shm *shm = f->shm;
    if (!shm) {
      file_put(f);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -EBADF;
    }

    size_t npages = shm->npages;
    size_t list_pages = shm->page_list ? (size_t)shm->num_pages : 0;
    size_t total_pages = npages + list_pages;
    size = total_pages * PAGE_SIZE;

    uint64_t vaddr = proc->mm->mmap_brk;
    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

    for (size_t i = 0; i < total_pages; i++) {
      uint64_t page_phys;
      if (i < npages) {
        page_phys = shm->phys + i * PAGE_SIZE;
      } else {
        page_phys = shm->page_list[i - npages];
      }
      if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys,
                                pte_flags)) {
        for (size_t j = 0; j < i; j++)
          unmap_user_pages(pml4, vaddr + j * PAGE_SIZE,
                           vaddr + (j + 1) * PAGE_SIZE, 1);
        spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
        return -ENOMEM;
      }
    }

    mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!region) {
      for (size_t i = 0; i < total_pages; i++)
        unmap_user_pages(pml4, vaddr + i * PAGE_SIZE,
                         vaddr + (i + 1) * PAGE_SIZE, 1);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -ENOMEM;
    }

    region->vaddr = vaddr;
    region->size = size;
    region->phys = 0;
    region->shm_obj = shm_get(shm);
    region->next = proc->mm->mmap_regions;
    proc->mm->mmap_regions = region;
    proc->mm->mmap_brk = vaddr + size;

    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return vaddr;
  }

  // MAP_PHYSICAL
  if (flags & MAP_PHYSICAL) {
    uint64_t vaddr = proc->mm->mmap_phys_brk;
    uint64_t phys_start = ALIGN_DOWN(offset, PAGE_SIZE);
    uint64_t phys_end = ALIGN_UP(offset + size, PAGE_SIZE);
    size_t npages = (phys_end - phys_start) / PAGE_SIZE;

    uint64_t max_phys_addr = (uint64_t)total_page_frames * PAGE_SIZE;
    uint64_t kernel_phys_start = KERNEL_LOAD_ADDR;
    uint64_t kernel_phys_end = bump_end_phys();

    if (phys_start >= kernel_phys_start && phys_start < kernel_phys_end) {
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -EINVAL;
    }

    if (flags & MAP_UC) {
      if (phys_start >= 0x100000000ULL) {
        spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
        return -EINVAL;
      }
    } else {
      if (phys_start >= max_phys_addr) {
        spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
        return -EINVAL;
      }
    }

    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;
    if (flags & MAP_UC) {
      pte_flags |= PTE_PCD | PTE_PWT;
    }

    for (size_t i = 0; i < npages; i++) {
      if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                phys_start + i * PAGE_SIZE, pte_flags)) {
        printk(LOG_ERROR, "mmap PHYSICAL: map failed at i=%lu\n",
               (unsigned long)i);
        for (size_t j = 0; j < i; j++)
          unmap_user_pages(pml4, vaddr + j * PAGE_SIZE,
                           vaddr + (j + 1) * PAGE_SIZE, 1);
        spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
        return -ENOMEM;
      }
    }

    mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!region) {
      for (size_t i = 0; i < npages; i++)
        unmap_user_pages(pml4, vaddr + i * PAGE_SIZE,
                         vaddr + (i + 1) * PAGE_SIZE, 1);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -ENOMEM;
    }

    region->vaddr = vaddr;
    region->size = npages * PAGE_SIZE;
    region->phys = phys_start;
    region->shm_obj = NULL;
    region->next = proc->mm->mmap_regions;
    proc->mm->mmap_regions = region;
    proc->mm->mmap_phys_brk = vaddr + npages * PAGE_SIZE;

    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return vaddr;
  }

  // Anonymous private mapping
  size = ALIGN_UP(size, PAGE_SIZE);
  uint64_t vaddr = proc->mm->mmap_brk;
  // prot=0 (PROT_NONE) → guard page: map page but NOT present.
  // Access triggers #PF (desired for stack-overflow detection).
  uint64_t pte_flags = PTE_USER;
  if (prot == 0) {
    // PROT_NONE: keep physical page allocated but present=0 + PTE_PROTNONE.
    // pte_present() still recognizes it (reclaim/fork/mprotect), but hardware
    // treats it as not-present → access triggers #PF → SEGV_ACCERR (Linux).
    pte_flags |= PTE_PROTNONE | PTE_NX;
  } else {
    pte_flags |= PTE_PRESENT;
    if (prot & PROT_WRITE)
      pte_flags |= PTE_RW;
    if (!(prot & PROT_EXEC))
      pte_flags |= PTE_NX;
  }

  size_t npages = size / PAGE_SIZE;
  uint64_t *phys_pages = (uint64_t *)kmalloc(npages * sizeof(uint64_t));
  if (!phys_pages) {
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -ENOMEM;
  }

  size_t mapped = 0;
  for (size_t i = 0; i < npages; i++) {
    struct page *page = bfc_alloc_page(1);
    if (!page) {
      for (size_t j = 0; j < mapped; j++) {
        uint64_t va = vaddr + j * PAGE_SIZE;
        unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
      }
      kfree(phys_pages);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -ENOMEM;
    }
    phys_pages[i] = (__force uint64_t)page_to_phys(page);
    // Zero the page before mapping to userspace (page may contain stale data
    // from previous user)
    __memset((__force void *)phys_to_virt((__force phys_addr_t)phys_pages[i]),
             0, PAGE_SIZE);
    if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, phys_pages[i],
                              pte_flags)) {
      bfc_free_page(&bfc_frames[PHY_TO_PAGE(phys_pages[i])], 1);
      for (size_t j = 0; j < mapped; j++) {
        uint64_t va = vaddr + j * PAGE_SIZE;
        unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
      }
      kfree(phys_pages);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -ENOMEM;
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
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -ENOMEM;
  }

  region->vaddr = vaddr;
  region->size = size;
  region->phys = 0;
  region->shm_obj = NULL;
  region->prot = prot;
  region->next = proc->mm->mmap_regions;
  proc->mm->mmap_regions = region;
  proc->mm->mmap_brk = vaddr + size;

  kfree(phys_pages);
  spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
  return vaddr;
}

// ===================== BSD syscall: munmap =====================
int64_t sys_munmap(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  uint64_t addr = arg1;
  size_t size = (size_t)arg2;

  if (size == 0)
    return (int64_t)-EINVAL;

  xtask *proc = current_task;
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

  mmap_region **pp = &proc->mm->mmap_regions;
  while (*pp) {
    if ((*pp)->vaddr == addr) {
      mmap_region *region = *pp;
      size = region->size;

      size_t npages = size / PAGE_SIZE;
      if (region->shm_obj || region->phys) {
        for (size_t i = 0; i < npages; i++) {
          uint64_t va = addr + i * PAGE_SIZE;
          uint64_t *pdpt = ensure_pd(pml4, va);
          if (!pdpt)
            continue;
          uint64_t *pd = ensure_pt_in_pd(pdpt, va, 2);
          if (!pd)
            continue;
          uint64_t *pt = ensure_pt_in_pd(pd, va, 1);
          if (!pt)
            continue;
          uint64_t pt_idx = (va >> 12) & 0x1FF;
          pt[pt_idx] = 0;
        }
      } else {
        for (size_t i = 0; i < npages; i++) {
          uint64_t va = addr + i * PAGE_SIZE;
          unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
        }
      }

      if (region->shm_obj) {
        shm_put(region->shm_obj);
      }

      *pp = region->next;
      kfree(region);
      return 0;
    }
    pp = &(*pp)->next;
  }

  return (int64_t)-EINVAL;
}

// ===================== BSD syscall: mprotect =====================
// Change protection of existing user mappings page-by-page. Aligns with Linux:
// PROT_NONE clears PTE_PRESENT and sets PTE_PROTNONE (hardware not-present, but
// pte_present() recognizes it). partial-unmapped → -ENOMEM, already-changed
// pages kept (Linux semantics). Does not split huge pages (→ -EINVAL).
int64_t sys_mprotect(int64_t arg1, int64_t arg2, int64_t prot_arg,
                     int64_t unused1, int64_t unused2, int64_t unused3) {
  uint64_t addr = arg1;
  size_t size = (size_t)arg2;
  int prot = (int)prot_arg;

  if (size == 0)
    return 0; // Linux: no-op
  if (addr & (PAGE_SIZE - 1))
    return (int64_t)-EINVAL;
  if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return (int64_t)-EINVAL;
  if (addr >= 0x800000000000ULL)
    return (int64_t)-EINVAL;

  size = ALIGN_UP(size, PAGE_SIZE);

  xtask *proc = current_task;
  spinlock *lk = &proc->mm->mmap_lock;
  uint64_t flags;
  spin_lock_irqsave(lk, &flags);

  // New leaf flags (PTE_USER always; physical address preserved per-page).
  uint64_t base = PTE_USER;
  if (prot == 0) {
    // PROT_NONE: present=0 + PROTNONE + NX
    base |= PTE_PROTNONE | PTE_NX;
  } else {
    base |= PTE_PRESENT;
    if (prot & PROT_WRITE)
      base |= PTE_RW;
    if (!(prot & PROT_EXEC))
      base |= PTE_NX;
  }

  size_t npages = size / PAGE_SIZE;
  for (size_t i = 0; i < npages; i++) {
    uint64_t va = addr + i * PAGE_SIZE;
    uint64_t *pte = lookup_pte(proc->mm->cr3, va);
    if (!pte) {
      // Page genuinely unmapped → -ENOMEM, already-changed pages kept (Linux).
      spin_unlock_irqrestore(lk, flags);
      return (int64_t)-ENOMEM;
    }
    if (*pte & PTE_PS) {
      // Refuse to split huge pages (no anonymous huge-page mprotect users).
      spin_unlock_irqrestore(lk, flags);
      return (int64_t)-EINVAL;
    }
    uint64_t phys = *pte & PTE_PHYS_MASK;
    // Preserve USER + physical address; clear COW/PROTNONE/old RW/NX, reapply.
    *pte = phys | base;
    invlpg(va); // stale TLB would defeat RW→RX / R→PROTNONE transitions
  }

  // Sync mmap_region->prot so fork COW honors the new protection.
  for (mmap_region *mr = proc->mm->mmap_regions; mr; mr = mr->next) {
    if (mr->vaddr == addr) {
      mr->prot = (uint32_t)prot;
      break;
    }
  }

  spin_unlock_irqrestore(lk, flags);
  return 0;
}

// ===================== BSD syscall: sysconf =====================
// Backs the libc sysconf() for values that are genuinely runtime-variable.
// Static/architecture-fixed values (PAGESIZE, CLK_TCK, OPEN_MAX, …) stay in
// the user-side switch; this syscall only carries the dynamic ones so they
// track real boot state rather than hardcoded guesses.
//   _SC_NPROCESSORS_ONLN/CONF → ncpu (set by smp_boot_aps to g_madt.ncpus)
//   _SC_PHYS_PAGES            → total_page_frames (E820 max phys, init_mem)
//   _SC_AVPHYS_PAGES          → free pages in bfc_free_list (cont_page_num sum)
int64_t sys_sysconf(int64_t name, int64_t unused1, int64_t unused2,
                    int64_t unused3, int64_t unused4, int64_t unused5) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;

  switch ((int)name) {
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
    return (int64_t)ncpu;
  case _SC_PHYS_PAGES:
    return (int64_t)total_page_frames;
  case _SC_AVPHYS_PAGES: {
    size_t free_pages = 0;
    for (struct page *p = bfc_free_list; p; p = p->bfc.next)
      free_pages += p->bfc.cont_page_num;
    return (int64_t)free_pages;
  }
  default:
    return -1; // POSIX: unsupported → -1, errno unchanged
  }
}

// pipe wq 回调：__wake_up(p->wq) → 唤醒挂在 p->wq 的阻塞 reader/writer。
// 不查 wait_event（队列身份制：在 p->wq 上即唤醒）。类比 ring.c ring_wake_cb。
static void pipe_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(target);
}

// ===================== BSD syscall: pipe =====================
int64_t sys_pipe(int64_t arg1, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4, int64_t unused5) {
  int __user *fd_ptr = (int __user *__force)arg1;

  uint64_t ptr = (__force uint64_t)fd_ptr;
  if (!ptr || ptr >= KERNEL_VMA_BOUNDARY ||
      ptr + 2 * sizeof(int) > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;

  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int read_fd = alloc_fd(proc->proc->files, 3);
  int write_fd =
      (read_fd >= 0) ? alloc_fd(proc->proc->files, read_fd + 1) : -EMFILE;
  if (read_fd < 0 || write_fd < 0) {
    spin_unlock(fdlk);
    return (int64_t)-EMFILE;
  }

  uint8_t *buf = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
  if (!buf) {
    fd_uninstall(proc->proc->files, read_fd);
    fd_uninstall(proc->proc->files, write_fd);
    spin_unlock(fdlk);
    return (int64_t)-ENOMEM;
  }

  struct pipe *p = (struct pipe *)kmalloc(sizeof(struct pipe));
  if (!p) {
    fd_uninstall(proc->proc->files, read_fd);
    fd_uninstall(proc->proc->files, write_fd);
    kfree(buf);
    spin_unlock(fdlk);
    return (int64_t)-ENOMEM;
  }

  for (int i = 0; i < PIPE_BUF_SIZE; i++)
    buf[i] = 0;

  p->buf = buf;
  p->head = 0;
  p->tail = 0;
  p->wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!p->wq) {
    fd_uninstall(proc->proc->files, read_fd);
    fd_uninstall(proc->proc->files, write_fd);
    kfree(p);
    kfree(buf);
    spin_unlock(fdlk);
    return (int64_t)-ENOMEM;
  }
  init_wait_queue_head(p->wq);
  refcount_set(&p->p_count, 2);

  struct file *fr = (struct file *)kmalloc(sizeof(struct file));
  if (!fr) {
    fd_uninstall(proc->proc->files, read_fd);
    fd_uninstall(proc->proc->files, write_fd);
    kfree(p->wq);
    kfree(p);
    kfree(buf);
    spin_unlock(fdlk);
    return (int64_t)-ENOMEM;
  }
  __memset(fr, 0, sizeof(*fr));
  refcount_set(&fr->f_count, 1);
  fr->type = FD_PIPE;
  fr->flags = O_RDONLY;
  fr->pipe = p;
  fd_install(proc->proc->files, read_fd, fr);

  struct file *fw = (struct file *)kmalloc(sizeof(struct file));
  if (!fw) {
    fd_uninstall(proc->proc->files, write_fd);
    file_put(fr);
    fd_uninstall(proc->proc->files, read_fd);
    kfree(p->wq);
    kfree(p);
    kfree(buf);
    spin_unlock(fdlk);
    return (int64_t)-ENOMEM;
  }
  __memset(fw, 0, sizeof(*fw));
  refcount_set(&fw->f_count, 1);
  fw->type = FD_PIPE;
  fw->flags = O_WRONLY;
  fw->pipe = p;
  fd_install(proc->proc->files, write_fd, fw);

  int fd_pair[2] = {read_fd, write_fd};
  if (copy_to_user(fd_ptr, fd_pair, sizeof(fd_pair))) {
    struct file *f_r = fd_uninstall(proc->proc->files, read_fd);
    struct file *f_w = fd_uninstall(proc->proc->files, write_fd);
    spin_unlock(fdlk);
    synchronize_rcu();
    file_put(f_r);
    file_put(f_w);
    return (int64_t)-EFAULT;
  }

  spin_unlock(fdlk);
  return 0;
}

// ===================== BSD syscall: write =====================
int64_t sys_write(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  const char __user *buf = (const char __user *__force)arg2;
  size_t len = (size_t)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)-EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;

  if (f->f_op) {
    if (f->f_op->write)
      return f->f_op->write(proc, f, buf, len);
    ret = -EINVAL;
    goto out;
  }

  // FD_REGULAR: kernel FAT32 via page cache
  if (f->type == FD_REGULAR) {
    if (!(f->flags & (O_WRONLY | O_RDWR))) {
      ret = -EBADF;
      goto out;
    }
    struct inode *ip = f->inode;
    if (!ip) {
      ret = -EBADF;
      goto out;
    }

    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }

    uint64_t offset = f->offset;
    int written = fat32_write(ip, offset, (const void __force *)buf, len);
    if (written < 0) {
      ret = (int64_t)written;
      goto out;
    }
    f->offset = offset + written;
    ret = (int64_t)written;
    goto out;
  }

  // FD_FILE: proxy to fs_driver
  if (f->type == FD_FILE) {
    if (!(f->flags & (O_WRONLY | O_RDWR))) {
      ret = -EINVAL;
      goto out;
    }

    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }

    size_t max_data = 65536 - sizeof(file_t_io_req);
    if (len > max_data)
      len = max_data;
    if (len == 0) {
      ret = 0;
      goto out;
    }

    size_t msg_len = sizeof(file_t_io_req) + len;
    uint8_t *msg_buf = (uint8_t *)kmalloc(msg_len);
    if (!msg_buf) {
      ret = -ENOMEM;
      goto out;
    }

    file_t_io_req *req = (file_t_io_req *)msg_buf;
    req->cmd = FILE_CMD_WRITE;
    req->fs_fd = f->file_data.fs_fd;
    req->offset = f->file_data._offset;
    req->count = (uint32_t)len;
    if (copy_from_user(msg_buf + sizeof(file_t_io_req), buf, len)) {
      kfree(msg_buf);
      ret = (int64_t)-EFAULT;
      goto out;
    }

    file_t_io_resp resp;
    int64_t msg_ret =
        sys_msg_to(f->file_data.fs_pid, msg_buf, msg_len, &resp, sizeof(resp));
    kfree(msg_buf);

    if (msg_ret < 0) {
      ret = -msg_ret;
      goto out;
    }

    if (resp.status != 0) {
      ret = -(int64_t)resp.status;
      goto out;
    }

    size_t written = resp.count;
    f->file_data._offset += written;
    if (f->file_data.file_size < f->file_data._offset)
      f->file_data.file_size = f->file_data._offset;
    ret = (int64_t)written;
    goto out;
  }

  // FD_SOCKET
  if (f->type == FD_SOCKET) {
    if (!(f->flags & (O_WRONLY | O_RDWR))) {
      ret = -EINVAL;
      goto out;
    }
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct unix_sock *sock = f->sock;
    if (!sock) {
      ret = -EBADF;
      goto out;
    }
    ret = unix_sock_write(sock, (const void __force *)buf, len);
    goto out;
  }

  // FD_NETLINK: write → netlink broadcast
  if (f->type == FD_NETLINK) {
    if (!(f->flags & (O_WRONLY | O_RDWR))) {
      ret = -EINVAL;
      goto out;
    }
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct netlink_sock *nlsock = f->nlsock;
    if (!nlsock) {
      ret = -EBADF;
      goto out;
    }
    struct iovec iov;
    iov.iov_base = (void *)(__force uint64_t)buf;
    iov.iov_len = len;
    ret = netlink_sock_sendmsg(nlsock, &iov, 1,
                               (f->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    goto out;
  }

  // FD_DEV: write via dev_ops callback
  if (f->type == FD_DEV) {
    if (!(f->flags & (O_WRONLY | O_RDWR))) {
      ret = -EINVAL;
      goto out;
    }
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    struct inode *ip = f->inode;
    if (!ip || !ip->i_priv) {
      ret = -ENODEV;
      goto out;
    }
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    if (ops->driver_pid == 0 && ops->write) {
      ret = (int64_t)ops->write(proc, fd, (const void __force *)buf, len);
      goto out;
    }
    ret = -ENOSYS;
    goto out;
  }

  // FD_TTY: PTY write
  if (f->type == FD_TTY) {
    if (!(f->flags & (O_WRONLY | O_RDWR))) {
      ret = -EINVAL;
      goto out;
    }
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct pty *pty = f->pty;
    if (!pty) {
      ret = -EBADF;
      goto out;
    }
    int is_master = pty_is_master_inode(f->inode);
    if (is_master)
      ret = pty_master_write(pty, proc, (const void __force *)buf, len);
    else
      ret = pty_slave_write(pty, proc, (const void __force *)buf, len);
    goto out;
  }

  // FD_EVENTFD: 8-byte counter write
  if (f->type == FD_EVENTFD) {
    ret = eventfd_do_write(f, buf, len);
    goto out;
  }

  // FD_PIPE — explicit type dispatch so any unhandled fd type returns
  // -EINVAL instead of falling through into the pipe path (which once
  // dereferenced NULL f->pipe on a directory fd and page-faulted).
  if (f->type != FD_PIPE) {
    ret = -EINVAL;
    goto out;
  }
  if (!(f->flags & (O_WRONLY | O_RDWR))) {
    ret = -EINVAL;
    goto out;
  }

  if (!buf) {
    ret = -EFAULT;
    goto out;
  }
  uint64_t ptr_start = (__force uint64_t)buf;
  uint64_t ptr_end = ptr_start + len;
  if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
      ptr_end > KERNEL_VMA_BOUNDARY) {
    ret = -EFAULT;
    goto out;
  }

  struct pipe *p = f->pipe;
  if (!p) {
    ret = -EBADF;
    goto out;
  }
  size_t written = 0;

  while (written < len) {
    if (refcount_read(&p->p_count) <= 1) {
      if (written > 0)
        break;
      ret = -EPIPE;
      goto out;
    }
    if ((p->head + 1) % PIPE_BUF_SIZE == p->tail) {
      if (f->flags & O_NONBLOCK) {
        if (written > 0)
          break;
        ret = -EAGAIN;
        goto out;
      }
      /* 阻塞写：挂 p->wq + prepare_to_wait 顺序（先挂 wq 标 BLOCKED → 重查空间
       * → signal_pending → schedule）。SPSC 无锁 ring，模式1，不引入 pipe 锁。
       */
      wait_queue_head *wq = p->wq;
      wait_queue_t wait;
      wait.func = pipe_wake_cb;
      wait.data = proc;
      list_init(&wait.node);
      add_wait_queue(wq, &wait);
      for (;;) {
        proc->state = BLOCKED;
        proc->wait_event = WAIT_NONE;
        if ((p->head + 1) % PIPE_BUF_SIZE != p->tail)
          break; /* 有空间 */
        if (f->flags & O_NONBLOCK) {
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(wq, &wait);
          if (written > 0)
            break;
          ret = -EAGAIN;
          goto out;
        }
        proc->wait_deadline = sched_clock() + 5000000000ULL;
        {
          int cpu = proc->assigned_cpu;
          uint64_t flags;
          spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
          sched_timer_queue_insert(cpu, proc);
          spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
        }
        schedule();
        if (proc->wait_timed_out) {
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(wq, &wait);
          if (written > 0)
            break;
          ret = -ETIMEDOUT;
          goto out;
        }
        {
          uint64_t pend =
              __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
          uint64_t deliv = pend & ~proc->proc->sig_blocked;
          deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
          if (deliv) {
            sched_cancel_spurious_wake(proc);
            remove_wait_queue(wq, &wait);
            if (written > 0)
              break;
            ret = -EINTR;
            goto out;
          }
        }
      }
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(wq, &wait);
      continue; // 回外层 while 重查（含 p_count<=1 的 EPIPE）
    }
    p->buf[p->head] = ((const char __force *)buf)[written];
    p->head = (p->head + 1) % PIPE_BUF_SIZE;
    written++;
  }

  __wake_up(p->wq, POLLIN);

  ret = (int64_t)written;

out:
  file_put(f);
  return ret;
}

// ===================== BSD syscall: read =====================
int64_t sys_read(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  char __user *buf = (char __user *__force)arg2;
  size_t len = (size_t)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)-EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;

  // file_operations 分发: f_op 非 NULL 时优先走 f_op
  if (f->f_op) {
    if (f->f_op->read)
      return f->f_op->read(proc, f, buf, len);
    ret = -EINVAL;
    goto out;
  }

  // FD_REGULAR: kernel FAT32 via page cache
  if (f->type == FD_REGULAR) {
    if ((f->flags & O_WRONLY) && !(f->flags & O_RDWR)) {
      ret = -EBADF;
      goto out;
    }
    struct inode *ip = f->inode;
    if (!ip) {
      ret = -EBADF;
      goto out;
    }
    uint64_t offset = f->offset;
    if (offset >= ip->size) {
      ret = 0;
      goto out;
    }

    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }

    uint64_t avail = ip->size - offset;
    if (len > avail)
      len = avail;

    int nread = fat32_read(ip, offset, (void __force *)buf, len);
    if (nread < 0) {
      ret = -(int64_t)nread;
      goto out;
    }
    f->offset = offset + nread;
    ret = (int64_t)nread;
    goto out;
  }

  // FD_FILE: proxy to fs_driver
  if (f->type == FD_FILE) {
    if ((f->flags & O_WRONLY) && !(f->flags & O_RDWR)) {
      ret = -EINVAL;
      goto out;
    }

    if (f->file_data._offset >= f->file_data.file_size) {
      ret = 0;
      goto out;
    }

    uint64_t avail = f->file_data.file_size - f->file_data._offset;
    if (len > avail)
      len = avail;
    size_t max_data = 65536 - sizeof(file_t_io_resp);
    if (len > max_data)
      len = max_data;
    if (len == 0) {
      ret = 0;
      goto out;
    }

    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }

    file_t_io_req req = {0};
    req.cmd = FILE_CMD_READ;
    req.fs_fd = f->file_data.fs_fd;
    req.offset = f->file_data._offset;
    req.count = (uint32_t)len;

    size_t resp_size = sizeof(file_t_io_resp) + (size_t)len;
    uint8_t *resp_buf = (uint8_t *)kmalloc(resp_size);
    if (!resp_buf) {
      ret = -ENOMEM;
      goto out;
    }

    int64_t msg_ret =
        sys_msg_to(f->file_data.fs_pid, &req, sizeof(req), resp_buf, resp_size);
    if (msg_ret < 0) {
      kfree(resp_buf);
      ret = -msg_ret;
      goto out;
    }

    file_t_io_resp *resp = (file_t_io_resp *)resp_buf;
    if (resp->status != 0) {
      kfree(resp_buf);
      ret = -(int64_t)resp->status;
      goto out;
    }

    size_t nread = resp->count;
    if (nread > len)
      nread = len;
    if (copy_to_user(buf, resp_buf + sizeof(file_t_io_resp), nread)) {
      kfree(resp_buf);
      ret = (int64_t)-EFAULT;
      goto out;
    }

    f->file_data._offset += nread;
    kfree(resp_buf);
    ret = (int64_t)nread;
    goto out;
  }

  // FD_SOCKET
  if (f->type == FD_SOCKET) {
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct unix_sock *sock = f->sock;
    if (!sock) {
      ret = -EBADF;
      goto out;
    }
    ret = unix_sock_read(sock, (void __force *)buf, len);
    goto out;
  }

  // FD_NETLINK: read → netlink recv
  if (f->type == FD_NETLINK) {
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct netlink_sock *nlsock = f->nlsock;
    if (!nlsock) {
      ret = -EBADF;
      goto out;
    }
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = len;
    ret = netlink_sock_recvmsg(nlsock, &iov, 1, NULL, NULL,
                               (f->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    goto out;
  }

  // FD_DEV
  if (f->type == FD_DEV) {
    if ((f->flags & O_WRONLY) && !(f->flags & O_RDWR)) {
      ret = -EINVAL;
      goto out;
    }
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct inode *ip = f->inode;
    if (!ip || !ip->i_priv) {
      ret = -ENODEV;
      goto out;
    }
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    if (ops->driver_pid == 0 && ops->read) {
      ret = (int64_t)ops->read(proc, fd, (void __force *)buf, len);
      goto out;
    }
    ret = -ENOSYS;
    goto out;
  }

  // FD_TTY
  if (f->type == FD_TTY) {
    if ((f->flags & O_WRONLY) && !(f->flags & O_RDWR)) {
      ret = -EINVAL;
      goto out;
    }
    if (!buf) {
      ret = -EFAULT;
      goto out;
    }
    uint64_t ptr_start = (__force uint64_t)buf;
    uint64_t ptr_end = ptr_start + len;
    if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
        ptr_end > KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    struct pty *pty = f->pty;
    if (!pty) {
      ret = -EBADF;
      goto out;
    }
    int is_master = pty_is_master_inode(f->inode);
    if (is_master)
      ret = pty_master_read(pty, proc, (void __force *)buf, len);
    else
      ret = pty_slave_read(pty, proc, (void __force *)buf, len);
    goto out;
  }

  // FD_EVENTFD: 8-byte counter read
  if (f->type == FD_EVENTFD) {
    ret = eventfd_do_read(f, buf);
    goto out;
  }

  // FD_TIMERFD: 8-byte tick count read
  if (f->type == FD_TIMERFD) {
    ret = timerfd_do_read(f, buf);
    goto out;
  }

  // FD_SIGNALFD: one signalfd_siginfo (128 bytes) read
  if (f->type == FD_SIGNALFD) {
    ret = signalfd_do_read(f, buf);
    goto out;
  }

  // FD_PIPE — explicit type dispatch so any unhandled fd type returns
  // -EINVAL instead of falling through into the pipe path (which once
  // dereferenced NULL f->pipe on a directory fd and page-faulted).
  if (f->type != FD_PIPE) {
    ret = -EINVAL;
    goto out;
  }
  if ((f->flags & O_WRONLY) && !(f->flags & O_RDWR)) {
    ret = -EINVAL;
    goto out;
  }

  if (!buf) {
    ret = -EFAULT;
    goto out;
  }
  uint64_t ptr_start = (__force uint64_t)buf;
  uint64_t ptr_end = ptr_start + len;
  if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
      ptr_end > KERNEL_VMA_BOUNDARY) {
    ret = -EFAULT;
    goto out;
  }

  struct pipe *p = f->pipe;
  if (!p) {
    ret = -EBADF;
    goto out;
  }

  while (p->head == p->tail) {
    if (refcount_read(&p->p_count) == 1) {
      ret = 0;
      goto out;
    }
    if (f->flags & O_NONBLOCK) {
      ret = -EAGAIN;
      goto out;
    }
    /* 阻塞读：挂 p->wq + prepare_to_wait 顺序（先挂 wq 标 BLOCKED → 重查
     * head!=tail → signal_pending → schedule）。SPSC 无锁 ring，模式1，不引入
     * pipe 锁。 */
    wait_queue_head *wq = p->wq;
    wait_queue_t wait;
    wait.func = pipe_wake_cb;
    wait.data = proc;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
    for (;;) {
      proc->state = BLOCKED;
      proc->wait_event = WAIT_NONE;
      if (refcount_read(&p->p_count) == 1) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(wq, &wait);
        ret = 0;
        goto out;
      }
      if (p->head != p->tail)
        break; /* 有数据 */
      if (f->flags & O_NONBLOCK) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(wq, &wait);
        ret = -EAGAIN;
        goto out;
      }
      proc->wait_deadline = sched_clock() + 5000000000ULL;
      {
        int cpu = proc->assigned_cpu;
        uint64_t flags;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
        sched_timer_queue_insert(cpu, proc);
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
      }
      schedule();
      if (proc->wait_timed_out) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(wq, &wait);
        ret = -ETIMEDOUT;
        goto out;
      }
      {
        uint64_t pend =
            __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->proc->sig_blocked;
        deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
        if (deliv) {
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(wq, &wait);
          ret = -EINTR;
          goto out;
        }
      }
    }
    sched_cancel_spurious_wake(proc);
    remove_wait_queue(wq, &wait);
  }

  {
    size_t nread = 0;
    while (nread < len && p->head != p->tail) {
      ((char __force *)buf)[nread] = p->buf[p->tail];
      p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
      nread++;
    }

    __wake_up(p->wq, POLLOUT);

    ret = (int64_t)nread;
    goto out;
  }

out:
  file_put(f);
  return ret;
}

// ===================== BSD syscall: close =====================
int64_t sys_close(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5) {
  int fd = (int)arg1;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  spinlock *fdlk = &current_proc->files->fd_lock;
  spin_lock(fdlk);
  struct file *f = fd_uninstall(current_proc->files, fd);
  spin_unlock(fdlk);
  if (!f)
    return (int64_t)-EBADF;
  synchronize_rcu();
  file_put(f);
  return 0;
}

// ===================== BSD syscall: dup2 =====================
int64_t sys_dup2(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4) {
  int old_fd = (int)arg1;
  int new_fd = (int)arg2;

  if (old_fd < 0 || old_fd >= MAX_FD || new_fd < 0 || new_fd >= MAX_FD)
    return (int64_t)-EBADF;

  if (old_fd == new_fd)
    return (int64_t)new_fd;

  xtask *proc = current_task;

  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);

  struct file *old_f = fd_lookup(proc->proc->files, old_fd);
  if (!old_f) {
    spin_unlock(fdlk);
    return (int64_t)-EBADF;
  }

  struct file *victim = fd_uninstall(proc->proc->files, new_fd);

  fd_install(proc->proc->files, new_fd, old_f);
  file_get(old_f);
  if (old_f->type == FD_TTY)
    pty_dup_file(old_f);

  spin_unlock(fdlk);
  if (victim) {
    synchronize_rcu();
    file_put(victim);
  }
  return (int64_t)new_fd;
}

// ===================== BSD syscall: fcntl =====================
int64_t sys_fcntl(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  int cmd = (int)arg2;
  int arg = (int)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)-EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  switch (cmd) {
  case F_GETFL:
    ret = (int64_t)f->flags;
    goto out;
  case F_SETFL:
    f->flags = (f->flags & ~O_SETFL_MASK) | (arg & O_SETFL_MASK);
    ret = 0;
    goto out;
  case F_ADD_SEALS: {
    if (f->type != FD_SHM) {
      ret = -EINVAL;
      goto out;
    }
    struct shm *shm = f->shm;
    if (!shm) {
      ret = -EBADF;
      goto out;
    }
    if (!(shm->flags & SHM_SEALED)) {
      ret = -EPERM;
      goto out;
    }
    if (shm->seals & F_SEAL_SEAL) {
      ret = -EPERM;
      goto out;
    }

    unsigned int new_seals = (unsigned int)arg;
    if (new_seals &
        ~(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) {
      ret = -EINVAL;
      goto out;
    }

    if (new_seals & F_SEAL_WRITE) {
      for (mmap_region *mr = proc->mm->mmap_regions; mr; mr = mr->next) {
        if (mr->shm_obj == shm) {
          break;
        }
      }
    }

    shm->seals |= new_seals;
    ret = 0;
    goto out;
  }
  case F_GET_SEALS: {
    if (f->type != FD_SHM) {
      ret = -EINVAL;
      goto out;
    }
    struct shm *shm = f->shm;
    if (!shm) {
      ret = -EBADF;
      goto out;
    }
    ret = (int64_t)shm->seals;
    goto out;
  }
  case F_GETFD:
    // Map kernel FD_CLOEXEC bit (0x8000) to POSIX userspace bit (1).
    ret = (f->flags & FD_CLOEXEC) ? 1 : 0;
    goto out;
  case F_SETFD:
    // POSIX userspace FD_CLOEXEC == 1; any other bits are ignored.
    if (arg & 1)
      f->flags |= FD_CLOEXEC;
    else
      f->flags &= ~FD_CLOEXEC;
    ret = 0;
    goto out;
  case F_DUPFD:
  case F_DUPFD_CLOEXEC: {
    int min_fd = (int)arg3;
    if (min_fd < 0 || min_fd >= MAX_FD) {
      ret = -EINVAL;
      goto out;
    }
    spinlock *fdlk = &proc->proc->files->fd_lock;
    spin_lock(fdlk);
    int new_fd = alloc_fd(proc->proc->files, min_fd);
    if (new_fd < 0) {
      spin_unlock(fdlk);
      ret = -EMFILE;
      goto out;
    }
    fd_install(proc->proc->files, new_fd, f);
    file_get(f);
    if (cmd == F_DUPFD_CLOEXEC)
      f->flags |= FD_CLOEXEC;
    spin_unlock(fdlk);
    ret = (int64_t)new_fd;
    goto out;
  }
  default:
    ret = -EINVAL;
    goto out;
  }
out:
  file_put(f);
  return ret;
}

// ===================== ENOSYS stubs (C group) =====================
int64_t sys_sendfile(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  return -ENOSYS;
}
int64_t sys_link(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6) {
  return -ENOSYS;
}
int64_t sys_symlink(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                    int64_t a6) {
  return -ENOSYS;
}
int64_t sys_readlink(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  return -ENOSYS;
}
int64_t sys_chmod(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6) {
  (void)a1;
  return 0;
}
int64_t sys_fchmod(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a1;
  return 0;
}
int64_t sys_chown(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  return 0;
}
int64_t sys_fchown(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  return 0;
}
int64_t sys_linkat(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  return -ENOSYS;
}
int64_t sys_symlinkat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  return -ENOSYS;
}
int64_t sys_readlinkat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                       int64_t a5, int64_t a6) {
  return -ENOSYS;
}
int64_t sys_fchmodat(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  return 0;
}
int64_t sys_fchownat(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  return 0;
}
int64_t sys_utimensat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  return -ENOSYS;
}
int64_t sys_clock_settime(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                          int64_t a5, int64_t a6) {
  return -ENOSYS;
}
int64_t sys_getitimer(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  return -ENOSYS;
}
int64_t sys_setitimer(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  return -ENOSYS;
}

// ===================== trivial-return stubs (C2 group) =====================
int64_t sys_access(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a1;
  (void)a2;
  return 0;
}
int64_t sys_faccessat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  return 0;
}

// ===================== Thin wrappers (A group) =====================

// A1: mkdirat(dirfd, path, mode) — AT_FDCWD-only thin wrapper over sys_mkdir
int64_t sys_mkdirat(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                    int64_t unused2, int64_t unused3) {
  int dirfd = (int)arg1;
  if (dirfd != AT_FDCWD)
    return -ENOSYS;
  return sys_mkdir(arg2, arg3, 0, 0, 0, 0);
}

// A2: unlinkat(dirfd, path, flags) — AT_FDCWD-only; AT_REMOVEDIR → rmdir, else
// → unlink
int64_t sys_unlinkat(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                     int64_t unused2, int64_t unused3) {
  int dirfd = (int)arg1;
  int flags = (int)arg3;
  if (dirfd != AT_FDCWD)
    return -ENOSYS;
  if (flags & AT_REMOVEDIR)
    return sys_rmdir(arg2, 0, 0, 0, 0, 0);
  return sys_unlink(arg2, 0, 0, 0, 0, 0);
}

// A3: renameat(olddirfd, oldpath, newdirfd, newpath) — AT_FDCWD-only
int64_t sys_renameat(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                     int64_t unused1, int64_t unused2) {
  int olddirfd = (int)arg1;
  int newdirfd = (int)arg3;
  if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD)
    return -ENOSYS;
  return sys_rename(arg2, arg4, 0, 0, 0, 0);
}

// A4: dup(oldfd) — find lowest available fd ≥ 0, install same file
int64_t sys_dup(int64_t arg1, int64_t unused1, int64_t unused2, int64_t unused3,
                int64_t unused4, int64_t unused5) {
  int old_fd = (int)arg1;
  if (old_fd < 0 || old_fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;
  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  struct file *old_f = fd_lookup(proc->proc->files, old_fd);
  if (!old_f) {
    spin_unlock(fdlk);
    return (int64_t)-EBADF;
  }
  int new_fd = alloc_fd(proc->proc->files, 0);
  if (new_fd < 0) {
    spin_unlock(fdlk);
    return (int64_t)-EMFILE;
  }
  fd_install(proc->proc->files, new_fd, old_f);
  file_get(old_f);
  spin_unlock(fdlk);
  return (int64_t)new_fd;
}

// A5: dup3(oldfd, newfd, flags) — like dup2 but with O_CLOEXEC support
// Temporary: sets FD_CLOEXEC on shared file struct (same bug as
// F_DUPFD_CLOEXEC, complete fix deferred to per-fd-flags infrastructure, see
// E3).
int64_t sys_dup3(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  int old_fd = (int)arg1;
  int new_fd = (int)arg2;
  int flags = (int)arg3;

  if (old_fd < 0 || old_fd >= MAX_FD || new_fd < 0 || new_fd >= MAX_FD)
    return (int64_t)-EBADF;
  if (old_fd == new_fd)
    return (int64_t)-EINVAL;

  xtask *proc = current_task;
  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);

  struct file *old_f = fd_lookup(proc->proc->files, old_fd);
  if (!old_f) {
    spin_unlock(fdlk);
    return (int64_t)-EBADF;
  }

  struct file *victim = fd_uninstall(proc->proc->files, new_fd);
  fd_install(proc->proc->files, new_fd, old_f);
  file_get(old_f);

  if (flags & O_CLOEXEC)
    old_f->flags |= FD_CLOEXEC; // known bug: sets on shared file (E3 deferred)

  spin_unlock(fdlk);
  if (victim) {
    synchronize_rcu();
    file_put(victim);
  }
  return (int64_t)new_fd;
}

// A8: gettimeofday(tv, tz) — thin wrapper over clock_gettime(CLOCK_REALTIME)
// tz is always ignored (Linux returns 0 for tz, most callers pass NULL).
// NOTE: CLOCK_REALTIME currently uses sched_clock() (monotonic, same as
// CLOCK_MONOTONIC) — no wall-clock source yet. See E7 (deferred).
int64_t sys_gettimeofday(int64_t arg1, int64_t arg2, int64_t unused1,
                         int64_t unused2, int64_t unused3, int64_t unused4) {
  struct timeval __user *tv = (struct timeval __user *)(uintptr_t)arg1;
  (void)arg2; // timezone always ignored

  if (!tv)
    return 0; // Linux: tv=NULL is valid (just don't fill it)

  uint64_t ns =
      sched_clock(); // CLOCK_REALTIME = sched_clock() for now (E7 TODO)
  struct timeval ktv;
  ktv.tv_sec = (time_t)(ns / 1000000000ULL);
  ktv.tv_usec = (long)(ns % 1000000000ULL) / 1000;

  if (copy_to_user(tv, &ktv, sizeof(ktv)))
    return -EFAULT;
  return 0;
}

// ===================== Simple kernel implementations (B group)
// =====================

// B1: pread64(fd, buf, count, offset) — read at specified offset without
// changing file offset
int64_t sys_pread64(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                    int64_t unused1, int64_t unused2) {
  int fd = (int)arg1;
  void __user *buf = (void __user *__force)arg2;
  size_t count = (size_t)arg3;
  uint64_t offset = (uint64_t)arg4;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_REGULAR) {
    ret = -ESPIPE;
    goto out;
  }
  if ((f->flags & O_WRONLY) && !(f->flags & O_RDWR)) {
    ret = -EBADF;
    goto out;
  }
  struct inode *ip = f->inode;
  if (!ip) {
    ret = -EBADF;
    goto out;
  }
  if (!buf) {
    ret = -EFAULT;
    goto out;
  }
  uint64_t ptr_start = (__force uint64_t)buf;
  uint64_t ptr_end = ptr_start + count;
  if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
      ptr_end > KERNEL_VMA_BOUNDARY) {
    ret = -EFAULT;
    goto out;
  }

  if (offset >= ip->size) {
    ret = 0;
    goto out;
  }
  size_t avail = ip->size - (size_t)offset;
  if (count > avail)
    count = avail;
  int nread = fat32_read(ip, offset, (void __force *)buf, count);
  if (nread < 0) {
    ret = -(int64_t)nread;
    goto out;
  }
  ret = (int64_t)nread;

out:
  file_put(f);
  return ret;
}

// B2: pwrite64(fd, buf, count, offset) — write at specified offset without
// changing file offset
int64_t sys_pwrite64(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                     int64_t unused1, int64_t unused2) {
  int fd = (int)arg1;
  const void __user *buf = (const void __user *__force)arg2;
  size_t count = (size_t)arg3;
  uint64_t offset = (uint64_t)arg4;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_REGULAR) {
    ret = -ESPIPE;
    goto out;
  }
  if (!(f->flags & (O_WRONLY | O_RDWR))) {
    ret = -EBADF;
    goto out;
  }
  struct inode *ip = f->inode;
  if (!ip) {
    ret = -EBADF;
    goto out;
  }
  if (!buf) {
    ret = -EFAULT;
    goto out;
  }
  uint64_t ptr_start = (__force uint64_t)buf;
  uint64_t ptr_end = ptr_start + count;
  if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
      ptr_end > KERNEL_VMA_BOUNDARY) {
    ret = -EFAULT;
    goto out;
  }

  int written = fat32_write(ip, offset, (const void __force *)buf, count);
  if (written < 0) {
    ret = (int64_t)written;
    goto out;
  }
  ret = (int64_t)written;

out:
  file_put(f);
  return ret;
}

// B3: readv(fd, iov, iovcnt) — scatter read over iovec array
int64_t sys_readv(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  const struct iovec __user *uiov = (const struct iovec __user *__force)arg2;
  int iovcnt = (int)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;
  if (!uiov || iovcnt < 0 || iovcnt > 1024)
    return -EINVAL;

  struct iovec *kiov =
      (struct iovec *)kmalloc(sizeof(struct iovec) * (size_t)iovcnt);
  if (!kiov)
    return -ENOMEM;
  if (copy_from_user(kiov, uiov, sizeof(struct iovec) * (size_t)iovcnt)) {
    kfree(kiov);
    return -EFAULT;
  }

  int64_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (!kiov[i].iov_base || kiov[i].iov_len == 0)
      continue;
    int64_t r = sys_read(fd, (int64_t)(__force uintptr_t)kiov[i].iov_base,
                         (int64_t)kiov[i].iov_len, 0, 0, 0);
    if (r < 0) {
      if (total > 0)
        break;
      kfree(kiov);
      return r;
    }
    total += r;
    if ((size_t)r < kiov[i].iov_len)
      break;
  }
  kfree(kiov);
  return total;
}

// B4: writev(fd, iov, iovcnt) — scatter write over iovec array
int64_t sys_writev(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                   int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  const struct iovec __user *uiov = (const struct iovec __user *__force)arg2;
  int iovcnt = (int)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;
  if (!uiov || iovcnt < 0 || iovcnt > 1024)
    return -EINVAL;

  struct iovec *kiov =
      (struct iovec *)kmalloc(sizeof(struct iovec) * (size_t)iovcnt);
  if (!kiov)
    return -ENOMEM;
  if (copy_from_user(kiov, uiov, sizeof(struct iovec) * (size_t)iovcnt)) {
    kfree(kiov);
    return -EFAULT;
  }

  int64_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (!kiov[i].iov_base || kiov[i].iov_len == 0)
      continue;
    int64_t r = sys_write(fd, (int64_t)(__force uintptr_t)kiov[i].iov_base,
                          (int64_t)kiov[i].iov_len, 0, 0, 0);
    if (r < 0) {
      if (total > 0)
        break;
      kfree(kiov);
      return r;
    }
    total += r;
    if ((size_t)r < kiov[i].iov_len)
      break;
  }
  kfree(kiov);
  return total;
}

// B5: uname(buf) — fill new_utsname struct
int64_t sys_uname(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5) {
  struct new_utsname __user *ubuf =
      (struct new_utsname __user *)(uintptr_t)arg1;

  if (!ubuf)
    return -EFAULT;

  struct new_utsname kbuf;
  __memset(&kbuf, 0, sizeof(kbuf));
  __strncpy(kbuf.sysname, "Xos", __NEW_UTS_LEN);
  __strncpy(kbuf.nodename, "(hostname)", __NEW_UTS_LEN);
  __strncpy(kbuf.release, "0.1", __NEW_UTS_LEN);
  __strncpy(kbuf.version, "#1 SMP", __NEW_UTS_LEN);
  __strncpy(kbuf.machine, "x86_64", __NEW_UTS_LEN);
  __strncpy(kbuf.domainname, "", __NEW_UTS_LEN);

  if (copy_to_user(ubuf, &kbuf, sizeof(kbuf)))
    return -EFAULT;
  return 0;
}

// ===================== BSD syscall: ioctl =====================
int64_t sys_ioctl(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  uint32_t cmd = (uint32_t)arg2;
  void __user *arg = (void __user *__force)arg3;

  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)(-(int64_t)EBADF);

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)(-(int64_t)EBADF);
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  // f_op with an ioctl callback handles the command entirely in-kernel
  // (e.g. pts/ptmx tty ioctl, evdev broker consumer EVIOCG*/GRAB). f_op without
  // ioctl must fall through to the FD_DEV driver_pid proxy path below so that
  // EVIOCG* reach the user-space driver (or INPUT_REGISTER hits the control
  // node).
  if (f->f_op && f->f_op->ioctl)
    return f->f_op->ioctl(proc, f, cmd, arg);

  switch (f->type) {
  case FD_DEV: {
    struct inode *ip = f->inode;
    if (!ip || !ip->i_priv) {
      ret = -(int64_t)ENODEV;
      goto out;
    }
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;

    /* RINGBUF_WAKE / RINGBUF_INJECT handlers removed (evdev broker replaces the
     * SHM ring; see kernel/bsd/evdev_broker.c). */

    if (ops->driver_pid == 0) {
      /* 控制节点 /dev/input/control 的 INPUT_REGISTER：走内核 direct path
       * 返回 owner write-fd（能 alloc_fd/fd_install，转发路径不能装 fd）。
       * arg 为用户指针，evdev_control_ioctl 内部 copy_from_user。 */
      if (cmd == INPUT_REGISTER) {
        long r = evdev_control_ioctl(cmd, arg);
        ret = (int64_t)r;
        goto out;
      }
      if (!ops->ioctl) {
        ret = -(int64_t)ENOTTY;
        goto out;
      }

      uint16_t arg_size = _IOC_SIZE(cmd);
      uint8_t dir = _IOC_DIR(cmd);
      printk(LOG_DEBUG, "sys_ioctl(direct): pid=%d cmd=0x%x dir=%u size=%u\n",
             current_task->pid, cmd, dir, arg_size);

      uint8_t kbuf_stack[256];
      uint8_t *kbuf = kbuf_stack;
      bool kbuf_dyn = false;
      if (arg_size > 240) {
        kbuf = kmalloc(arg_size);
        if (!kbuf) {
          ret = -(int64_t)ENOMEM;
          goto out;
        }
        kbuf_dyn = true;
      }
      __memset(kbuf, 0, arg_size);

      if ((__force uint64_t)arg != 0 && (dir & _IOC_WRITE) && arg_size > 0) {
        size_t cfu = copy_from_user(kbuf, arg, arg_size);
        if (cfu) {
          printk(LOG_DEBUG, "sys_ioctl: copy_from_user EFAULT cfu=%zu\n", cfu);
          if (kbuf_dyn)
            kfree(kbuf);
          ret = (int64_t)-EFAULT;
          goto out;
        }
      }

      long result = ops->ioctl(cmd, kbuf);
      printk(LOG_DEBUG, "sys_ioctl: ops->ioctl result=%ld\n", result);

      if ((__force uint64_t)arg != 0 && (dir & _IOC_READ) && result >= 0 &&
          arg_size > 0) {
        size_t ctu = copy_to_user(arg, kbuf, arg_size);
        if (ctu) {
          printk(LOG_DEBUG, "sys_ioctl: copy_to_user EFAULT ctu=%zu\n", ctu);
          if (kbuf_dyn)
            kfree(kbuf);
          ret = (int64_t)-EFAULT;
          goto out;
        }
      }

      if (kbuf_dyn)
        kfree(kbuf);

      ret = (int64_t)result;
      goto out;
    }
    // User-space driver: IPC proxy
    pid_t target_pid = ops->driver_pid;
    if (target_pid <= 0) {
      ret = -(int64_t)ENODEV;
      goto out;
    }
    printk(LOG_DEBUG, "sys_ioctl: fd=%d cmd=0x%x driver_pid=%d (req path)\n",
           fd, cmd, target_pid);

    uint16_t arg_size = _IOC_SIZE(cmd);
    uint8_t dir = _IOC_DIR(cmd);

    if (arg_size > 48) {
      if ((__force uint64_t)arg == 0) {
        ret = -(int64_t)EINVAL;
        goto out;
      }
    }

    if (target_pid < 0 || target_pid >= MAX_PROC) {
      ret = -(int64_t)ESRCH;
      goto out;
    }
    xtask *target = task_get(target_pid);
    if (target->pid != target_pid) {
      ret = -(int64_t)ESRCH;
      goto out;
    }

    // === Branch point: arg_size <= 48 inline, > 48 variable-length ===
    if (arg_size <= 48) {
      // ===== inline path (RECV_REQ + req_data[56] = cmd + arg) =====
      uint8_t req_data[56];
      __memset(req_data, 0, 56);
      *(uint32_t *)req_data = cmd;
      if ((dir & _IOC_WRITE) && (__force uint64_t)arg != 0) {
        if (arg_size > 0) {
          if (copy_from_user(req_data + 4, arg, arg_size)) {
            ret = (int64_t)-EFAULT;
            goto out;
          }
        }
      }

      uint8_t msg[RECV_MSG_SIZE];
      recv_msg *hdr = (recv_msg *)msg;
      hdr->type = RECV_REQ;
      hdr->src = (uint32_t)current_task->pid;
      // Device minor at data[52..55] (after cmd at [0..3] and arg data at
      // [4..4+arg_size); arg_size<=48 so no overlap). evdev reads it from
      // msg->data+52 to route the ioctl to the right device.
      *(uint32_t *)(req_data + 52) = ops->minor;
      __memcpy(hdr->data, req_data, 56);

      spin_lock(&target->recv_lock);
      uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
      if (next == target->recv_tail) {
        spin_unlock(&target->recv_lock);
        ret = -(int64_t)EBUSY;
        goto out;
      }
      __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
      target->recv_head = next;
      spin_unlock(&target->recv_lock);

      int target_cpu = target->assigned_cpu;
      uint64_t flags;
      spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
      if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
        wake_from_wait(target);
      }
      spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);

      // Wake target's ipcfd wq, if any (evdev_refact.md §5.6).  evdev blocks in
      // epoll_wait (WAIT_POLL) on its ipcfd, not WAIT_RECV, so without this the
      // REQ strands on its recv queue until the 3s caller timeout — black
      // screen on EVIOCGBIT.  Same wake as sys_req/sys_notify.
      if (target->ipcfd_file) {
        wait_queue_head *iwq = file_wq_get(target->ipcfd_file);
        if (iwq)
          __wake_up(iwq, POLLIN);
      }

      // Block caller on WAIT_REQ_REPLY. Arm the wait (set BLOCKED + deadline +
      // insert timer) under our own scheduler_lock, in the same critical
      // section. Without this, the target (evdev on another CPU) can receive
      // the REQ, run sys_resp() and try to wake us between the wake above and
      // setting BLOCKED below — sys_resp's wake-check then sees us
      // not-yet-BLOCKED and drops the wake, stranding us until the 3s timeout.
      // Now that sys_recv's lost-wake is fixed, evdev replies fast, so this
      // caller-side window is the more likely failure; close it the same way.
      //
      // req_replied is set by sys_resp under this lock before its wake-check;
      // we clear it before arming and re-check it under the lock. If the reply
      // already landed, stay RUNNING and return without sleeping (lost-wake
      // guard). See sys_req for the full rationale.
      proc->req_target_pid = target_pid;
      proc->req_reply_buf = arg;
      proc->req_reply_len = arg_size;
      proc->req_result = 0;
      proc->req_replied = 0;
      proc->wait_timed_out = 0;

      if (sched_arm_timed_wait(proc, WAIT_REQ_REPLY,
                               sched_clock() + 3000000000ULL,
                               &proc->req_replied))
        schedule();

      file_put(f);
      f = NULL;

      if (proc->wait_timed_out)
        return (int64_t)-ETIMEDOUT;
      if (proc->req_result != 0)
        return (int64_t)proc->req_result;
      return 0;
    }

    // ===== variable-length path (RECV_IOCTL + kmalloc'd buffer) =====
    if ((__force uint64_t)arg == 0) {
      ret = -(int64_t)EINVAL;
      goto out;
    }

    void *kbuf = kmalloc(arg_size);
    if (!kbuf) {
      ret = -(int64_t)ENOMEM;
      goto out;
    }
    if ((dir & _IOC_WRITE) && (__force uint64_t)arg != 0) {
      if (copy_from_user(kbuf, arg, arg_size)) {
        kfree(kbuf);
        ret = (int64_t)-EFAULT;
        goto out;
      }
    }

    uint8_t msg[RECV_MSG_SIZE];
    recv_msg *hdr = (recv_msg *)msg;
    __memset(msg, 0, RECV_MSG_SIZE);
    hdr->type = RECV_IOCTL;
    hdr->src = (uint32_t)current_task->pid;
    hdr->ioctl.cmd = cmd;
    hdr->ioctl.arg_size = arg_size;
    hdr->ioctl.kmaddr = kbuf;
    hdr->ioctl.len = arg_size;
    hdr->ioctl.minor = ops->minor;

    spin_lock(&target->recv_lock);
    uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
    if (next == target->recv_tail) {
      spin_unlock(&target->recv_lock);
      kfree(kbuf);
      ret = -(int64_t)EBUSY;
      goto out;
    }
    __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
    target->recv_head = next;
    spin_unlock(&target->recv_lock);

    {
      int target_cpu = target->assigned_cpu;
      uint64_t flags;
      spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
      if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
        wake_from_wait(target);
      }
      spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);

      // Wake target's ipcfd wq, if any — same evdev WAIT_POLL fix as the inline
      // path above (evdev drains RECV_IOCTL from its epoll ipcfd branch).
      if (target->ipcfd_file) {
        wait_queue_head *iwq = file_wq_get(target->ipcfd_file);
        if (iwq)
          __wake_up(iwq, POLLIN);
      }
    }

    // Block caller on WAIT_REQ_REPLY. Arm the wait under our own scheduler_lock
    // — same caller-side lost-wake fix + req_replied guard as the inline path
    // above (see comment there).
    proc->req_target_pid = target_pid;
    proc->req_reply_buf = arg;
    proc->req_reply_len = arg_size;
    proc->req_result = 0;
    proc->req_replied = 0;
    proc->wait_timed_out = 0;

    if (sched_arm_timed_wait(proc, WAIT_REQ_REPLY,
                             sched_clock() + 3000000000ULL, &proc->req_replied))
      schedule();

    file_put(f);
    f = NULL;

    if (proc->wait_timed_out)
      return (int64_t)-ETIMEDOUT;
    if (proc->req_result != 0)
      return (int64_t)proc->req_result;
    return 0;
  }
  case FD_TTY: {
    struct pty *pty = f->pty;
    if (!pty) {
      ret = -(int64_t)EBADF;
      goto out;
    }
    ret = (int64_t)pty_ioctl(pty, cmd, arg);
    goto out;
  }
  case FD_SOCKET:
  case FD_NETLINK:
  case FD_PIPE:
  case FD_REGULAR:
  case FD_DIR:
  case FD_FILE:
  case FD_SHM:
    ret = -(int64_t)ENOTTY;
    goto out;
  default:
    ret = -(int64_t)EBADF;
    goto out;
  }
out:
  if (f)
    file_put(f);
  return ret;
}

// ===================== BSD syscall: fstat =====================
int64_t sys_fstat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4) {
  int fd = (int)arg1;
  struct kstat __user *ust = (struct kstat __user * __force) arg2;

  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)(-(int64_t)EBADF);

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)(-(int64_t)EBADF);
  }
  file_get(f);
  rcu_read_unlock();

  struct kstat ks;
  __memset(&ks, 0, sizeof(ks));
  ks.st_nlink = 1;
  ks.st_blksize = 512;

  int64_t ret;
  switch (f->type) {
  case FD_REGULAR: {
    struct inode *ip = f->inode;
    if (!ip) {
      ret = -(int64_t)EBADF;
      goto out;
    }
    ks.st_ino = ip->ino;
    ks.st_size = ip->size;
    ks.st_mode = S_IFREG | 0644;
    ks.st_uid = current_proc->uid;
    ks.st_gid = current_proc->gid;
    ks.st_blocks = (ip->size + 511) / 512;
    break;
  }
  case FD_DIR: {
    struct inode *ip = f->inode;
    if (!ip) {
      ret = -(int64_t)EBADF;
      goto out;
    }
    ks.st_ino = ip->ino;
    ks.st_mode = S_IFDIR | 0755;
    ks.st_blocks = 0;
    break;
  }
  case FD_DEV: {
    struct inode *ip = f->inode;
    if (!ip) {
      ret = -(int64_t)EBADF;
      goto out;
    }
    ks.st_ino = ip->ino;
    /* DRM 设备号同 devtmpfs_getattr（0.5-3）：libdrm fstat 判 render/primary。
     */
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    if (ops && __strcmp(ops->subsystem, "drm") == 0)
      ks.st_rdev = k_makedev(DRM_MAJOR_FOR_STAT, ops->minor);
    else
      ks.st_rdev = ip->ino;
    if (ops && ops->is_block)
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
    ret = -(int64_t)EBADF;
    goto out;
  }

  if (copy_to_user(ust, &ks, sizeof(ks))) {
    ret = -(int64_t)EFAULT;
    goto out;
  }
  ret = 0;
out:
  file_put(f);
  return ret;
}

// ===================== BSD syscall: fdev_pid =====================
int64_t sys_fdev_pid(int64_t arg1, int64_t unused2, int64_t unused3,
                     int64_t unused4, int64_t unused5, int64_t unused6) {
  int fd = (int)arg1;
  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)(-(int64_t)EBADF);

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)(-(int64_t)EBADF);
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_DEV) {
    ret = -(int64_t)EBADF;
    goto out;
  }

  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv) {
    ret = 0;
    goto out;
  }
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  ret = (int64_t)ops->driver_pid;
out:
  file_put(f);
  return ret;
}

// ===================== BSD syscall: memfd_create =====================
int64_t sys_memfd_create(int64_t arg1, int64_t arg2, int64_t unused1,
                         int64_t unused2, int64_t unused3, int64_t unused4) {
  const char __user *user_name = (const char __user *__force)arg1;
  unsigned int flags = (unsigned int)arg2;

  if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
    return -EINVAL;

  xtask *proc = current_task;

  struct shm *shm = (struct shm *)kmalloc(sizeof(struct shm));
  if (!shm)
    return -ENOMEM;

  shm->phys = 0;
  shm->npages = 0;
  shm->file_size = 0;
  refcount_set(&shm->s_count, 1);
  shm->flags = (flags & MFD_ALLOW_SEALING) ? SHM_SEALED : 0;
  shm->seals = 0;
  shm->page_list = NULL;
  shm->num_pages = 0;

  if (user_name) {
    uint64_t uptr = (__force uint64_t)user_name;
    if (uptr >= KERNEL_VMA_BOUNDARY) {
      kfree(shm);
      return -EFAULT;
    }
    long nlen = strncpy_from_user(shm->name, user_name, 31);
    if (nlen < 0) {
      kfree(shm);
      return -EFAULT;
    }
    if (nlen > 31)
      nlen = 31;
    shm->name[nlen] = '\0';
  } else {
    shm->name[0] = '\0';
  }

  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(proc->proc->files, 2);
  if (fd < 0) {
    spin_unlock(fdlk);
    kfree(shm);
    return -EMFILE;
  }

  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    fd_uninstall(proc->proc->files, fd);
    spin_unlock(fdlk);
    kfree(shm);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_SHM;
  f->flags = O_RDWR | ((flags & MFD_CLOEXEC) ? FD_CLOEXEC : 0);
  f->shm = shm;
  fd_install(proc->proc->files, fd, f);
  spin_unlock(fdlk);

  return (int64_t)fd;
}

// ===================== BSD syscall: ftruncate =====================
int64_t sys_ftruncate(int64_t arg1, int64_t arg2, int64_t unused1,
                      int64_t unused2, int64_t unused3, int64_t unused4) {
  int fd = (int)arg1;
  int64_t size = (int64_t)arg2;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)-EBADF;
  }

  /* Regular files: dispatch size change to i_op->setattr (锁由 setattr
   * 内部持,对齐 §6.6;消除硬编码 fat32_ftruncate)。 */
  if (f->type == FD_REGULAR) {
    struct inode *ip = f->inode;
    rcu_read_unlock();
    if (!ip)
      return (int64_t)-EBADF;
    if (size < 0)
      return (int64_t)-EINVAL;
    if (!ip->i_op || !ip->i_op->setattr)
      return (int64_t)-EINVAL;
    int rc = ip->i_op->setattr(ip, (uint64_t)size);
    return (int64_t)rc;
  }

  if (f->type != FD_SHM) {
    rcu_read_unlock();
    return (int64_t)-EINVAL;
  }
  if (!f->shm) {
    rcu_read_unlock();
    return (int64_t)-EBADF;
  }
  struct shm *shm = f->shm;
  rcu_read_unlock();

  if (size < 0)
    return (int64_t)-EINVAL;

  size_t new_size = (size_t)size;
  size_t new_npages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
  size_t old_total = shm->page_list ? (size_t)shm->num_pages : shm->npages;

  if (new_npages > old_total) {
    if (shm->seals & F_SEAL_GROW)
      return (int64_t)-EPERM;

    size_t extra = new_npages - old_total;

    if (!shm->page_list && shm->npages == 0) {
      struct page *pages = bfc_alloc_page(new_npages);
      if (pages) {
        uint64_t phys = (__force uint64_t)page_to_phys(pages);
        __memset((__force void *)phys_to_virt((__force phys_addr_t)phys), 0,
                 new_npages * PAGE_SIZE);
        shm->phys = phys;
        shm->npages = new_npages;
        shm->file_size = new_size;
        return 0;
      }
    }

    if (!shm->page_list && shm->npages > 0) {
      size_t total = shm->npages + extra;
      int list_cap = (int)((total + 15) / 16 * 16);
      if (list_cap < 16)
        list_cap = 16;
      shm->page_list = (uint64_t *)kmalloc((size_t)list_cap * sizeof(uint64_t));
      if (!shm->page_list)
        return (int64_t)-ENOMEM;

      for (size_t i = 0; i < shm->npages; i++) {
        shm->page_list[i] = shm->phys + i * PAGE_SIZE;
      }
      shm->num_pages = (int)shm->npages;
      shm->phys = 0;
      shm->npages = 0;

      for (size_t i = 0; i < extra; i++) {
        uint64_t pphys = shm_add_page(shm);
        if (!pphys) {
          for (size_t j = 0; j < i; j++) {
            struct page *p = &bfc_frames[PHY_TO_PAGE(
                shm->page_list[shm->num_pages - 1 - j])];
            bfc_free_page(p, 1);
          }
          kfree(shm->page_list);
          shm->page_list = NULL;
          shm->num_pages = 0;
          return (int64_t)-ENOMEM;
        }
        shm->page_list[shm->num_pages] = pphys;
        shm->num_pages++;
      }
    } else if (shm->page_list) {
      for (size_t i = 0; i < extra; i++) {
        uint64_t pphys = shm_add_page(shm);
        if (!pphys) {
          for (size_t j = 0; j < i; j++) {
            struct page *p =
                &bfc_frames[PHY_TO_PAGE(shm->page_list[--shm->num_pages])];
            bfc_free_page(p, 1);
          }
          return (int64_t)-ENOMEM;
        }
        shm->page_list[shm->num_pages] = pphys;
        shm->num_pages++;
      }
    }

    shm->file_size = new_size;

  } else if (new_npages < old_total) {
    if (shm->seals & F_SEAL_SHRINK)
      return (int64_t)-EPERM;

    if (shm->page_list) {
      int free_start = (int)new_npages;
      for (int i = free_start; i < shm->num_pages; i++) {
        struct page *p = &bfc_frames[PHY_TO_PAGE(shm->page_list[i])];
        bfc_free_page(p, 1);
      }
      shm->num_pages = (int)new_npages;
      if (shm->num_pages == 0) {
        kfree(shm->page_list);
        shm->page_list = NULL;
      }
    } else {
      uint64_t free_phys = shm->phys + new_npages * PAGE_SIZE;
      size_t free_npages = shm->npages - new_npages;
      struct page *page = &bfc_frames[PHY_TO_PAGE(free_phys)];
      bfc_free_page(page, free_npages);
      shm->npages = new_npages;
    }

    shm->file_size = new_size;
  } else {
    shm->file_size = new_size;
  }

  return 0;
}

// ===================== BSD syscall: block_async =====================
int64_t sys_block_async(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                        int64_t unused1, int64_t unused2) {
  uint32_t lba = (uint32_t)arg1;
  void __user *buf = (void __user *__force)arg2;
  uint32_t count = (uint32_t)arg3;
  uint8_t dir = (uint8_t)arg4;

  int ret = ahci_submit_async(lba, (void __force *)buf, count, dir);
  return (int64_t)ret;
}

// ===================== BSD syscall: debug_memstat =====================
int64_t sys_debug_memstat(int64_t arg1, int64_t arg2, int64_t unused1,
                          int64_t unused2, int64_t unused3, int64_t unused4) {
  void __user *buf = (void __user *__force)(uintptr_t)arg1;
  size_t len = (size_t)arg2;
  if (!buf || len < sizeof(struct kernel_mem_stats))
    return (int64_t)-EINVAL;

  struct kernel_mem_stats stats;
  __memset(&stats, 0, sizeof(stats));
  stats.total_pages = kernel_mem_stats.total_pages;
  stats.used_pages = kernel_mem_stats.used_pages;
  stats.slab_used_bytes = kernel_mem_stats.slab_used_bytes;
  stats.slab_peak_bytes = kernel_mem_stats.slab_peak_bytes;
  stats.kmalloc_calls = kernel_mem_stats.kmalloc_calls;
  stats.kfree_calls = kernel_mem_stats.kfree_calls;

  if (copy_to_user(buf, &stats, sizeof(stats)))
    return (int64_t)-EFAULT;
  return (int64_t)sizeof(stats);
}

// ===================== BSD syscall: install_fd =====================
int64_t sys_install_fd_impl(int64_t arg1, int64_t arg2, int64_t arg3,
                            int64_t arg4, int64_t arg5, int64_t unused1) {
  pid_t fs_pid = (pid_t)arg1;
  int32_t fs_fd = (int32_t)arg2;
  uint64_t offset = arg3;
  int flags = (int)arg4;
  uint64_t file_size = arg5;

  if (fs_pid < 0 || fs_pid >= MAX_PROC)
    return (int64_t)-EINVAL;
  if (fs_fd < 0)
    return (int64_t)-EINVAL;
  if (flags & ~(O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK))
    return (int64_t)-EINVAL;

  xtask *proc = current_task;

  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(fdlk);
    return (int64_t)-EMFILE;
  }

  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    fd_uninstall(proc->proc->files, fd);
    spin_unlock(fdlk);
    return (int64_t)-ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_FILE;
  f->flags = flags;
  f->file_data.fs_pid = fs_pid;
  f->file_data.fs_fd = fs_fd;
  f->file_data._offset = offset;
  f->file_data.file_size = file_size;
  refcount_set(&f->file_data.f_count, 1);
  fd_install(proc->proc->files, fd, f);

  spin_unlock(fdlk);
  return (int64_t)fd;
}

// ===================== BSD syscall: dma_alloc =====================
int64_t sys_dma_alloc(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                      int64_t unused2, int64_t unused3) {
  size_t size = (size_t)arg1;
  void __user *__user *vaddr_ptr = (void __user *__user *__force)arg2;
  uint64_t __user *paddr_ptr = (uint64_t __user * __force) arg3;

  if (size == 0)
    return (int64_t)-EINVAL;

  uint64_t vp = (__force uint64_t)vaddr_ptr;
  uint64_t pp = (__force uint64_t)paddr_ptr;
  if (!vp || vp >= KERNEL_VMA_BOUNDARY ||
      vp + sizeof(void *) > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;
  if (!pp || pp >= KERNEL_VMA_BOUNDARY ||
      pp + sizeof(int64_t) > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;

  size = ALIGN_UP(size, PAGE_SIZE);
  size_t npages = size / PAGE_SIZE;

  struct page *pages = bfc_alloc_page_low(npages);
  if (!pages)
    return (int64_t)-ENOMEM;

  uint64_t phys = (__force uint64_t)page_to_phys(pages);
  xtask *proc = current_task;

  uint64_t vaddr = proc->mm->mmap_brk;
  uint64_t vaddr_end = vaddr + size;

  for (size_t i = 0; i < npages; i++) {
    uint64_t page_phys = phys + i * PAGE_SIZE;
    uint64_t page_vaddr = vaddr + i * PAGE_SIZE;
    if (!map_user_page_direct(
            (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3),
            page_vaddr, page_phys, PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
      if (i > 0)
        unmap_user_pages(
            (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3),
            vaddr, vaddr + i * PAGE_SIZE, i);
      bfc_free_page(pages, npages);
      return (int64_t)-ENOMEM;
    }
  }

  proc->mm->mmap_brk = vaddr_end;

  mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (!region) {
    unmap_user_pages(
        (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3), vaddr,
        vaddr_end, npages);
    bfc_free_page(pages, npages);
    return (int64_t)-ENOMEM;
  }
  region->vaddr = vaddr;
  region->size = size;
  region->phys = phys;
  region->next = proc->mm->mmap_regions;
  proc->mm->mmap_regions = region;

  {
    uint64_t vaddr_val = vaddr;
    if (copy_to_user(vaddr_ptr, &vaddr_val, sizeof(vaddr_val)))
      return (int64_t)-EFAULT;
  }
  if (copy_to_user(paddr_ptr, &phys, sizeof(phys)))
    return (int64_t)-EFAULT;

  return 0;
}

// ===================== BSD syscall: dma_free =====================
int64_t sys_dma_free(int64_t arg1, int64_t unused1, int64_t unused2,
                     int64_t unused3, int64_t unused4, int64_t unused5) {
  uint64_t vaddr = (int64_t)arg1;
  if (!vaddr)
    return (int64_t)-EINVAL;

  xtask *proc = current_task;

  mmap_region **pp = &proc->mm->mmap_regions;
  while (*pp) {
    mmap_region *r = *pp;
    if (r->vaddr == vaddr) {
      size_t npages = r->size / PAGE_SIZE;

      unmap_user_pages(
          (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3),
          r->vaddr, r->vaddr + r->size, npages);

      struct page *page = bfc_frames + (r->phys / PAGE_SIZE);
      bfc_free_page(page, npages);

      *pp = r->next;
      kfree(r);
      return 0;
    }
    pp = &r->next;
  }

  return (int64_t)-EINVAL;
}

// ===================== BSD syscall: lseek =====================
int64_t sys_lseek(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  int64_t offset = (int64_t)arg2;
  int whence = (int)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return (int64_t)-EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;

  if (f->type == FD_PIPE || f->type == FD_SOCKET || f->type == FD_NETLINK ||
      f->type == FD_DEV) {
    ret = -ESPIPE;
    goto out;
  }

  if (f->type == FD_REGULAR || f->type == FD_DIR) {
    struct inode *ip = f->inode;
    if (!ip) {
      ret = -EBADF;
      goto out;
    }
    int64_t new_offset;
    switch (whence) {
    case SEEK_SET:
      new_offset = offset;
      break;
    case SEEK_CUR:
      new_offset = (int64_t)f->offset + offset;
      break;
    case SEEK_END:
      new_offset = (int64_t)ip->size + offset;
      break;
    default: {
      ret = -EINVAL;
      goto out;
    }
    }
    if (new_offset < 0) {
      ret = -EINVAL;
      goto out;
    }
    f->offset = (int64_t)new_offset;
    ret = (int64_t)new_offset;
    goto out;
  }

  if (f->type != FD_FILE) {
    ret = -ESPIPE;
    goto out;
  }

  {
    uint64_t new_offset;
    switch (whence) {
    case SEEK_SET:
      new_offset = (int64_t)offset;
      break;
    case SEEK_CUR:
      new_offset = f->file_data._offset + offset;
      break;
    case SEEK_END:
      new_offset = f->file_data.file_size + offset;
      break;
    default: {
      ret = -EINVAL;
      goto out;
    }
    }

    f->file_data._offset = new_offset;
    ret = (int64_t)new_offset;
  }
out:
  file_put(f);
  return ret;
}

// ===================== Session/pgid syscalls =====================
int64_t sys_setsid(int64_t unused1, int64_t unused2, int64_t unused3,
                   int64_t unused4, int64_t unused5, int64_t unused6) {
  if (current_proc->sid == current_task->pid)
    return (int64_t)-EPERM;
  current_proc->sid = current_task->pid;
  current_proc->pgid = current_task->pid;
  return (int64_t)current_proc->sid;
}

int64_t sys_setpgid(int64_t arg1, int64_t arg2, int64_t unused1,
                    int64_t unused2, int64_t unused3, int64_t unused4) {
  pid_t pid = (pid_t)arg1;
  pid_t pgid = (pid_t)arg2;
  if (pid < 0 || pgid < 0)
    return (int64_t)-EINVAL;
  if (pid == 0)
    pid = current_task->pid;
  if (pgid == 0)
    pgid = pid;
  if (pid >= MAX_PROC || task_get(pid)->pid != pid)
    return (int64_t)-ESRCH;
  if (pid != current_task->pid) {
    if (task_get(pid)->mm->parent_pid != current_task->pid)
      return (int64_t)-ESRCH;
    if (task_get(pid)->proc->sid != current_proc->sid)
      return (int64_t)-EPERM;
  }
  task_get(pid)->proc->pgid = pgid;
  return 0;
}

int64_t sys_getpgid(int64_t arg1, int64_t unused1, int64_t unused2,
                    int64_t unused3, int64_t unused4, int64_t unused5) {
  pid_t pid = (pid_t)arg1;
  if (pid == 0)
    pid = current_task->pid;
  if (pid < 0 || pid >= MAX_PROC || task_get(pid)->pid != pid)
    return (int64_t)-ESRCH;
  return (int64_t)task_get(pid)->proc->pgid;
}

int64_t sys_getsid(int64_t arg1, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4, int64_t unused5) {
  pid_t pid = (pid_t)arg1;
  if (pid == 0)
    pid = current_task->pid;
  if (pid < 0 || pid >= MAX_PROC || task_get(pid)->pid != pid)
    return (int64_t)-ESRCH;
  return (int64_t)task_get(pid)->proc->sid;
}

// ===================== BSD syscall dispatch =====================
int64_t syscall_dispatch(trapframe *tf) {
  int64_t nr = tf->rax;
  switch (nr) {
  case SYS_EXIT:
    return sys_exit(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_WAIT4:
    return sys_wait4(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MMAP:
    return sys_mmap(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MUNMAP:
    return sys_munmap(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MPROTECT:
    return sys_mprotect(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SYSCONF:
    return sys_sysconf(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETRANDOM:
    return sys_getrandom(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_PIPE:
    return sys_pipe(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_WRITE:
    return sys_write(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_READ:
    return sys_read(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_CLOSE:
    return sys_close(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_DUP2:
    return sys_dup2(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FCNTL:
    return sys_fcntl(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_IOCTL:
    return sys_ioctl(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FSTAT:
    return sys_fstat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FDEV_PID:
    return sys_fdev_pid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_LSEEK:
    return sys_lseek(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MEMFD_CREATE:
    return sys_memfd_create(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FTRUNCATE:
    return sys_ftruncate(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_DMA_ALLOC:
    return sys_dma_alloc(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_DMA_FREE:
    return sys_dma_free(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_PCI_DEV_INFO:
    return sys_pci_dev_info(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_BLOCK_ASYNC:
    return sys_block_async(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_INSTALL_FD:
    return sys_install_fd_impl(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                               tf->r9);
  case SYS_DEBUG_MEMSTAT:
    return sys_debug_memstat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                             tf->r9);
  case SYS_KILL:
    return sys_kill(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RT_SIGACTION:
    return sys_sigaction(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RT_SIGRETURN:
    return sys_sigreturn(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETSID:
    return sys_setsid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETPGID:
    return sys_setpgid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETPGID:
    return sys_getpgid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETSID:
    return sys_getsid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FORK:
    return sys_fork(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_EXECVE:
    return sys_execve(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // VFS syscalls (implemented in kernel/vfs.c)
  case SYS_OPEN:
    return sys_open(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_STAT:
    return sys_stat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_OPENAT:
    return sys_openat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_NEWFSTATAT:
    return sys_newfstatat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MKDIR:
    return sys_mkdir(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_UNLINK:
    return sys_unlink(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RENAME:
    return sys_rename(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RMDIR:
    return sys_rmdir(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_DEV_CREATE:
    return sys_dev_create(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETDENTS64:
    return sys_getdents(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // Socket syscalls (implemented in kernel/socket.c)
  case SYS_SOCKET:
    return sys_socket(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_BIND:
    return sys_bind(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_LISTEN:
    return sys_listen(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_ACCEPT:
    return sys_accept(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_CONNECT:
    return sys_connect(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SOCKETPAIR:
    return sys_socketpair(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SENDMSG:
    return sys_sendmsg(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RECVMSG:
    return sys_recvmsg(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SHUTDOWN:
    return sys_shutdown(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETSOCKNAME:
    return sys_getsockname(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETPEERNAME:
    return sys_getpeername(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETSOCKOPT:
    return sys_setsockopt(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETSOCKOPT:
    return sys_getsockopt(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_POLL:
    return sys_poll(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // Thread syscalls
  case SYS_EXIT_GROUP:
    return sys_exit_group(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_TGKILL:
    return sys_tgkill(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RT_SIGPROCMASK:
    return sys_sigprocmask(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SET_TID_ADDRESS:
    return sys_set_tid_address(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                               tf->r9);
  case SYS_CLONE:
    return sys_clone(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8);
  case SYS_FUTEX:
    return sys_futex(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_ARCH_PRCTL:
    return sys_arch_prctl(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_PTHREAD_SET_CANCEL_HANDLER:
    return sys_pthread_set_cancel_handler(tf->rdi, tf->rsi, tf->rdx, tf->r10,
                                          tf->r8, tf->r9);
  // POSIX identity & permissions (group 1)
  case SYS_GETUID:
    return sys_getuid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETEUID:
    return sys_geteuid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETGID:
    return sys_getgid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETEGID:
    return sys_getegid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETUID:
    return sys_setuid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETGID:
    return sys_setgid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETPPID:
    return sys_getppid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETPGRP:
    return sys_getpgrp(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_UMASK:
    return sys_umask(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETHOSTNAME:
    return sys_gethostname(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETHOSTNAME:
    return sys_sethostname(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // alarm / pause (group 2)
  case SYS_ALARM:
    return sys_alarm(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_PAUSE:
    return sys_pause(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // truncate / fsync / sync (group 3)
  case SYS_TRUNCATE:
    return sys_truncate(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FSYNC:
    return sys_fsync(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SYNC:
    return sys_sync(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // POSIX signal (group 4)
  case SYS_RT_SIGPENDING:
    return sys_sigpending(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // epoll
  case SYS_EPOLL_CREATE:
    return sys_epoll_create(tf->rdi);
  case SYS_EPOLL_CREATE1:
    return sys_epoll_create1(tf->rdi);
  case SYS_EPOLL_CTL:
    return sys_epoll_ctl(tf->rdi, tf->rsi, tf->rdx, tf->r10);
  case SYS_EPOLL_WAIT:
    return sys_epoll_wait(tf->rdi, tf->rsi, tf->rdx, tf->r10);
  case SYS_EPOLL_PWAIT:
    return sys_epoll_pwait(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_EVENTFD2:
    return sys_eventfd2(tf->rdi, tf->rsi);
  case SYS_TIMERFD_CREATE:
    return sys_timerfd_create(tf->rdi, tf->rsi);
  case SYS_TIMERFD_SETTIME:
    return sys_timerfd_settime(tf->rdi, tf->rsi, tf->rdx, tf->r10);
  case SYS_SIGNALFD4:
    return sys_signalfd4(tf->rdi, tf->rsi, tf->rdx, tf->r10);
  case SYS_MOUNT:
    return sys_mount(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // ENOSYS stubs (C group)
  case SYS_SENDFILE:
    return sys_sendfile(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_LINK:
    return sys_link(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SYMLINK:
    return sys_symlink(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_READLINK:
    return sys_readlink(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_UTIMENSAT:
    return sys_utimensat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_CLOCK_SETTIME:
    return sys_clock_settime(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                             tf->r9);
  case SYS_GETITIMER:
    return sys_getitimer(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETITIMER:
    return sys_setitimer(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_LINKAT:
    return sys_linkat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SYMLINKAT:
    return sys_symlinkat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_READLINKAT:
    return sys_readlinkat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // trivial-return stubs (C2 group)
  case SYS_ACCESS:
    return sys_access(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FACCESSAT:
    return sys_faccessat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_CHMOD:
    return sys_chmod(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FCHMOD:
    return sys_fchmod(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FCHMODAT:
    return sys_fchmodat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_CHOWN:
    return sys_chown(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FCHOWN:
    return sys_fchown(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FCHOWNAT:
    return sys_fchownat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // Thin wrappers (A group)
  case SYS_DUP:
    return sys_dup(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_DUP3:
    return sys_dup3(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MKDIRAT:
    return sys_mkdirat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_UNLINKAT:
    return sys_unlinkat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RENAMEAT:
    return sys_renameat(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RECVFROM:
    return sys_recvfrom(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SENDTO:
    return sys_sendto(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETTIMEOFDAY:
    return sys_gettimeofday(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // Simple kernel implementations (B group)
  case SYS_PREAD64:
    return sys_pread64(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_PWRITE64:
    return sys_pwrite64(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_READV:
    return sys_readv(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_WRITEV:
    return sys_writev(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_UNAME:
    return sys_uname(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_DEV_SET_META:
    return sys_dev_set_meta(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MKNOD:
    return sys_mknod(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_IPCFD_CREATE:
    return sys_ipcfd_create();
  case SYS_IPCFD_READ:
    return sys_ipcfd_read(tf->rdi, tf->rsi, tf->rdx, tf->r10);
  // SYS_CLONE(56)/SYS_FUTEX(202)/SYS_ARCH_PRCTL(158) implemented in phase 3b,
  // this phase returns -ENOSYS
  default:
    printk(LOG_WARN, "syscall_dispatch: unknown syscall nr=%lu pid=%d\n",
           (unsigned long)nr, current_task->pid);
    return -ENOSYS;
  }
}
