/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/syscall.c — BSD syscall implementations
// Extracted from kernel/trap.c (phase 3 step 3.3)

#include "kernel/bsd/syscall.h"

#include <stdbool.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/rtc.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "boot/boot.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/evdev_broker.h"
#include "kernel/bsd/eventfd.h"
#include "kernel/bsd/eventpoll.h"
#include "kernel/bsd/fat32.h"
#include "kernel/bsd/file_lock.h"
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
#include "kernel/kernel.h" // hostname_get (sys_uname nodename)
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h" // copy_*/strncpy_from_user declarations
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mem/vma.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"
#include "utils/macro.h"

#include "kernel/bsd/kfcntl.h"
#include <xos/capability.h>
#include <xos/confname.h> // _SC_* (shared with user-side sysconf)
#include <xos/errno.h>
#include <xos/ioctl.h>
#include <xos/mman.h>
#include <xos/page.h>
#include <xos/prctl.h>
#include <xos/signal.h>
#include <xos/socket.h>
#include <xos/stat.h>
#include <xos/statfs.h>
#include <xos/syscall.h>
#include <xos/syscall_nums.h>
#include <xos/time.h>
#include <xos/utsname.h>

// OS-unique mmap flags (not in uapi mman.h, collision-free in 1024+ namespace)
#define MAP_PHYSICAL 0x80000000
#define MAP_UC 0x08 /* Map as uncacheable (device MMIO) */

// Every MAP_* bit the kernel recognizes — uapi standard flags plus the two
// OS-internal ones. Unknown bits (outside this set) are rejected with
// -EINVAL; recognized-but-unsupported bits (HUGETLB/POPULATE/STACK/LOCKED/
// NORESERVE/GROWSDOWN/GROWSUP) are silently no-op'd since the existing
// sys_mmap branches only inspect SHARED/PRIVATE/FIXED*/ANONYMOUS/PHYSICAL/UC.
// Kept here (not in a header) because MAP_PHYSICAL/MAP_UC are syscall.c-local.
#define MAP_KNOWN_FLAGS                                                        \
  (MAP_SHARED | MAP_PRIVATE | MAP_SHARED_VALIDATE | MAP_FIXED |                \
   MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_GROWSUP |         \
   MAP_LOCKED | MAP_NORESERVE | MAP_POPULATE | MAP_STACK | MAP_HUGETLB |       \
   MAP_PHYSICAL | MAP_UC)

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

  // 3. Orphan adoption: reparent all children whose signal->parent_pid matches
  //    the dying process. The parent relationship is per-process
  //    (signal->parent_pid), NOT per-mm: a CLONE_VM child shares its parent's
  //    mm (whose parent_pid is the grandparent), so matching on mm->parent_pid
  //    would miss shared-vm children and leave them pointing at a dead PID.
  //    Update signal->parent_pid (used by waitpid/child scan, do_exit's SIGCHLD
  //    notification, and sys_getppid) to init. mm->parent_pid is left as-is; it
  //    is no longer the authority for parentage.
  if (init_pid >= 0) {
    spin_lock(&tasks_lock);
    for (int i = 0; i < MAX_PROC; i++) {
      if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->proc &&
          tasks[i]->proc->signal &&
          tasks[i]->proc->signal->parent_pid == proc->pid) {
        tasks[i]->proc->signal->parent_pid = init_pid;
      }
    }
    spin_unlock(&tasks_lock);
  }

  // 4. clear_tid_addr: write 0 + futex_wake (pthread_join relies on this
  //    wakeup) BEFORE ZOMBIE — proc is alive, no concurrent sched_task_reap
  //    possible.
  if (proc->proc->clear_tid_addr) {
    // clear_tid_addr may be set two ways: CLONE_CHILD_SETTID (fork writes the
    // child's tid there before scheduling it) or sys_set_tid_address (POSIX:
    // the kernel only records the address; it does NOT write tid into it, so
    // the word may hold any user value). The previous ASSERT(*==pid) guarded
    // the CLONE_CHILD_SETTID timing race (bug.md Bug 2) but wrongly fired on
    // the set_tid_address path (e.g. test_set_tid_address_64bit stores 0x1234).
    // Linux's do_exit does not assert here — clearing + futex_wake is the
    // whole contract — so the check is dropped. Bug 2's lost-wakeup is already
    // prevented at fork (CLONE_CHILD_SETTID writes tid before the child runs).
    *((int *)(uintptr_t)proc->proc->clear_tid_addr) = 0;
    sys_futex((int64_t)proc->proc->clear_tid_addr, (int64_t)FUTEX_WAKE, 1, 0, 0,
              0);
  }

  // 4b. robust-futex list: walk the dying thread's robust list and mark any
  //     still-held robust mutex with FUTEX_OWNER_DIED + wake a waiter, so a
  //     blocked acquirer can recover the lock. Must run before ZOMBIE / mm_put
  //     (proc->proc alive and pml4 alive so copy_from_user on user pointers
  //     works). No-op if set_robust_list was never called (in-tree pthread).
  exit_robust_list(proc->proc, proc->pid);

  // 5. Thread-group bookkeeping BEFORE ZOMBIE (proc/signal alive).
  //    Read signal fields into locals — after ZOMBIE we must not touch sig.
  struct signal_struct *sig = proc->proc->signal;
  pid_t ppid = sig->parent_pid;
  // S19 §1: exit_signal selects which signal the parent is notified with on
  // the last thread's exit (default SIGCHLD; 0 = do not notify, e.g. a
  // CLONE_THREAD exit reports via clear_tid_addr + futex, not a signal).
  int esig = proc->proc->exit_signal;
  atomic_dec(&sig->live_count);
  int notify_parent = atomic_dec_and_test(&sig->thread_count);
  /* 02 SA_NOCLDWAIT: the parent opted out of zombies — skip SIGCHLD posting
   * and have init reap this child. We can NOT sched_task_reap() here: the
   * dying task is still current and its cr3 is live, so freeing its mm/PML4
   * mid-exit would triple-fault. Instead reparent the child to init (mirrors
   * orphan adoption in step 3) and wake init's WAIT_CHILD — init reaps the
   * zombie in its own context. Read the original parent here (step 5, sig
   * alive); after ZOMBIE only the local bool + reparented parent_pid are
   * used. */
  bool auto_reap = false;
  if (notify_parent && ppid >= 0 && ppid < MAX_PROC &&
      task_get(ppid)->pid == ppid) {
    xtask *parent = task_get(ppid);
    if (parent->proc &&
        (parent->proc->signal->action[SIGCHLD].sa_flags & SA_NOCLDWAIT)) {
      auto_reap = true;
      if (init_pid >= 0) {
        sig->parent_pid = init_pid;
        ppid = init_pid;
      }
    }
  }

  // 6. Set ZOMBIE
  //    GATE: after this, sched_task_reap/proc_reap on another CPU may kfree
  //    proc and signal_put signal_struct. Do NOT dereference proc->proc or sig.
  int cpu = proc->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  proc->state = ZOMBIE;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

  // 7. Notify parent (last thread) — uses local ppid, not sig->parent_pid.
  //    Two independent effects:
  //    - wake a parent blocked in waitpid: a reapable child is a thread-group
  //      *leader* (tgid == pid). Its exit must wake the parent's WAIT_CHILD
  //      even when exit_signal==0 — clone's low byte 0 means "no SIGCHLD
  //      posted", NOT "do not reap". A non-leader thread exit (CLONE_THREAD)
  //      keeps esig==0 and is not a reapable child; it relies on clear_tid_addr
  //      + futex_wake, so it does NOT wake waitpid (would spuriously wake a
  //      waitpid targeting a different child).
  //    - post the exit signal: only when esig != 0 (SIGCHLD default, or the
  //      clone low byte; 0 = post nothing).
  //    tgid==pid leader detection is correct because step 6 of sys_clone sets
  //    tgid = alloc_idx (= pid) for non-CLONE_THREAD children.
  bool is_leader = (proc->tgid == proc->pid);
  if (notify_parent && (is_leader || esig != 0)) {
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
        // 02 SA_NOCLDWAIT: skip SIGCHLD posting (parent opted out; when
        // auto_reap reparented to init, init reaps without needing SIGCHLD).
        if (!auto_reap && esig != 0) {
          __atomic_or_fetch(&parent->proc->sig_pending, SIGMASK(esig),
                            __ATOMIC_RELEASE);
        }
        int pcpu = parent->assigned_cpu;
        uint64_t pflags;
        spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
        if (is_leader && parent->state == BLOCKED &&
            parent->wait_event == WAIT_CHILD) {
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
  //    02 SA_NOCLDWAIT: no reap here — the child is ZOMBIE and will be reaped
  //    by init (reparented in step 5) via waitpid(-1). Reaping current would
  //    free the live cr3 mid-exit.
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
  uint64_t gflags;
  spin_lock_irqsave(&sig->sig_lock, &gflags);
  sig->group_exit = 1;
  sig->group_exit_code = status;
  spin_unlock_irqrestore(&sig->sig_lock, gflags);

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
// Mirror of user/include/sys/wait.h options. WNOHANG returns 0 without
// blocking. S01 adds WUNTRACED: a stopped child is reported once (one-shot
// stop_reported flag) without being reaped, with *wstatus =
// (stopsig << 8) | 0x7f.
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4
// Linux wait4/waitid extension flags (bits/waitflags.h values). This kernel has
// no thread-group vs clone-child distinction — all children already match
// (child_matches keys on signal->parent_pid), so __WALL/__WNOTHREAD are no-ops
// and __WCLONE is accepted-but-equivalent-to-__WALL.
#define __WCLONE 0x80000000
#define __WALL 0x40000000
#define __WNOTHREAD 0x20000000
// Mask of every wait option bit this kernel recognizes (mirrors Linux do_wait,
// which rejects unknown option bits). __WALL/__WNOTHREAD are no-ops here (all
// children already match; wait doesn't cross threads); __WCLONE has no
// distinguishable semantics without a tgroup field, so it is accepted but
// treated like __WALL.
#define __W_KNOWN                                                              \
  (WNOHANG | WUNTRACED | WCONTINUED | __WALL | __WCLONE | __WNOTHREAD)

// S19 §5: kernel-side rusage layout, byte-identical to
// user/include/sys/resource.h (struct timeval comes from <xos/time.h>). Filled
// from the child's cpu_time_ns before sched_task_reap frees the xtask;
// copy_to_user delivers it to the caller. Only ru_utime is populated (this OS
// does not separate user/system CPU time yet — recorded in todo.md); the rest
// is zeroed.
struct k_rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long ru_maxrss;
  long ru_ixrss;
  long ru_idrss;
  long ru_isrss;
  long ru_minflt;
  long ru_majflt;
  long ru_nswap;
  long ru_inblock;
  long ru_oublock;
  long ru_msgsnd;
  long ru_msgrcv;
  long ru_nsignals;
  long ru_nvcsw;
  long ru_nivcsw;
};

// S19 §5.1: does `candidate` match the waitpid/wait4 selection rule?
//   pid == -1  : any child of caller
//   pid == 0   : any child whose pgid == caller's pgid
//   pid < -1   : any child whose pgid == -pid
//   pid > 0    : the specific child pid
// "child of caller" = candidate's signal->parent_pid == caller_pid. The parent
// relationship is per-process (signal->parent_pid), NOT per-mm: a CLONE_VM
// child shares its parent's mm, whose parent_pid is the *grandparent*, so using
// mm->parent_pid here would reject shared-vm children (see test_clone_*).
// Called under tasks_lock (caller's proc/pgid stable; candidate fields read
// under the lock).
static inline bool child_matches(xtask *candidate, pid_t pid, pid_t caller_pid,
                                 pid_t caller_pgid) {
  if (!candidate || candidate->pid < 0 || !candidate->proc ||
      !candidate->proc->signal)
    return false;
  if (candidate->proc->signal->parent_pid != caller_pid)
    return false;
  if (pid == -1)
    return true;
  if (pid == 0)
    return candidate->proc->pgid == caller_pgid;
  if (pid < -1)
    return candidate->proc->pgid == -pid;
  // pid > 0
  return candidate->pid == pid;
}

// S19 §5.2: fill a kernel rusage from a reaped child's cpu_time_ns. The whole
// CPU time is reported as ru_utime (no user/sys split yet); ru_stime stays 0.
static void fill_rusage(struct k_rusage *ru, uint64_t cpu_time_ns) {
  __memset(ru, 0, sizeof(*ru));
  ru->ru_utime.tv_sec = (long)(cpu_time_ns / 1000000000ULL);
  ru->ru_utime.tv_usec = (long)((cpu_time_ns % 1000000000ULL) / 1000);
}

// S01 helper: report a stopped child to a waitpid caller. Writes the
// WIFSTOPPED encoding into *wstatus and sets the one-shot stop_reported flag
// so the same stop is not reported twice. Returns 1 if reported, 0 if the
// caller did not request WUNTRACED or this stop was already reported.
static int waitpid_report_stopped(xtask *child, int32_t __user *wstatus,
                                  int wuntraced) {
  if (!wuntraced || child->stop_reported)
    return 0;
  if (wstatus) {
    uint64_t p = (__force uint64_t)wstatus;
    if (p && p < KERNEL_VMA_BOUNDARY &&
        p + sizeof(int32_t) - 1 < KERNEL_VMA_BOUNDARY)
      *(__force int32_t *)wstatus = child->exit_code;
  }
  child->stop_reported = 1;
  return 1;
}

// S19 §5.3: report a continued child to a waitpid caller under WCONTINUED.
// Writes the WIFCONTINUED status (0xffff, see user/include/sys/wait.h) and
// clears the one-shot cont_pending. Returns 1 if reported, 0 if the caller did
// not request WCONTINUED or this continue was already reported.
static int waitpid_report_continued(xtask *child, int32_t __user *wstatus,
                                    int wcontinued) {
  if (!wcontinued || !child->cont_pending)
    return 0;
  if (wstatus) {
    uint64_t p = (__force uint64_t)wstatus;
    if (p && p < KERNEL_VMA_BOUNDARY &&
        p + sizeof(int32_t) - 1 < KERNEL_VMA_BOUNDARY)
      *(__force int32_t *)wstatus = (int32_t)0xffff;
  }
  child->cont_pending = 0;
  return 1;
}

static int64_t sys_waitpid_rusage(int64_t arg1, int64_t arg2, int64_t options,
                                  struct k_rusage *rusage_out, int64_t unused3,
                                  int64_t unused4) {
  pid_t pid = (pid_t)arg1;
  int32_t __user *exit_code_ptr = (int32_t __user * __force) arg2;
  int nohang = (int)options & WNOHANG;
  int wuntraced = (int)options & WUNTRACED;
  int wcontinued = (int)options & WCONTINUED;
  // Accept the Linux wait4 extension flags. Reject any genuinely unknown
  // option bit (see __W_KNOWN above).
  if ((int)options & ~__W_KNOWN)
    return (int64_t)-EINVAL;
  pid_t caller_pid = current_task->pid;
  pid_t caller_pgid = current_proc ? current_proc->pgid : 0;

  // S19 §5.1: pid <= 0 selects a *set* of children (any / by process group),
  // scanned over the whole task table. pid > 0 targets one specific child.
  if (pid <= 0) {
    while (1) {
      spin_lock(&tasks_lock);
      xtask *zombie = NULL;
      xtask *stopped = NULL;
      xtask *continued = NULL;
      bool has_children = false;
      for (int i = 0; i < MAX_PROC; i++) {
        xtask *t = tasks[i];
        if (!child_matches(t, pid, caller_pid, caller_pgid))
          continue;
        has_children = true;
        if (t->state == ZOMBIE) {
          zombie = t;
          break;
        }
        if (wuntraced && t->state == STOPPED && !t->stop_reported)
          stopped = stopped ? stopped : t;
        if (wcontinued && t->cont_pending)
          continued = continued ? continued : t;
      }
      if (!has_children) {
        spin_unlock(&tasks_lock);
        return (int64_t)-ECHILD;
      }
      // S01: report a stopped child without reaping it (one-shot).
      if (stopped) {
        spin_unlock(&tasks_lock);
        if (waitpid_report_stopped(stopped, exit_code_ptr, wuntraced))
          return (int64_t)stopped->pid;
        continue;
      }
      // S19 §5.3: report a continued child without reaping it (one-shot).
      if (continued) {
        spin_unlock(&tasks_lock);
        if (waitpid_report_continued(continued, exit_code_ptr, wcontinued))
          return (int64_t)continued->pid;
        continue;
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
        // S19 §5.2: capture rusage BEFORE sched_task_reap frees the xtask
        // (reap may kmem_cache_free the xtask object → UAF if read after).
        if (rusage_out)
          fill_rusage(rusage_out, zombie->cpu_time_ns);
        sched_task_reap(zombie);
        return (int64_t)zpid;
      }
      spin_unlock(&tasks_lock);

      // WNOHANG: no zombie ready, don't block — return 0 immediately.
      if (nohang)
        return 0;

      // Snapshot the merged pending set (private sig_pending + thread-group
      // shared_pending) BEFORE taking scheduler_lock. We can't call
      // signal_pending() under scheduler_lock (it takes sig_lock → reversed
      // order vs the wake path's sig_lock→scheduler_lock). A signal arriving
      // after this snapshot still wakes us via wake_process_any and the next
      // loop iteration re-snapshots.
      uint64_t pend =
          __atomic_load_n(&current_proc->sig_pending, __ATOMIC_ACQUIRE);
      {
        uint64_t sflags;
        spin_lock_irqsave(&current_proc->signal->sig_lock, &sflags);
        pend |= current_proc->signal->shared_pending;
        spin_unlock_irqrestore(&current_proc->signal->sig_lock, sflags);
      }

      int pcpu = current_task->assigned_cpu;
      uint64_t pflags;
      spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
      spin_lock(&tasks_lock);
      zombie = NULL;
      stopped = NULL;
      continued = NULL;
      has_children = false;
      for (int i = 0; i < MAX_PROC; i++) {
        xtask *t = tasks[i];
        if (!child_matches(t, pid, caller_pid, caller_pgid))
          continue;
        has_children = true;
        if (t->state == ZOMBIE) {
          zombie = t;
          break;
        }
        if (wuntraced && t->state == STOPPED && !t->stop_reported)
          stopped = stopped ? stopped : t;
        if (wcontinued && t->cont_pending)
          continued = continued ? continued : t;
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
      if (stopped) {
        spin_unlock(&tasks_lock);
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
        if (waitpid_report_stopped(stopped, exit_code_ptr, wuntraced))
          return (int64_t)stopped->pid;
        continue;
      }
      if (continued) {
        spin_unlock(&tasks_lock);
        spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
        if (waitpid_report_continued(continued, exit_code_ptr, wcontinued))
          return (int64_t)continued->pid;
        continue;
      }
      spin_unlock(&tasks_lock);
      // Before blocking: if a signal is pending, return EINTR rather than
      // sleeping. Checking here (not after schedule()) lets a wakeup that
      // simultaneously marks a child ZOMBIE and posts its exit_signal be
      // resolved as a reaped child (loop top re-scan) instead of EINTR.
      // `pend` was snapshotted before scheduler_lock (merges shared_pending so
      // a kill()-delivered signal interrupts waitpid too).
      {
        uint64_t deliv = pend & ~current_proc->sig_blocked;
        deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
        deliv &= ~(SIGMASK(SIGCHLD));
        if (deliv) {
          spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
          return (int64_t)-EINTR;
        }
      }
      current_task->wait_event = WAIT_CHILD;
      current_task->state = BLOCKED;
      spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
      schedule();
    }
  }

  // pid > 0: wait for a specific child. pid is in range [1, MAX_PROC) here.
  if (pid >= MAX_PROC) {
    printk(LOG_WARN, "waitpid: pid=%d out of range\n", pid);
    return -EINVAL;
  }

  xtask *child = task_get(pid);

  spin_lock(&tasks_lock);
  if (!child_matches(child, pid, caller_pid, caller_pgid)) {
    printk(LOG_WARN,
           "waitpid: pid=%d validation fail: child_pid=%d parent_pid=%d "
           "caller=%d\n",
           pid, child->pid,
           (child->proc && child->proc->signal)
               ? child->proc->signal->parent_pid
               : -1,
           caller_pid);
    spin_unlock(&tasks_lock);
    return -ECHILD;
  }
  spin_unlock(&tasks_lock);

  while (1) {
    // S01: report a stopped child once under WUNTRACED before blocking. The
    // child stays STOPPED (not reaped); stop_reported is the one-shot gate.
    if (wuntraced) {
      int ccpu = child->assigned_cpu;
      uint64_t cflags;
      spin_lock_irqsave(&cpu_locals[ccpu].scheduler_lock, &cflags);
      int is_stopped = (child->state == STOPPED);
      spin_unlock_irqrestore(&cpu_locals[ccpu].scheduler_lock, cflags);
      if (is_stopped && waitpid_report_stopped(child, exit_code_ptr, wuntraced))
        return (int64_t)child->pid;
    }
    // S19 §5.3: report a continued child once under WCONTINUED before blocking.
    if (wcontinued) {
      int ccpu = child->assigned_cpu;
      uint64_t cflags;
      spin_lock_irqsave(&cpu_locals[ccpu].scheduler_lock, &cflags);
      int wants_cont = child->cont_pending;
      spin_unlock_irqrestore(&cpu_locals[ccpu].scheduler_lock, cflags);
      if (wants_cont &&
          waitpid_report_continued(child, exit_code_ptr, wcontinued))
        return (int64_t)child->pid;
    }

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

    // Before blocking: if a signal is pending, return EINTR now rather than
    // sleeping. Checking here (not after schedule()) matters when the child
    // exited and posted its exit_signal in the same wakeup — schedule() returns
    // with both "child is ZOMBIE" and "signal pending" true, and we must let
    // the loop top reap the ZOMBIE (returning pid) instead of bailing with
    // EINTR. Linux/POSIX: report a ready child state change over an interrupt.
    // Merge shared_pending so kill()-delivered signals interrupt waitpid too.
    {
      uint64_t pend =
          __atomic_load_n(&current_proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t sflags;
      spin_lock_irqsave(&current_proc->signal->sig_lock, &sflags);
      pend |= current_proc->signal->shared_pending;
      spin_unlock_irqrestore(&current_proc->signal->sig_lock, sflags);
      uint64_t deliv = pend & ~current_proc->sig_blocked;
      deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
      deliv &= ~(SIGMASK(SIGCHLD));
      if (deliv) {
        printk(LOG_WARN, "waitpid: pid=%d EINTR pending=0x%lx\n", pid, pend);
        return (int64_t)-EINTR;
      }
    }

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
  // S19 §5.2: capture rusage BEFORE sched_task_reap frees the xtask.
  if (rusage_out)
    fill_rusage(rusage_out, child->cpu_time_ns);
  sched_task_reap(child);
  return (int64_t)pid;
}

// sys_waitpid: user-side waitpid() entry. No rusage (wait4 provides it).
int64_t sys_waitpid(int64_t arg1, int64_t arg2, int64_t options,
                    int64_t unused2, int64_t unused3, int64_t unused4) {
  return sys_waitpid_rusage(arg1, arg2, options, NULL, unused3, unused4);
}

// wait4(pid, wstatus, options, rusage). S19 §5: rusage (if non-NULL) is filled
// from the reaped child's cpu_time_ns before reap and copied out to the caller.
int64_t sys_wait4(int64_t pid, int64_t wstatus, int64_t options, int64_t rusage,
                  int64_t unused1, int64_t unused2) {
  struct k_rusage kru;
  struct k_rusage *ru_out = (rusage != 0) ? &kru : NULL;
  int64_t ret = sys_waitpid_rusage(pid, wstatus, options, ru_out, 0, 0);
  if (ret < 0 || !ru_out)
    return ret;
  // ret == reaped pid. copy_to_user under the user CR3 (user pages are not
  // mapped in the kernel CR3). A fault here is unrecoverable (child already
  // reaped) — return -EFAULT as Linux does on a bad rusage pointer.
  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)current_task->cr3)
                   : "memory");
  size_t ctu = copy_to_user((void __user *)(uintptr_t)rusage, &kru,
                            sizeof(struct k_rusage));
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  if (ctu)
    return (int64_t)-EFAULT;
  return ret;
}

// ===================== S12: file-backed mmap =====================
// Helpers below are called from sys_mmap *under* proc->mm->mmap_lock (the
// dispatch takes the lock before calling them). They therefore do NOT take the
// lock, but must spin_unlock_irqrestore it on every error return path (matching
// sys_mmap's own error paths). On success they return the mapped vaddr (with
// the lock still held so sys_mmap's tail unlocks once); the region records only
// metadata — pages are faulted in on demand by file_fault_handler.

// Place a fresh region describing [vaddr, vaddr+size) with the given backing
// fields. Uses vma_pick_addr for placement (honors MAP_FIXED / hint per S11).
// Returns the region (inserted) or NULL + *out_err set (caller
// unlocks+returns).
static mmap_region *mmap_place_file_region(xtask *proc, uint64_t *pml4,
                                           uint64_t addr, uint64_t size,
                                           uint32_t flags, uint64_t hint,
                                           uint32_t prot, int fd,
                                           uint64_t offset, int64_t *out_err) {
  int64_t picked = vma_pick_addr(proc->mm, pml4, addr, size, flags, hint);
  if (picked < 0) {
    *out_err = picked;
    return NULL;
  }
  uint64_t vaddr = (uint64_t)picked;
  mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (!region) {
    *out_err = -ENOMEM;
    return NULL;
  }
  __memset(region, 0, sizeof(*region));
  region->vaddr = vaddr;
  region->size = size;
  region->phys = 0;
  region->shm_obj = NULL;
  region->prot = prot;
  region->fd = fd;
  region->offset = offset;
  region->flags = flags;
  region->next = NULL;
  if (vma_insert_sorted(proc->mm, region) != 0) {
    kfree(region);
    *out_err = -ENOMEM;
    return NULL;
  }
  if (vaddr == proc->mm->mmap_brk)
    proc->mm->mmap_brk = vaddr + size;
  return region;
}

// memfd (FD_SHM) MAP_PRIVATE: private COW mapping of a memfd. The region holds
// a shm_get reference to the source shm; file_fault_handler copies pages out of
// shm->phys / shm->page_list into private user pages on demand.
static int64_t sys_mmap_shm_private(xtask *proc, uint64_t *pml4, uint64_t addr,
                                    uint64_t size, uint32_t flags,
                                    uint64_t hint, uint32_t prot,
                                    struct file *f, int fd, uint64_t offset,
                                    uint64_t mmap_flags) {
  struct shm *shm = f->shm;
  if (!shm) {
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EBADF;
  }
  // Reject mappings beyond the memfd's current size (ftruncate-grown). Linux
  // allows mmap past EOF with zero pages; S12 rejects it (todo).
  uint64_t shm_size = shm->page_list ? (uint64_t)shm->num_pages * PAGE_SIZE
                                     : (uint64_t)shm->npages * PAGE_SIZE;
  if (offset + size > shm_size) {
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EINVAL;
  }

  shm_get(
      shm); // region holds a reference; released in munmap/mm_release/execve
  int64_t err = 0;
  mmap_region *region = mmap_place_file_region(proc, pml4, addr, size, flags,
                                               hint, prot, fd, offset, &err);
  if (!region) {
    shm_put(shm);
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return err;
  }
  region->shm_private_src = shm;
  // inode stays NULL: memfd has no page-cache inode; fault reads shm pages.

  spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
  return (int64_t)region->vaddr;
}

// FD_REGULAR file-backed mmap (MAP_PRIVATE or MAP_SHARED). Takes an inode_get
// reference on the region so the mapping survives close(fd) (Linux semantics);
// file_fault_handler reads mr->inode directly, never fd_lookup.
static int64_t sys_mmap_file_backed(xtask *proc, uint64_t *pml4, uint64_t addr,
                                    uint64_t size, uint32_t prot, int flags,
                                    int fd, uint64_t offset,
                                    uint64_t mmap_flags, uint64_t hint) {
  if (fd >= MAX_FD) {
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EBADF;
  }
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (f)
    file_get(f);
  rcu_read_unlock();
  if (!f) {
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EBADF;
  }

  // memfd (FD_SHM): MAP_SHARED stays on the existing shared page-list path;
  // MAP_PRIVATE takes the new private COW path.
  if (f->type == FD_SHM) {
    if (flags & MAP_SHARED) {
      // Hand back to sys_mmap's existing SHM path (it re-looks-up the fd).
      file_put(f);
      return (int64_t)-ENOSYS; // sentinel: caller falls through (lock held)
    }
    int64_t r = sys_mmap_shm_private(proc, pml4, addr, size, (uint32_t)flags,
                                     hint, prot, f, fd, offset, mmap_flags);
    file_put(f);
    return r;
  }

  // FD_DEV (DRM GEM, framebuffer, char devices) is mapped via f_op->mmap /
  // dev_ops->mmap in sys_mmap's existing MAP_SHARED+fd path — it is NOT a
  // page-cache file mapping. Fall through to that path.
  if (f->type == FD_DEV) {
    file_put(f);
    return (int64_t)-ENOSYS; // sentinel: caller falls through (lock held)
  }

  // Only FD_REGULAR (ordinary files backed by the page cache) take the
  // demand-fault path below. Any other fd type is an invalid file mapping.
  if (f->type != FD_REGULAR) {
    file_put(f);
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EINVAL;
  }

  struct inode *ip = f->inode;
  if (!ip) {
    file_put(f);
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EBADF;
  }
  inode_get(ip); // region reference — survives close(fd)

  // offset alignment (MAP_FIXED already checked in sys_mmap; re-check here for
  // the non-fixed path too, matching Linux's -EINVAL on unaligned file offset).
  if (offset & (PAGE_SIZE - 1)) {
    inode_put(ip);
    file_put(f);
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EINVAL;
  }

  // The mapping may extend past EOF within its last page: a short file (e.g. a
  // 5-byte config) mmap'd into a whole 4KB page must succeed — page_cache_fill
  // zero-fills the on-disk tail, and file_fault_handler zeroes the sub-EOF tail
  // of the private copy. Only refuse when the mapping starts past EOF entirely
  // (no file bytes are covered at all — neither useful nor what callers
  // expect). Linux's full past-EOF semantics (zero pages beyond EOF + SIGBUS on
  // write past EOF) remain deferred to todo.
  if (offset >= ip->size && ip->size != 0) {
    inode_put(ip);
    file_put(f);
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return -EINVAL;
  }

  int64_t err = 0;
  mmap_region *region = mmap_place_file_region(
      proc, pml4, addr, size, (uint32_t)flags, hint, prot, fd, offset, &err);
  if (!region) {
    inode_put(ip);
    file_put(f);
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return err;
  }
  region->inode = ip;

  file_put(f); // region holds the inode reference; fd may be closed now
  spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
  return (int64_t)region->vaddr;
}

// ===================== BSD syscall: mmap =====================
int64_t sys_mmap(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                 int64_t arg5, int64_t arg6) {
  uint64_t addr = (uint64_t)arg1; // addr hint (or exact addr with MAP_FIXED)
  size_t size = (size_t)arg2;
  uint32_t prot = (uint32_t)arg3;
  int flags = (int)arg4;
  int fd = (int)arg5;
  uint64_t offset = arg6;

  if (size == 0 && ((flags & MAP_SHARED) == 0 || fd < 0))
    return -EINVAL;
  // Reject genuinely-unknown flag bits; recognized-but-unsupported bits
  // (HUGETLB/POPULATE/STACK/LOCKED/NORESERVE/GROWSDOWN/GROWSUP) fall through
  // as no-op. No hard size cap — the natural limit is bfc_alloc_page / address
  // space failure → -ENOMEM (RLIMIT_AS is a future todo; the OS has no rlimit).
  if ((flags & ~MAP_KNOWN_FLAGS) != 0)
    return -EINVAL;
  size = ALIGN_UP(size, PAGE_SIZE);

  bool fixed = (flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE);
  // MAP_FIXED / MAP_FIXED_NOREPLACE require a page-aligned addr (and offset for
  // fd mappings); a non-aligned hint (no MAP_FIXED) is rounded down, never an
  // error.
  if (fixed) {
    if (addr & (PAGE_SIZE - 1))
      return -EINVAL;
    if (fd >= 0 && (offset & (PAGE_SIZE - 1)))
      return -EINVAL;
  }
  uint64_t hint = (!fixed && addr) ? ALIGN_DOWN(addr, PAGE_SIZE) : 0;

  xtask *proc = current_task;
  uint64_t mmap_flags;
  spin_lock_irqsave(&proc->mm->mmap_lock, &mmap_flags);
  printk(LOG_DEBUG,
         "sys_mmap: pid=%d addr=0x%lx size=%zu flags=%d fd=%d "
         "offset=%llu\n",
         proc->pid, (unsigned long)addr, size, flags, fd,
         (unsigned long long)offset);
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

  // S12: file-backed mmap. fd >= 0 without MAP_ANONYMOUS and with an explicit
  // MAP_PRIVATE/MAP_SHARED is a file (or memfd) mapping — route it to the
  // demand-fault path. MAP_ANONYMOUS (even with an fd passed) stays anonymous,
  // matching Linux. The helper returns -ENOSYS to fall through to the existing
  // SHM/DEV path for memfd MAP_SHARED (which keeps its shared page-list
  // mapping); every other return (vaddr or hard error) is final — the helper
  // unlocked. -ENOSYS is returned with mmap_lock STILL HELD and no file ref
  // kept, so the fall-through runs under the same lock acquisition as a normal
  // entry.
  if (fd >= 0 && !(flags & MAP_ANONYMOUS) &&
      (flags & (MAP_PRIVATE | MAP_SHARED))) {
    int64_t r = sys_mmap_file_backed(proc, pml4, addr, size, prot, flags, fd,
                                     offset, mmap_flags, hint);
    if (r != (int64_t)-ENOSYS)
      return r;
  }

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

          int64_t picked =
              vma_pick_addr(proc->mm, pml4, addr, size, (uint32_t)flags, hint);
          if (picked < 0) {
            shm_put(target_shm);
            file_put(f);
            spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
            return picked;
          }
          uint64_t vaddr = (uint64_t)picked;
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
          region->fd = fd;
          region->offset = offset;
          region->flags = (uint32_t)flags;
          region->inode = NULL;
          region->shm_private_src = NULL;
          region->next = NULL;
          vma_insert_sorted(proc->mm, region);
          if (vaddr == proc->mm->mmap_brk)
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
    // Take a reference for the new region up front, before vma_pick_addr may
    // (under MAP_FIXED) vma_unmap_range the existing mappings of this same shm
    // object. Those releases shm_put the old regions; without this get, the
    // last one would drop refs to 0 and free shm while f->shm still points at
    // it — then region->shm_obj = shm_get(shm) below would be a UAF. The
    // region adopts this reference (no second shm_get on success).
    shm_get(shm);

    size_t npages = shm->npages;
    size_t list_pages = shm->page_list ? (size_t)shm->num_pages : 0;
    size_t total_pages = npages + list_pages;
    size = total_pages * PAGE_SIZE;

    int64_t picked =
        vma_pick_addr(proc->mm, pml4, addr, size, (uint32_t)flags, hint);
    if (picked < 0) {
      shm_put(shm);
      file_put(f);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return picked;
    }
    uint64_t vaddr = (uint64_t)picked;
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
        shm_put(shm);
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
      shm_put(shm);
      file_put(f);
      spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
      return -ENOMEM;
    }

    region->vaddr = vaddr;
    region->size = size;
    region->phys = 0;
    region->shm_obj = shm;
    region->fd = fd;
    region->offset = offset;
    region->flags = (uint32_t)flags;
    region->inode = NULL;
    region->shm_private_src = NULL;
    region->next = NULL;
    vma_insert_sorted(proc->mm, region);
    if (vaddr == proc->mm->mmap_brk)
      proc->mm->mmap_brk = vaddr + size;

    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return vaddr;
  }

  // MAP_PHYSICAL
  if (flags & MAP_PHYSICAL) {
    // OS-internal MMIO mapping (no Linux equivalent): always bump-allocates
    // from mmap_phys_brk and ignores MAP_FIXED/MAP_FIXED_NOREPLACE. Recorded
    // in doc/design/todo.md.
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))
      printk(LOG_DEBUG,
             "mmap: MAP_FIXED* ignored on MAP_PHYSICAL (OS-internal)\n");
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
    region->fd = -1;
    region->offset = offset;
    region->flags = (uint32_t)flags;
    region->inode = NULL;
    region->shm_private_src = NULL;
    region->next = NULL;
    vma_insert_sorted(proc->mm, region);
    proc->mm->mmap_phys_brk = vaddr + npages * PAGE_SIZE;

    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return vaddr;
  }

  // Anonymous private mapping
  int64_t picked =
      vma_pick_addr(proc->mm, pml4, addr, size, (uint32_t)flags, hint);
  if (picked < 0) {
    spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
    return picked;
  }
  uint64_t vaddr = (uint64_t)picked;
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
  region->fd = fd;
  region->offset = offset;
  region->flags = (uint32_t)flags;
  region->inode = NULL;
  region->shm_private_src = NULL;
  region->next = NULL;
  vma_insert_sorted(proc->mm, region);
  if (vaddr == proc->mm->mmap_brk)
    proc->mm->mmap_brk = vaddr + size;

  kfree(phys_pages);
  spin_unlock_irqrestore(&proc->mm->mmap_lock, mmap_flags);
  return vaddr;
}

// ===================== BSD syscall: munmap =====================
// Unmap every mapping overlapping [addr, addr+size): a region fully inside the
// interval is dropped, a partially-overlapping one is split (front/tail
// residue kept), matching Linux munmap(2). addr/size are page-aligned; an
// empty interval (no mapping) is silently a no-op success. Delegates the
// list/PTE/release work to vma_unmap_range (shared with MAP_FIXED), then
// tries to merge any same-prot residue left adjacent by a hole punch.
int64_t sys_munmap(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  uint64_t addr = arg1;
  size_t size = (size_t)arg2;

  if (size == 0)
    return (int64_t)-EINVAL;
  if (addr & (PAGE_SIZE - 1))
    return (int64_t)-EINVAL;
  size = ALIGN_UP(size, PAGE_SIZE);
  if (size == 0)
    return (int64_t)-EINVAL; // ALIGN_UP overflow

  xtask *proc = current_task;
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

  uint64_t flags;
  spin_lock_irqsave(&proc->mm->mmap_lock, &flags);
  int r = vma_unmap_range(proc->mm, pml4, addr, size);
  if (r == 0) {
    // A hole punch leaves residues on either side. They are normally separated
    // by the unmapped gap, so vma_merge is a no-op; it only joins residues
    // that end up adjacent (e.g. a cross-region unmap whose leftovers touch).
    mmap_region *prev = (addr) ? vma_find(proc->mm, addr - 1) : NULL;
    if (prev)
      vma_merge(proc->mm, prev);
    mmap_region *next = vma_find(proc->mm, addr + size);
    if (next)
      vma_merge(proc->mm, next);
  }
  spin_unlock_irqrestore(&proc->mm->mmap_lock, flags);
  return (int64_t)r;
}

// ===================== BSD syscall: mremap =====================
// Resize (and optionally relocate) a mapping. Linux mremap signature:
//   mremap(old_addr, old_size, new_size, flags, new_addr)
//   flags: MREMAP_MAYMOVE | MREMAP_FIXED (new_addr used only with FIXED).
//
// Scope: operates on a *whole* region — old_addr must equal region->vaddr and
// old_size must equal region->size (musl always mremaps whole regions: malloc
// heap grow/shrink at the region base; pthread_getattr_np probes mid-stack,
// which is not a region start → -EFAULT, exiting its probe loop cleanly with
// errno≠ENOMEM, same as the old stub). No sub-region mremap.
//
// Backing-type handling:
//  - anonymous:        shrink / grow-in-place / move (+grow) fully supported.
//  - file-backed (inode) / memfd MAP_PRIVATE (shm_private_src): shrink and move
//    supported; grow leaves new pages to demand fault-in. Moving relocates
//    already-faulted private pages.
//  - SHM shared / MAP_PHYSICAL: shrink and same-size move supported (PTEs are
//    relocated without refcount churn). Grow is rejected (-ENOMEM): the backing
//    extent is fixed/external. Callers fall back (musl malloc → copy_realloc).
//
// PROT_NONE pages (PTE_PROTNONE) are relocated like any present PTE so the
// protection is preserved at the new VA. Never-faulted file pages are skipped
// (no leaf PTE); fault-in works off region metadata.
//
// musl call sites:
//  - malloc.c:407  __mremap(base, oldlen, newlen, MREMAP_MAYMOVE) — realloc
//    fast path; failure (MAP_FAILED) falls to copy_realloc, so an mremap
//    that cannot place the new size must return -ENOMEM (not -ENOSYS).
//  - pthread_getattr_np.c:19  mremap(p-l-PAGE_SIZE, PAGE_SIZE, 2*PAGE_SIZE, 0)
//    — no MAYMOVE: in-place grow only; -EFAULT (mid-region) ends the loop
//    without the infinite retry that -ENOMEM would cause.

// Build the leaf-PTE flag bits for an anonymous page from PROT_* (mirrors the
// sys_mmap anon path at ~line 1356).
static uint64_t prot_to_pte_flags(uint32_t prot) {
  uint64_t f = PTE_USER;
  if (prot == 0)
    return f | PTE_PROTNONE | PTE_NX;
  f |= PTE_PRESENT;
  if (prot & PROT_WRITE)
    f |= PTE_RW;
  if (!(prot & PROT_EXEC))
    f |= PTE_NX;
  return f;
}

// Map the growth portion [base+old_npages*P, base+new_npages*P) for region r at
// virtual base `base` (the region's current or destination vaddr). Only touches
// the new tail; existing pages are the caller's concern (in-place: untouched;
// move: relocated by move_user_pages first). Returns 0 or -errno.
//  - anon: allocate zero pages with the region's prot-derived PTE flags.
//  - inode / shm_private_src: no-op (demand fault-in fills them).
//  - shm_obj / phys: -ENOMEM (fixed/external backing; no grow).
static int grow_region_pages(xtask *proc, uint64_t *pml4, mmap_region *r,
                             uint64_t base, size_t old_npages,
                             size_t new_npages) {
  if (new_npages <= old_npages)
    return 0;
  if (r->shm_obj || r->phys)
    return -ENOMEM;
  if (r->inode || r->shm_private_src)
    return 0; // fault-in

  uint64_t gstart = base + old_npages * PAGE_SIZE;
  size_t grow = new_npages - old_npages;
  int mapped = 0;
  if (!map_user_pages(pml4, gstart, gstart + grow * PAGE_SIZE,
                      prot_to_pte_flags(r->prot), &mapped)) {
    if (mapped)
      unmap_user_pages(pml4, gstart, gstart + mapped * PAGE_SIZE, mapped);
    return -ENOMEM;
  }
  return 0;
}

// Free the old-VA tail [base+keep_npages*P, base+old_npages*P) after a move
// that shrank the mapping (the moved prefix [0,keep) was already cleared by
// move_user_pages). SHM/MAP_PHYSICAL: clear PTEs only (externally-owned pages
// not freed). anon / file-private: unmap_user_pages frees + refcount-decs the
// private pages.
static void free_old_tail(uint64_t *pml4, mmap_region *r, uint64_t base,
                          size_t keep_npages, size_t old_npages) {
  for (size_t i = keep_npages; i < old_npages; i++) {
    uint64_t va = base + i * PAGE_SIZE;
    if (r->shm_obj || r->phys)
      clear_user_pte(pml4, va);
    else
      unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
  }
}

int64_t sys_mremap(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                   int64_t arg5, int64_t unused) {
  (void)unused;
  uint64_t old_addr = arg1;
  uint64_t old_size = arg2;
  uint64_t new_size = arg3;
  uint32_t flags = (uint32_t)arg4;
  uint64_t new_addr = arg5;

  if (old_addr & (PAGE_SIZE - 1))
    return (int64_t)-EINVAL;
  if (flags & ~((uint32_t)MREMAP_MAYMOVE | (uint32_t)MREMAP_FIXED))
    return (int64_t)-EINVAL;
  if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
    return (int64_t)-EINVAL;
  if (new_size == 0)
    return (int64_t)-EINVAL;
  if ((flags & MREMAP_FIXED) && (new_addr & (PAGE_SIZE - 1)))
    return (int64_t)-EINVAL;

  old_size = ALIGN_UP(old_size, PAGE_SIZE);
  new_size = ALIGN_UP(new_size, PAGE_SIZE);
  if (old_size == 0 || new_size == 0)
    return (int64_t)-EINVAL; // overflow

  xtask *proc = current_task;
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);

  uint64_t irqf;
  spin_lock_irqsave(&proc->mm->mmap_lock, &irqf);
  int64_t ret;

  mmap_region *r = vma_find(proc->mm, old_addr);
  // Whole-region contract: old_addr must be a region start and old_size its
  // full size. Sub-range mremap is not supported (musl never needs it).
  if (!r || r->vaddr != old_addr || r->size != old_size) {
    ret = -EFAULT;
    goto out;
  }

  size_t old_npages = old_size / PAGE_SIZE;
  size_t new_npages = new_size / PAGE_SIZE;
  uint64_t old_end = old_addr + old_size;
  uint64_t new_end = old_addr + new_size; // in-place candidate end

  // Decide in-place vs move. Shrink/same always in place. Grow in place only
  // if the extension is free and within the user VA bound.
  bool want_move = false;
  if (flags & MREMAP_FIXED)
    want_move = true;
  else if (new_size > old_size) {
    uint64_t grow = new_size - old_size;
    if (old_end + grow > USER_VMA_UPPER_BOUND ||
        vma_overlaps_any(proc->mm, old_end, grow))
      want_move = true;
  }

  if (!want_move) {
    // --- In-place resize ---
    if (new_size < old_size) {
      // Shrink: unmap the tail [old_addr+new_size, old_end). vma_unmap_range
      // splits the tail off (front residue [old_addr, old_addr+new_size)
      // stays) and frees its pages/refs per the region's backing type.
      int ur = vma_unmap_range(proc->mm, pml4, old_addr + new_size,
                               old_size - new_size);
      if (ur < 0) {
        ret = ur;
        goto out;
      }
    } else if (new_size > old_size) {
      int g =
          grow_region_pages(proc, pml4, r, old_addr, old_npages, new_npages);
      if (g < 0) {
        ret = g;
        goto out;
      }
      r->size = new_size;
      if (old_end == proc->mm->mmap_brk)
        proc->mm->mmap_brk = new_end;
    }
    ret = (int64_t)old_addr;
    goto out;
  }

  // --- Relocate ---
  uint64_t new_va;
  if (flags & MREMAP_FIXED) {
    new_va = new_addr;
    int ur = vma_unmap_range(proc->mm, pml4, new_va, new_size);
    if (ur < 0) {
      ret = ur;
      goto out;
    }
  } else {
    // MREMAP_MAYMOVE (no FIXED): musl passes new_addr=0 here. Treat it as a
    // hint; vma_find_gap falls back to the mmap_brk scan when the hint is not a
    // free gap.
    uint64_t gap = vma_find_gap(proc->mm, new_size, new_addr);
    if (!gap || vma_overlaps_any(proc->mm, gap, new_size)) {
      ret = -ENOMEM;
      goto out;
    }
    new_va = gap;
  }

  // 1. Grow at the destination first (may fail → nothing moved, r intact).
  if (new_size > old_size) {
    int g = grow_region_pages(proc, pml4, r, new_va, old_npages, new_npages);
    if (g < 0) {
      ret = g;
      goto out;
    }
  }
  // 2. Relocate existing pages old→new (two-phase; cannot fail mid-way — see
  //    move_user_pages). Relocates present PTEs only.
  if (!move_user_pages(pml4, old_addr, new_va, old_npages)) {
    // Only fails on destination page-table allocation. The growth pages mapped
    // in step 1 (if any) are orphaned; rare OOM, accepted.
    ret = -ENOMEM;
    goto out;
  }
  // 3. Free the old-VA tail when shrinking (the moved prefix was cleared by
  //    move_user_pages).
  if (new_size < old_size)
    free_old_tail(pml4, r, old_addr, new_npages, old_npages);
  // 4. Relink region metadata at the new VA.
  {
    mmap_region **pp = &proc->mm->mmap_regions;
    while (*pp != r)
      pp = &(*pp)->next;
    *pp = r->next;
    r->vaddr = new_va;
    r->size = new_size;
    r->next = NULL;
    // phys base is unchanged: a MAP_PHYSICAL mapping relocates the same phys
    // range to a new VA from offset 0 (only same-size move reaches here for
    // phys, since grow is rejected).
    if (vma_insert_sorted(proc->mm, r) != 0) {
      // Should not happen: new_va was a validated free gap. If it does, the
      // mapping is already moved at the PTE level; report failure.
      ret = -ENOMEM;
      goto out;
    }
    if (new_va == proc->mm->mmap_brk)
      proc->mm->mmap_brk = new_va + new_size;
  }
  ret = (int64_t)new_va;

out:
  spin_unlock_irqrestore(&proc->mm->mmap_lock, irqf);
  return ret;
}

// ===================== BSD syscall: mprotect =====================
// Change protection of an existing user mapping interval [addr, addr+size).
// Aligns with Linux: PROT_NONE clears PTE_PRESENT and sets PTE_PROTNONE
// (hardware not-present, but pte_present() recognizes it). Region metadata is
// split via vma_protect_range so a sub-interval change records its own prot
// (fork COW honors it). PROT_GROWSDOWN/PROT_SEM are masked to no-op (the OS
// has no stack-grow/sem-page semantics). Partial-unmapped → -ENOMEM,
// already-changed pages kept (Linux semantics). Does not split huge pages
// (→ -EINVAL). Metadata is committed first, PTEs second, so a split failure
// leaves the user-visible protection unchanged.
int64_t sys_mprotect(int64_t arg1, int64_t arg2, int64_t prot_arg,
                     int64_t unused1, int64_t unused2, int64_t unused3) {
  uint64_t addr = arg1;
  size_t size = (size_t)arg2;
  int prot = (int)prot_arg;

  if (size == 0)
    return 0; // Linux: no-op
  if (addr & (PAGE_SIZE - 1))
    return (int64_t)-EINVAL;
  // Mask off flags the OS treats as no-op (PROT_GROWSDOWN/PROT_SEM); reject
  // anything else beyond RWE. PROT_GROWSUP is an mmap-only flag — leaving it
  // unmasked so it is rejected matches Linux mprotect(2).
  uint32_t prot_masked = (uint32_t)prot & ~(PROT_GROWSDOWN | PROT_SEM);
  if (prot_masked & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return (int64_t)-EINVAL;
  if (addr >= 0x800000000000ULL)
    return (int64_t)-EINVAL;

  size = ALIGN_UP(size, PAGE_SIZE);

  xtask *proc = current_task;
  spinlock *lk = &proc->mm->mmap_lock;
  uint64_t flags;
  spin_lock_irqsave(lk, &flags);

  // (1) Commit region metadata first: split out the [addr, addr+size) piece
  // and set its prot. A failure here (-ENOMEM on split) returns before any
  // PTE is touched, so the user-visible protection is unchanged.
  int r = vma_protect_range(proc->mm, addr, size, prot_masked);
  if (r < 0) {
    spin_unlock_irqrestore(lk, flags);
    return (int64_t)r;
  }

  // (2) New leaf flags (PTE_USER always; physical address preserved per-page).
  uint64_t base = PTE_USER;
  if (prot_masked == 0) {
    // PROT_NONE: present=0 + PROTNONE + NX
    base |= PTE_PROTNONE | PTE_NX;
  } else {
    base |= PTE_PRESENT;
    if (prot_masked & PROT_WRITE)
      base |= PTE_RW;
    if (!(prot_masked & PROT_EXEC))
      base |= PTE_NX;
  }

  size_t npages = size / PAGE_SIZE;
  for (size_t i = 0; i < npages; i++) {
    uint64_t va = addr + i * PAGE_SIZE;
    uint64_t *pte = lookup_pte(proc->mm->cr3, va);
    if (!pte) {
      // Page genuinely unmapped → -ENOMEM, already-changed pages kept (Linux).
      // Metadata was already split above; the unmapped hole carries the new
      // prot but has no PTE, which is harmless (no page to fault against until
      // something maps it).
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
// 公共实现：flags 仅接受 O_CLOEXEC（per-fd bitmap）| O_NONBLOCK（f->flags）。
// sys_pipe → do_pipe(fd_ptr, 0)；sys_pipe2 传入用户 flags。
static int64_t do_pipe(int __user *fd_ptr, int flags) {
  if (flags & ~(O_CLOEXEC | O_NONBLOCK))
    return (int64_t)-EINVAL;

  uint64_t ptr = (__force uint64_t)fd_ptr;
  if (!ptr || ptr >= KERNEL_VMA_BOUNDARY ||
      ptr + 2 * sizeof(int) > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;

  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int read_fd = alloc_fd(proc->proc->files, 0);
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
  p->size = PIPE_BUF_SIZE;
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

  // pipe2 flags：仍在 fd_lock 内设置，对 execve 的 cloexec 扫描原子。
  if (flags & O_NONBLOCK) {
    fr->flags |= O_NONBLOCK;
    fw->flags |= O_NONBLOCK;
  }
  if (flags & O_CLOEXEC) {
    fd_set_cloexec(proc->proc->files, read_fd, 1);
    fd_set_cloexec(proc->proc->files, write_fd, 1);
  }

  int fd_pair[2] = {read_fd, write_fd};
  if (copy_to_user(fd_ptr, fd_pair, sizeof(fd_pair))) {
    struct file *f_r = fd_uninstall(proc->proc->files, read_fd);
    struct file *f_w = fd_uninstall(proc->proc->files, write_fd);
    fd_set_cloexec(proc->proc->files, read_fd, 0);
    fd_set_cloexec(proc->proc->files, write_fd, 0);
    spin_unlock(fdlk);
    synchronize_rcu();
    file_put(f_r);
    file_put(f_w);
    return (int64_t)-EFAULT;
  }

  spin_unlock(fdlk);
  return 0;
}

int64_t sys_pipe(int64_t arg1, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4, int64_t unused5) {
  return do_pipe((int __user *__force)arg1, 0);
}

int64_t sys_pipe2(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4) {
  return do_pipe((int __user *__force)arg1, (int)arg2);
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
    ret = unix_sock_write(sock, (const void __force *)buf, len, 0);
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
      // All read ends closed: POSIX raises SIGPIPE on pipe write by default.
      // Use the normal delivery path (not force_sig) so an explicit SIG_IGN or
      // blocked SIGPIPE is honored — write still returns -EPIPE either way.
      // force_sig would reset SIG_IGN→SIG_DFL, defeating
      // signal(SIGPIPE,SIG_IGN).
      deliver_signal_to(current_task, SIGPIPE);
      ret = -EPIPE;
      goto out;
    }
    if ((p->head + 1) % p->size == p->tail) {
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
      wait.exclusive = 0;
      list_init(&wait.node);
      add_wait_queue(wq, &wait);
      for (;;) {
        proc->state = BLOCKED;
        proc->wait_event = WAIT_NONE;
        if ((p->head + 1) % p->size != p->tail)
          break; /* 有空间 */
        if (f->flags & O_NONBLOCK) {
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(wq, &wait);
          if (written > 0)
            break;
          ret = -EAGAIN;
          goto out;
        }
        /* Borrow the process alarm deadline (if armed) as the wake deadline so
         * a pending SIGALRM can interrupt this indefinite blocking write —
         * mirrors sys_pause / epoll_wait. Re-read each iteration: a prior block
         * may have returned on alarm and the process may re-arm a new alarm. No
         * user timeout exists for write(); with no alarm we block indefinitely
         * (wait_deadline=0, not inserted) until space frees or readers close.
         */
        uint64_t alarm_dl = 0;
        if (proc->proc && proc->proc->signal) {
          uint64_t sflags;
          spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
          alarm_dl = proc->proc->signal->alarm_deadline;
          spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
        }
        if (alarm_dl != 0) {
          proc->wait_deadline = alarm_dl;
          int cpu = proc->assigned_cpu;
          uint64_t flags;
          spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
          sched_timer_queue_insert(cpu, proc);
          spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
        } else {
          proc->wait_deadline = 0;
        }
        schedule();
        {
          /* Merge shared_pending so kill()-delivered signals interrupt a
           * blocking pipe write (see pipe read). */
          if (signal_pending(proc)) {
            sched_cancel_spurious_wake(proc);
            remove_wait_queue(wq, &wait);
            if (written > 0)
              break;
            ret = -ERESTART;
            goto out;
          }
        }
      }
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(wq, &wait);
      continue; // 回外层 while 重查（含 p_count<=1 的 EPIPE）
    }
    p->buf[p->head] = ((const char __force *)buf)[written];
    p->head = (p->head + 1) % p->size;
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
    ret = unix_sock_read(sock, (void __force *)buf, len, 0);
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
    wait.exclusive = 0;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
    for (;;) {
      proc->state = BLOCKED;
      proc->wait_event = WAIT_NONE;
      /* 有数据优先于 EOF：写端关闭前已入队的字节必须先读出（POSIX drain
       * 语义）。原顺序先查 p_count==1，write+close 紧挨着执行时 reader 醒来
       * 会无视缓冲区的数据直接返回 0（flaky socket_msgflags Case A）。 */
      if (p->head != p->tail)
        break; /* 有数据 */
      if (refcount_read(&p->p_count) == 1) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(wq, &wait);
        ret = 0;
        goto out;
      }
      if (f->flags & O_NONBLOCK) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(wq, &wait);
        ret = -EAGAIN;
        goto out;
      }
      /* Borrow the process alarm deadline (if armed) as the wake deadline so a
       * pending SIGALRM can interrupt this indefinite blocking read — mirrors
       * sys_pause / epoll_wait. No user timeout exists for read(); a deadline
       * is armed only when a process alarm is set, so the timer queue wakes us
       * on alarm expiry and the EINTR check below returns -EINTR. With no
       * alarm we block indefinitely (wait_deadline=0, not inserted) until data
       * arrives or the pipe closes. */
      uint64_t alarm_dl = 0;
      if (proc->proc && proc->proc->signal) {
        uint64_t sflags;
        spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
        alarm_dl = proc->proc->signal->alarm_deadline;
        spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
      }
      if (alarm_dl != 0) {
        proc->wait_deadline = alarm_dl;
        int cpu = proc->assigned_cpu;
        uint64_t flags;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
        sched_timer_queue_insert(cpu, proc);
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
      } else {
        proc->wait_deadline = 0;
      }
      schedule();
      {
        /* Merge shared_pending so kill()-delivered signals interrupt a
         * blocking pipe read (signal_pending merges sig_pending +
         * shared_pending under sig_lock). */
        if (signal_pending(proc)) {
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(wq, &wait);
          ret = -ERESTART;
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
      p->tail = (p->tail + 1) % p->size;
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
  // S06: clear the cloexec bitmap entry so the recycled slot does not leak a
  // stale bit into a later fd that reuses it.
  fd_set_cloexec(current_proc->files, fd, 0);
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
  // S06: dup2 never sets cloexec on the new fd (POSIX); clear the reused
  // slot's bitmap bit so a stale value from the closed victim does not leak.
  fd_set_cloexec(proc->proc->files, new_fd, 0);

  spin_unlock(fdlk);
  if (victim) {
    synchronize_rcu();
    file_put(victim);
  }
  return (int64_t)new_fd;
}

// ===================== BSD syscall: fcntl =====================

// Round v up to the next power of two (v already a power of two is unchanged).
// Used by F_SETPIPE_SZ to clamp the requested pipe capacity to a power-of-two
// ring size in [PAGE_SIZE, PIPE_MAX_SIZE].
static uint32_t round_up_pow2(uint32_t v) {
  if (v <= 1)
    return 1;
  return 1u << (32 - __builtin_clz(v - 1));
}

int64_t sys_flock(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  int fd = (int)arg1;
  int operation = (int)arg2;

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

  int64_t ret = do_flock(f, operation);
  file_put(f);
  return ret;
}

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
    // S06: cloexec is per-fd (bitmap), not on the shared file. Report the
    // POSIX userspace bit (1) for this fd's bitmap entry.
    ret = fd_get_cloexec(proc->proc->files, fd) ? 1 : 0;
    goto out;
  case F_SETFD:
    // POSIX userspace FD_CLOEXEC == 1; any other bits are ignored. Toggle only
    // the current fd's bitmap bit (a dup'd fd sharing this file is unaffected).
    fd_set_cloexec(proc->proc->files, fd, arg & 1);
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
    // S06: only the NEW fd gets cloexec; the original fd (and any other dup)
    // is untouched. The shared file's flags no longer carry cloexec.
    if (cmd == F_DUPFD_CLOEXEC)
      fd_set_cloexec(proc->proc->files, new_fd, 1);
    else
      fd_set_cloexec(proc->proc->files, new_fd, 0);
    spin_unlock(fdlk);
    ret = (int64_t)new_fd;
    goto out;
  }
  case F_GETPIPE_SZ: {
    if (f->type != FD_PIPE || !f->pipe) {
      ret = -EINVAL;
      goto out;
    }
    ret = (int64_t)f->pipe->size;
    goto out;
  }
  case F_SETPIPE_SZ: {
    if (f->type != FD_PIPE || !f->pipe) {
      ret = -EINVAL;
      goto out;
    }
    struct pipe *p = f->pipe;
    uint32_t new_size = (arg == 0) ? PAGE_SIZE : (uint32_t)arg;
    new_size = round_up_pow2(new_size);
    if (new_size < PAGE_SIZE)
      new_size = PAGE_SIZE;
    if (new_size > PIPE_MAX_SIZE) {
      ret = -EINVAL;
      goto out;
    }
    /* Refuse to shrink below the data currently in the ring (Linux -EBUSY). */
    uint32_t used = (p->head + p->size - p->tail) % p->size;
    if (new_size <= used) {
      ret = -EBUSY;
      goto out;
    }
    uint8_t *new_buf = (uint8_t *)krealloc(p->buf, new_size);
    if (!new_buf) {
      ret = -ENOMEM;
      goto out;
    }
    p->buf = new_buf;
    p->size = new_size;
    /* head/tail are unchanged: ring math uses the new modulus, and since
     * new_size > used the existing [tail, head) data still fits. Wake any
     * blocked writer — a larger pipe may now have room. */
    __wake_up(p->wq, POLLOUT);
    ret = (int64_t)new_size;
    goto out;
  }
  case F_SETOWN: {
    /* Store the SIGIO recipient pid. This OS has no async-I/O completion path,
     * so no SIGIO is ever delivered — the value is recorded for F_GETOWN only.
     * Only positive pids are accepted (no process-group -pgid yet). */
    if (arg <= 0) {
      ret = -EINVAL;
      goto out;
    }
    f->f_owner = (pid_t)arg;
    f->f_owner_type = F_OWNER_PID;
    ret = 0;
    goto out;
  }
  case F_GETOWN:
    ret = (int64_t)f->f_owner;
    goto out;
  case F_SETOWN_EX: {
    /* F_SETOWN_EX extends F_SETOWN with a recipient class (TID/PID/PGRP).
     * Stored only (no SIGIO delivery path, same as F_SETOWN/F_SETSIG) so
     * F_GETOWN_EX round-trips it. F_OWNER_PGRP stores a negative pgid in
     * f_owner (matching legacy F_SETOWN's -pgid convention); read back as
     * positive via F_GETOWN_EX. */
    struct f_owner_ex ex;
    if (copy_from_user(&ex, (void __user *)(uintptr_t)arg3, sizeof(ex))) {
      ret = -EFAULT;
      goto out;
    }
    if (ex.type != F_OWNER_TID && ex.type != F_OWNER_PID &&
        ex.type != F_OWNER_PGRP) {
      ret = -EINVAL;
      goto out;
    }
    f->f_owner_type = ex.type;
    f->f_owner = (ex.type == F_OWNER_PGRP) ? -(pid_t)ex.pid : ex.pid;
    ret = 0;
    goto out;
  }
  case F_GETOWN_EX: {
    struct f_owner_ex ex;
    ex.type = f->f_owner_type;
    ex.pid = (f->f_owner_type == F_OWNER_PGRP) ? -f->f_owner : f->f_owner;
    if (copy_to_user((void __user *)(uintptr_t)arg3, &ex, sizeof(ex)))
      ret = -EFAULT;
    else
      ret = 0;
    goto out;
  }
  case F_SETSIG: {
    /* 0 restores the default SIGIO; otherwise [1, NSIG). Stored only. */
    if (arg < 0 || arg >= NSIG) {
      ret = -EINVAL;
      goto out;
    }
    f->f_owner_sig = arg;
    ret = 0;
    goto out;
  }
  case F_GETSIG:
    ret = (int64_t)f->f_owner_sig;
    goto out;
  case F_OFD_GETLK:
  case F_OFD_SETLK:
  case F_OFD_SETLKW: {
    /* OFD locks are owned by the open file description, not the process.
     * Only regular files carry byte-range locks. */
    if (f->type != FD_REGULAR || !f->inode) {
      ret = -ENOLCK;
      goto out;
    }
    struct flock ofd_lk;
    if (copy_from_user(&ofd_lk, (void __user *)(uintptr_t)arg3,
                       sizeof(ofd_lk))) {
      ret = -EFAULT;
      goto out;
    }
    ret = do_fcntl_lock_ofd(proc, f, cmd, &ofd_lk);
    if (cmd == F_OFD_GETLK && ret == 0) {
      if (copy_to_user((void __user *)(uintptr_t)arg3, &ofd_lk, sizeof(ofd_lk)))
        ret = -EFAULT;
    }
    goto out;
  }
  case F_GETLK:
  case F_SETLK:
  case F_SETLKW: {
    if (f->type != FD_REGULAR || !f->inode) {
      /* pipe/socket/dev/shm/dir cannot carry POSIX file locks. */
      ret = -ENOLCK;
      goto out;
    }
    struct flock lk;
    if (copy_from_user(&lk, (void __user *)(uintptr_t)arg3, sizeof(lk))) {
      ret = -EFAULT;
      goto out;
    }
    ret = do_fcntl_lock(proc, f, cmd, &lk);
    if (cmd == F_GETLK && ret == 0) {
      if (copy_to_user((void __user *)(uintptr_t)arg3, &lk, sizeof(lk)))
        ret = -EFAULT;
    }
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
/* §3.3 do_symlinkat:symlinkat 的共同实现(内核串)。ktarget/klink 已 copy_
 * from_user。解析 linkpath 的父目录 + 末段名,调 dir->i_op->symlink;FAT32
 * (symlink==NULL) → -ENOSYS。目标已存在 → -EEXIST。 */
static int do_symlinkat(const char *ktarget, int newdirfd, const char *klink) {
  struct inode *dir;
  char lname[256];
  int err;
  if (klink[0] == '/') {
    char relpath[256];
    struct mount_entry *m = vfs_resolve(klink, relpath, sizeof(relpath));
    if (!m)
      return -ENOENT;
    err = path_walk_parent(m, relpath, &dir, lname, sizeof(lname));
  } else {
    struct inode *start = resolve_dirfd_start(newdirfd);
    if (IS_ERR(start))
      return (int)PTR_ERR(start);
    char relpath[256];
    if (normalize_path(klink, relpath, sizeof(relpath)) < 0) {
      inode_put(start);
      return -ENAMETOOLONG;
    }
    err = path_walk_parent_from(start, relpath, &dir, lname, sizeof(lname));
    inode_put(start);
  }
  if (err) {
    if (dir)
      inode_put(dir);
    return err;
  }
  if (!dir->i_op || !dir->i_op->symlink) {
    inode_put(dir);
    return -ENOSYS; /* FAT32 物理不支持 symlink */
  }
  /* 目标已存在 → EEXIST。lookup 返 +1 引用,须 put 平衡。 */
  struct inode *exist = dir->i_op->lookup(dir, lname);
  if (exist) {
    inode_put(exist);
    inode_put(dir);
    return -EEXIST;
  }
  struct inode *ip = dir->i_op->symlink(dir, lname, ktarget); /* i_count=2 */
  inode_put(dir);
  if (IS_ERR(ip))
    return (int)PTR_ERR(ip);
  inode_put(ip); /* 平衡 symlink 出口的 +1 返回引用,目录项留 1 */
  return 0;
}

/* §3.3 do_readlinkat:readlinkat 的共同实现(内核串)。kpath 已 copy_from_user。
 * 不跟随末段 symlink(readlink 语义:取 link inode 本身)。INODE_LNK 校验 +
 * readlink 钩子;拷出 target 到内核 kbuf 再 copy_to_user 截断 bufsiz(Linux
 * 语义:返 min(实际长度, bufsiz),不 NUL 终止)。 */
static int do_readlinkat(int dirfd, const char *kpath, char __user *ubuf,
                         size_t bufsiz) {
  if (bufsiz == 0)
    return -EINVAL; /* Linux:bufsiz==0 → EINVAL(非 POSIX,但 glibc 依赖) */
  struct inode *ip;
  if (kpath[0] == '/') {
    char relpath[256];
    struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
    if (!m)
      return -ENOENT;
    ip = path_walk(m, relpath); /* +1;path_walk 跟随中间段,末段 LNK 原样返回 */
  } else {
    struct inode *start = resolve_dirfd_start(dirfd);
    if (IS_ERR(start))
      return (int)PTR_ERR(start);
    char relpath[256];
    if (normalize_path(kpath, relpath, sizeof(relpath)) < 0) {
      inode_put(start);
      return -ENAMETOOLONG;
    }
    ip = path_walk_from(start, relpath); /* +1 */
    inode_put(start);
  }
  if (!ip)
    return -ENOENT;
  if (ip->type != INODE_LNK || !ip->i_op || !ip->i_op->readlink) {
    inode_put(ip);
    return -EINVAL; /* 非软链 → EINVAL(Linux readlink 语义) */
  }
  char kbuf[256];
  int n = ip->i_op->readlink(ip, kbuf, sizeof(kbuf));
  inode_put(ip);
  if (n < 0)
    return n;
  int wn = (n < (int)bufsiz) ? n : (int)bufsiz; /* 截断返 bufsiz(Linux 语义) */
  if (copy_to_user(ubuf, kbuf, wn))
    return -EFAULT;
  return wn;
}

/* §3.4 do_linkat:linkat 的共同实现(内核串)。kold/knew 已 copy_from_user。
 * 解析 old 的目标 inode(follow 语义见下)+ new 的父目录 + 末段名,调
 * newdir->i_op->link;FAT32(link==NULL) → -EPERM。目标已存在 → EEXIST。
 *
 * Linux linkat flags:默认 0 = 跟随 old 的 symlink(若 old 是软链,链其目标);
 * AT_SYMLINK_FOLLOW(0x400) 显式跟随(与默认同);无 NOFOLLOW 位(linkat 不
 * 支持 AT_SYMLINK_NOFOLLOW,故 old 恒跟随)。本 OS 用 vfs_resolve 的 follow
 * 行为(path_walk 跟随中间段、末段由调用方定)——此处 old 取跟随末段的结果
 * (stat 语义),对齐 Linux link(默认跟随)。 */
static int do_linkat(int olddirfd, const char *kold, int newdirfd,
                     const char *knew, int flags) {
  if (flags & ~AT_SYMLINK_FOLLOW)
    return -EINVAL;
  /* 解析 old 目标 inode(+1,调用者 put)。绝对路径走 mount 表;相对走 dirfd。
   * follow=true:跟随末段 symlink(stat 语义,Linux link 默认)。
   * m_old:target 解析归属的 mount(绝对路径=vfs_resolve 的 m;相对路径=dirfd
   * 的 start 所在 mount,见下)。用于 VFS 层跨 fs EXDEV 判定(对齐 Linux
   * vfs_link:target->i_sb != newdir->i_sb → EXDEV),不依赖惰性 inode.mount
   * 字段(inode_create 初始化 NULL、仅 sys_open/stat 路径设值,fs 层比较误判)。
   */
  struct inode *target;
  struct mount_entry *m_old = NULL;
  if (kold[0] == '/') {
    char relpath[256];
    m_old = vfs_resolve(kold, relpath, sizeof(relpath));
    if (!m_old)
      return -ENOENT;
    target = path_walk(m_old, relpath); /* +1 */
  } else {
    struct inode *start = resolve_dirfd_start(olddirfd);
    if (IS_ERR(start))
      return (int)PTR_ERR(start);
    /* 相对路径 target 与 olddirfd 同 mount:dirfd 指向的目录 inode 归属的
     * mount 即 target 的 mount。优先用 inode.mount(若 dirfd 经 sys_open
     * 解析过已设);否则 fallback root mount("/"),与 mount_of_inode 语义一致。 */
    m_old = mount_of_inode(start);
    char relpath[256];
    if (normalize_path(kold, relpath, sizeof(relpath)) < 0) {
      inode_put(start);
      return -ENAMETOOLONG;
    }
    target = path_walk_from(start, relpath); /* +1 */
    inode_put(start);
  }
  if (!target) {
    return -ENOENT;
  }

  /* 解析 new 的父目录 + 末段名(不建末段,link 在父目录下加新名)。
   * m_new:new 归属 mount,与 m_old 同源取(绝对路径=vfs_resolve 的 m;相对路径
   * =dirfd start 所属 mount)。不依赖惰性 inode.mount(tmpfs 经 path_walk_parent
   * 取出的 newdir.mount 仍为 NULL,fallback 会误判为 root mount)。 */
  struct inode *newdir;
  char newname[256];
  struct mount_entry *m_new = NULL;
  int err;
  if (knew[0] == '/') {
    char relpath[256];
    m_new = vfs_resolve(knew, relpath, sizeof(relpath));
    if (!m_new) {
      inode_put(target);
      return -ENOENT;
    }
    err = path_walk_parent(m_new, relpath, &newdir, newname, sizeof(newname));
  } else {
    struct inode *start = resolve_dirfd_start(newdirfd);
    if (IS_ERR(start)) {
      inode_put(target);
      return (int)PTR_ERR(start);
    }
    m_new = mount_of_inode(start);
    char relpath[256];
    if (normalize_path(knew, relpath, sizeof(relpath)) < 0) {
      inode_put(start);
      inode_put(target);
      return -ENAMETOOLONG;
    }
    err = path_walk_parent_from(start, relpath, &newdir, newname,
                                sizeof(newname));
    inode_put(start);
  }
  if (err) {
    if (newdir)
      inode_put(newdir);
    inode_put(target);
    return err;
  }
  /* 跨 fs EXDEV 先于 EPERM 判定(对齐 Linux vfs_link:target->i_sb != dir->i_sb
   * → EXDEV 优先于 dir->i_op->link NULL 检查)。m_old/m_new 同源取自 mount 解析,
   * 不依赖惰性 inode.mount。 */
  if (m_old != m_new) {
    inode_put(newdir);
    inode_put(target);
    return -EXDEV;
  }
  if (!newdir->i_op || !newdir->i_op->link) {
    inode_put(newdir);
    inode_put(target);
    return -EPERM; /* FAT32 无硬链接 → EPERM(Linux fat 对 link 返 EPERM) */
  }
  err = newdir->i_op->link(newdir, target, newname);
  inode_put(newdir);
  inode_put(target);
  return err;
}

int64_t sys_link(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  /* link(old, new) = linkat(AT_FDCWD, old, AT_FDCWD, new, 0)。 */
  const char __user *uold = (const char __user *__force)a1;
  const char __user *unew = (const char __user *__force)a2;
  if (!uold || !unew)
    return -EFAULT;
  char kold[256], knew[256];
  if (strncpy_from_user(kold, uold, sizeof(kold)) < 0)
    return -EFAULT;
  if (strncpy_from_user(knew, unew, sizeof(knew)) < 0)
    return -EFAULT;
  return do_linkat(AT_FDCWD, kold, AT_FDCWD, knew, 0);
}
int64_t sys_symlink(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                    int64_t a6) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  /* symlink(target, linkpath) = symlinkat(target, AT_FDCWD, linkpath)。 */
  const char __user *utarget = (const char __user *__force)a1;
  const char __user *ulink = (const char __user *__force)a2;
  if (!utarget || !ulink)
    return -EFAULT;
  char ktarget[256];
  if (strncpy_from_user(ktarget, utarget, sizeof(ktarget)) < 0)
    return -EFAULT;
  char klink[256];
  if (strncpy_from_user(klink, ulink, sizeof(klink)) < 0)
    return -EFAULT;
  return do_symlinkat(ktarget, AT_FDCWD, klink);
}
int64_t sys_readlink(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;
  /* readlink(path, buf, bufsiz) = readlinkat(AT_FDCWD, path, buf, bufsiz)。 */
  const char __user *upath = (const char __user *__force)a1;
  char __user *ubuf = (char __user *__force)a2;
  size_t bufsiz = (size_t)a3;
  if (!upath || !ubuf)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_readlinkat(AT_FDCWD, kpath, ubuf, bufsiz);
}
/* ===================== chmod/fchmod/fchmodat =====================
 * do_utimensat/do_faccessat 模式:kpath 内核串(调用方已 strncpy_from_user),
 * flags 严格校验。落盘仅内存(与 utimensat Q5 一致:FAT32 不写 mode 到磁盘目录项,
 * inode 内存态 mode/uid/gid/ctime)。setuid 位清除规则是安全基石(apply_chmod)。
 */

/* resolve_path_or_fd:chmod/chown 共用路径解析。返 +1 inode(调用方 inode_put)
 * 或 ERR_PTR(-errno)/NULL。flags 含 AT_EMPTY_PATH 且 kpath 空 → fd 路径(照
 * vfs_fstat_fd vfs.c:725:rcu_read_lock→fd_lookup→file_get→rcu_read_unlock→
 * inode_get(f->inode)→file_put);否则 path_walk 解析,末段 symlink 默认跟随
 * (AT_SYMLINK_NOFOLLOW 时取 link 本身,照 vfs_statx vfs.c:785)。 */
static struct inode *resolve_path_or_fd(int dirfd, const char *kpath,
                                        int flags) {
  if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
    if (dirfd < 0)
      return ERR_PTR(-EBADF);
    xtask *proc = current_task;
    rcu_read_lock();
    struct file *f = fd_lookup(proc->proc->files, dirfd);
    if (!f) {
      rcu_read_unlock();
      return ERR_PTR(-EBADF);
    }
    file_get(f);
    rcu_read_unlock();
    struct inode *ip =
        f->inode ? inode_get(f->inode) : ERR_PTR(-EBADF); /* +1 */
    file_put(f);
    return ip;
  }

  struct inode *ip;
  if (kpath[0] == '/') {
    char relpath[256];
    struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
    if (!m)
      return ERR_PTR(-ENOENT);
    ip = path_walk(m, relpath); /* +1 */
  } else {
    struct inode *start = resolve_dirfd_start(dirfd);
    if (IS_ERR(start))
      return start;
    ip = path_walk_from(start, kpath); /* +1 */
    inode_put(start);
  }
  if (!ip)
    return ERR_PTR(-ENOENT);
  /* 末段 symlink 跟随:未设 AT_SYMLINK_NOFOLLOW 时跟随(chmod 默认作用于目标,
   * 非 link 本身)。中间段已由 path_walk 跟随。 */
  if (ip->type == INODE_LNK && !(flags & AT_SYMLINK_NOFOLLOW)) {
    int sym_depth = 0;
    struct inode *resolved = follow_symlink(ip, &sym_depth);
    inode_put(ip);
    return resolved; /* +1 或 ERR_PTR */
  }
  return ip; /* +1 */
}

/* update_ctime:写 inode ctime(改 mode/uid/gid 后)。dispatch i_op->update_time
 * 或 generic 回退(照 do_utimensat:3408)。仅 CTIME_BIT。 */
static int update_ctime(struct inode *ip) {
  uint64_t now =
      __atomic_load_n(&wall_clock_boot_ns, __ATOMIC_RELAXED) + sched_clock();
  if (ip->i_op && ip->i_op->update_time)
    return ip->i_op->update_time(ip, 0, 0, now, CTIME_BIT);
  return generic_update_time(ip, 0, 0, now, CTIME_BIT);
}

/* apply_chmod:持 i_lock 改 mode(保留 S_IFMT 文件类型位),非特权 chmod 清
 * setuid/setgid 位(对齐 Linux chmod_common)。锁序:仅持 i_lock(leaf lock,
 * 照 fat32 i_lock→fat_lock 序,i_lock 在内层),不碰 fat_lock/page_cache_lock。 */
static void apply_chmod(struct inode *ip, unsigned int new_mode) {
  spin_lock(&ip->i_lock);
  ip->mode = (ip->mode & S_IFMT) | (new_mode & 07777); /* 保留文件类型位 */
  if (!capable(CAP_FSETID) && S_ISREG(ip->mode))
    ip->mode &= ~(S_ISUID | S_ISGID); /* 非特权 chmod 必清 setuid 位 */
  if (!capable(CAP_FSETID) && S_ISDIR(ip->mode))
    ip->mode &= ~S_ISVTX;
  spin_unlock(&ip->i_lock);
}

/* do_fchmodat:chmod/fchmod/fchmodat 共同实现。flags 校验照 do_utimensat(接受
 * AT_SYMLINK_NOFOLLOW + AT_EMPTY_PATH)。权限:CAP_FOWNER 放行,否则 euid 须匹配
 * owner(对齐 Linux chmod_common + inode_owner_or_capable)。 */
static int do_fchmodat(int dirfd, const char *kpath, unsigned int mode,
                       int flags) {
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return -EINVAL;
  unsigned int new_mode =
      mode & 07777; /* 剥文件类型位,保留 S_ISUID/S_ISGID/S_ISVTX */

  struct inode *ip = resolve_path_or_fd(dirfd, kpath, flags);
  if (IS_ERR(ip))
    return (int)PTR_ERR(ip);
  if (!ip) {
    return -ENOENT;
  }

  int err = 0;
  if (!capable(CAP_FOWNER) && current_proc->euid != ip->uid) {
    err = -EPERM;
  } else {
    apply_chmod(ip, new_mode);
    err = update_ctime(ip);
  }
  inode_put(ip);
  return err;
}

int64_t sys_chmod(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  /* chmod(path, mode) = fchmodat(AT_FDCWD, path, mode, 0)。 */
  const char __user *upath = (const char __user *__force)a1;
  unsigned int mode = (unsigned int)a2;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_fchmodat(AT_FDCWD, kpath, mode, 0);
}
int64_t sys_fchmod(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  /* fchmod(fd, mode) = fchmodat(fd, "", mode, AT_EMPTY_PATH)。 */
  int fd = (int)a1;
  unsigned int mode = (unsigned int)a2;
  return do_fchmodat(fd, "", mode, AT_EMPTY_PATH);
}
/* ===================== chown/fchown/fchownat =====================
 * do_fchmodat 同模式:复用 resolve_path_or_fd/update_ctime。落盘仅内存(与
 * chmod/utimensat 一致)。权限简化为 CAP_CHOWN(root-only);Linux 复杂规则
 * (属主改 group 到自己所在 group)留 todo(单用户 root-default 不破坏现有测试)。
 */

/* apply_chown:持 i_lock 改 uid/gid((uid_t)-1/(gid_t)-1 = 不变),非特权 chown
 * 清 setuid/setgid 位(对齐 Linux chown_common)。锁序同 apply_chmod:仅 i_lock。
 */
static void apply_chown(struct inode *ip, unsigned int uid, unsigned int gid) {
  spin_lock(&ip->i_lock);
  if (uid != (unsigned int)-1)
    ip->uid = uid;
  if (gid != (unsigned int)-1)
    ip->gid = gid;
  if (!capable(CAP_FSETID) && S_ISREG(ip->mode))
    ip->mode &= ~(S_ISUID | S_ISGID); /* chown 改 owner 后非特权清 setuid 位 */
  spin_unlock(&ip->i_lock);
}

/* do_fchownat:chown/fchown/fchownat 共同实现。flags 校验同 chmod(接受
 * AT_SYMLINK_NOFOLLOW + AT_EMPTY_PATH)。(uid_t)-1/(gid_t)-1 =
 * 该字段不变(POSIX)。 */
static int do_fchownat(int dirfd, const char *kpath, unsigned int owner,
                       unsigned int group, int flags) {
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return -EINVAL;

  struct inode *ip = resolve_path_or_fd(dirfd, kpath, flags);
  if (IS_ERR(ip))
    return (int)PTR_ERR(ip);
  if (!ip) {
    return -ENOENT;
  }

  int err = 0;
  if (!capable(CAP_CHOWN)) {
    err = -EPERM; /* 简化为 root-only(对齐 plan 决策) */
  } else {
    apply_chown(ip, owner, group);
    err = update_ctime(ip);
  }
  inode_put(ip);
  return err;
}

int64_t sys_chown(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;
  /* chown(path, owner, group) = fchownat(AT_FDCWD, path, owner, group, 0)。 */
  const char __user *upath = (const char __user *__force)a1;
  unsigned int owner = (unsigned int)a2;
  unsigned int group = (unsigned int)a3;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_fchownat(AT_FDCWD, kpath, owner, group, 0);
}
int64_t sys_fchown(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;
  /* fchown(fd, owner, group) = fchownat(fd, "", owner, group, AT_EMPTY_PATH)。
   */
  int fd = (int)a1;
  unsigned int owner = (unsigned int)a2;
  unsigned int group = (unsigned int)a3;
  return do_fchownat(fd, "", owner, group, AT_EMPTY_PATH);
}
int64_t sys_linkat(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a5;
  (void)a6;
  /* linkat(olddirfd, old, newdirfd, new, flags)。 */
  int olddirfd = (int)a1;
  const char __user *uold = (const char __user *__force)a2;
  int newdirfd = (int)a3;
  const char __user *unew = (const char __user *__force)a4;
  int flags = (int)a5;
  if (!uold || !unew)
    return -EFAULT;
  char kold[256], knew[256];
  if (strncpy_from_user(kold, uold, sizeof(kold)) < 0)
    return -EFAULT;
  if (strncpy_from_user(knew, unew, sizeof(knew)) < 0)
    return -EFAULT;
  return do_linkat(olddirfd, kold, newdirfd, knew, flags);
}
int64_t sys_symlinkat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;
  /* symlinkat(target, newdirfd, linkpath)。 */
  const char __user *utarget = (const char __user *__force)a1;
  int newdirfd = (int)a2;
  const char __user *ulink = (const char __user *__force)a3;
  if (!utarget || !ulink)
    return -EFAULT;
  char ktarget[256], klink[256];
  if (strncpy_from_user(ktarget, utarget, sizeof(ktarget)) < 0)
    return -EFAULT;
  if (strncpy_from_user(klink, ulink, sizeof(klink)) < 0)
    return -EFAULT;
  return do_symlinkat(ktarget, newdirfd, klink);
}
int64_t sys_readlinkat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                       int64_t a5, int64_t a6) {
  (void)a5;
  (void)a6;
  /* readlinkat(dirfd, path, buf, bufsiz)。 */
  int dirfd = (int)a1;
  const char __user *upath = (const char __user *__force)a2;
  char __user *ubuf = (char __user *__force)a3;
  size_t bufsiz = (size_t)a4;
  if (!upath || !ubuf)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_readlinkat(dirfd, kpath, ubuf, bufsiz);
}
int64_t sys_fchmodat(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  (void)a5;
  (void)a6;
  /* fchmodat(dirfd, path, mode, flags)。 */
  int dirfd = (int)a1;
  const char __user *upath = (const char __user *__force)a2;
  unsigned int mode = (unsigned int)a3;
  int flags = (int)a4;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_fchmodat(dirfd, kpath, mode, flags);
}
int64_t sys_fchownat(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                     int64_t a6) {
  (void)a6;
  /* fchownat(dirfd, path, owner, group, flags)。 */
  int dirfd = (int)a1;
  const char __user *upath = (const char __user *__force)a2;
  unsigned int owner = (unsigned int)a3;
  unsigned int group = (unsigned int)a4;
  int flags = (int)a5;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_fchownat(dirfd, kpath, owner, group, flags);
}
int64_t sys_clock_settime(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                          int64_t a5, int64_t a6) {
  int clk = (int)a1;
  const struct timespec __user *ts_u =
      (const struct timespec __user *)(uintptr_t)a2;

  // Only CLOCK_REALTIME / CLOCK_TAI may be set (Linux likewise); others -EPERM.
  if (clk != CLOCK_REALTIME && clk != CLOCK_TAI)
    return -EPERM;

  // CAP_SYS_TIME via capable() 收口(今天等价 euid==0;Linux 要求 CAP_SYS_TIME)。
  if (!capable(CAP_SYS_TIME))
    return -EPERM;

  struct timespec kts;
  if (copy_from_user(&kts, ts_u, sizeof(kts)))
    return -EFAULT;
  if (kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
    return -EINVAL;

  // Adjust the in-memory offset so the next CLOCK_REALTIME read returns new_ns
  // at this instant: wall_clock_boot_ns = new_ns - sched_clock().  CMOS is not
  // written back (todo: NVRAM persistence).  uint64 wraparound makes a past
  // time still land on the right value modulo 2^64, matching Linux.
  uint64_t new_ns =
      (uint64_t)kts.tv_sec * 1000000000ULL + (uint64_t)kts.tv_nsec;
  uint64_t boot_ns = new_ns - sched_clock();
  __atomic_store_n(&wall_clock_boot_ns, boot_ns, __ATOMIC_RELEASE);
  return 0;
}
int64_t sys_getitimer(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  return -ENOSYS;
}
// ===================== BSD syscall: setitimer =====================
// ITIMER_REAL routes onto the per-process alarm_deadline (signal_struct) via
// alarm_set_deadline, so musl's alarm()/ualarm() — which call setitimer
// (ITIMER_REAL, &it, &it) and read the old remaining back from the `old`
// arg — actually arm a timer and see the previous value. Without this, alarm()
// silently no-ops (kernel returned -ENOSYS, musl ignored it, `it` was not
// overwritten) → alarm(5) returned 5 not 0, and alarm(1)+pause() never fired
// SIGALRM → pause() hung forever (signal.elf freeze).
//
// Single-shot only: it_value arms alarm_deadline for one SIGALRM. it_interval
// (repeating) is NOT yet supported — the alarm_check mechanism consumes the
// deadline on expiry and does not re-arm. Recorded in doc/design/todo.md.
// ITIMER_VIRTUAL/ITIMER_PROF unsupported → -ENOSYS.
struct k_itimerval {
  struct timeval it_interval;
  struct timeval it_value;
};
int64_t sys_setitimer(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;
  int which = (int)a1;
  if (which != 0 /* ITIMER_REAL */)
    return (int64_t)-ENOSYS;

  const struct k_itimerval __user *newv =
      (const struct k_itimerval __user *)(uintptr_t)a2;
  struct k_itimerval __user *oldv = (struct k_itimerval __user *)(uintptr_t)a3;

  uint64_t now = sched_clock();
  uint64_t old;
  if (newv) {
    struct k_itimerval knew;
    if (copy_from_user(&knew, newv, sizeof(knew)))
      return (int64_t)-EFAULT;
    uint64_t dur = (uint64_t)knew.it_value.tv_sec * 1000000000ULL +
                   (uint64_t)knew.it_value.tv_usec * 1000ULL;
    uint64_t new_deadline = dur ? now + dur : 0;
    old = alarm_set_deadline(new_deadline);
  } else {
    // Query-only (newv == NULL): return old without changing the deadline.
    struct signal_struct *sig = current_proc->signal;
    uint64_t sflags;
    spin_lock_irqsave(&sig->sig_lock, &sflags);
    old = sig->alarm_deadline;
    spin_unlock_irqrestore(&sig->sig_lock, sflags);
  }

  if (oldv) {
    uint64_t rem_ns = (old && old > now) ? old - now : 0;
    struct k_itimerval kold;
    __memset(&kold, 0, sizeof(kold));
    kold.it_value.tv_sec = (long)(rem_ns / 1000000000ULL);
    kold.it_value.tv_usec = (long)((rem_ns % 1000000000ULL) / 1000ULL);
    if (copy_to_user(oldv, &kold, sizeof(kold)))
      return (int64_t)-EFAULT;
  }
  return 0;
}

// ===================== trivial-return stubs (C2 group) =====================

/* ===================== path-based inode 元数据/链接 syscall
 * ===================== 9
 * 个:access(21)/faccessat(269)/readlink(89)/readlinkat(267)/link(86)/
 * linkat(265)/symlink(88)/symlinkat(266)/utimensat(280)。服务「在本 OS 上编译
 * llvm libc」目标,语义对齐 glibc/Linux(Q6 严格 flags)。
 *
 * 解析模型:绝对路径 → mount 表最长前缀匹配(vfs_resolve)+ path_walk;
 * 相对路径(dirfd)→ resolve_dirfd_start + path_walk_from。AT_FDCWD 解析到
 * bp->cwd 的 inode(resolve_dirfd_start,musl *at wrapper 直送 AT_FDCWD;
 * bp->cwd 由 sys_chdir 维护,是 cwd 唯一真相)。
 * 权限(Q4):inode_permission 按 euid 判定,非"无脑 root 放行"(本 OS 有完整
 * permission ladder,proc.h uid/euid/...,test_setuid_saved 证 ladder 真在跑)。
 * 时间戳(Q5):inode 内存态 atime/mtime/ctime,getattr 读;不落盘 FAT32
 * (llvm libc utimensat test 不跨重启)。UTIME_NOW/OMIT 见 uapi fcntl.h。
 * symlink/link(Q2/Q3):tmpfs/devtmpfs 真实现(阶段2/3);FAT32 物理不支持 →
 * symlink/link 返 -EPERM/-ENOSYS(readlink 同)。详见 tmp1.md。
 */

/* do_faccessat:access(path,mode)=faccessat(AT_FDCWD,path,mode,0) 的共同实现。
 * kpath 为内核字符串(调用方已 copy_from_user)。flags 严格校验(Q6)。 */
static int do_faccessat(int dirfd, const char *kpath, int mode, int flags) {
  if (mode & ~(R_OK | W_OK | X_OK | F_OK))
    return -EINVAL;
  if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return -EINVAL;

  struct inode *ip;
  if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
    /* stat fd 本身:复用 vfs_statx 的 fd 路径。 */
    if (dirfd < 0)
      return -EBADF;
    xtask *proc = current_task;
    rcu_read_lock();
    struct file *f = fd_lookup(proc->proc->files, dirfd);
    if (!f) {
      rcu_read_unlock();
      return -EBADF;
    }
    file_get(f);
    rcu_read_unlock();
    int r = f->inode ? inode_permission(f->inode, mode) : -EBADF;
    file_put(f);
    return r;
  }

  if (kpath[0] == '/') {
    char relpath[256];
    struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
    if (!m)
      return -ENOENT;
    ip = path_walk(m, relpath); /* +1 */
  } else {
    struct inode *start = resolve_dirfd_start(dirfd);
    if (IS_ERR(start))
      return (int)PTR_ERR(start);
    ip = path_walk_from(start, kpath); /* +1 */
    inode_put(start);
  }
  if (!ip)
    return -ENOENT;
  int r = inode_permission(ip, mode);
  inode_put(ip);
  return r;
}

int64_t sys_access(int64_t a1, int64_t a2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  const char __user *upath = (const char __user *__force)a1;
  int mode = (int)a2;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_faccessat(AT_FDCWD, kpath, mode, 0);
}

int64_t sys_faccessat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t unused5, int64_t unused6) {
  (void)unused5;
  (void)unused6;
  int dirfd = (int)a1;
  const char __user *upath = (const char __user *__force)a2;
  int mode = (int)a3;
  int flags = (int)a4;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_faccessat(dirfd, kpath, mode, flags);
}

/* sys_faccessat2(dirfd, path, mode, flags) — SYS_FACCESSAT2 (439).  LLVM libc
 * hard-#errors without SYS_faccessat2 (faccessat.cpp).  The legacy
 * SYS_faccessat (269) accepts the same flags; the only Linux difference is
 * that the old entry ignored `flags`, while this kernel's do_faccessat has
 * always honoured them.  So this is a verbatim alias of sys_faccessat. */
int64_t sys_faccessat2(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                       int64_t unused5, int64_t unused6) {
  (void)unused5;
  (void)unused6;
  int dirfd = (int)a1;
  const char __user *upath = (const char __user *__force)a2;
  int mode = (int)a3;
  int flags = (int)a4;
  if (!upath)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  return do_faccessat(dirfd, kpath, mode, flags);
}

/* fill_statfs: populate struct statfs for a mount.  Per the decision in
 * doc/design/todo.md, capacity fields (f_blocks/f_bfree/f_bavail/f_files/
 * f_ffree) stay 0 — FAT32 keeps no free-cluster counter and llvm-libc's
 * pathconf() only reads f_type/f_bsize/f_frsize/f_namelen.  f_type is chosen
 * by fstype name so FAT32 honestly reports MSDOS_SUPER_MAGIC (FAT has no
 * symlink support, matching _PC_2_SYMLINKS=0).  Returns the magic, or 0 if
 * the fstype is unknown (f_type 0 + the caller still copies out). */
static long statfs_magic_for(const struct mount_entry *m) {
  if (!m || !m->fs)
    return 0;
  const char *name = m->fs->name;
  if (__strcmp(name, "fat32") == 0)
    return MSDOS_SUPER_MAGIC;
  if (__strcmp(name, "tmpfs") == 0 || __strcmp(name, "devtmpfs") == 0)
    return TMPFS_MAGIC;
  if (__strcmp(name, "sysfs") == 0)
    return SYSFS_MAGIC;
  return 0;
}

static void fill_statfs(struct statfs *ks, const struct mount_entry *m) {
  __memset(ks, 0, sizeof(*ks));
  long magic = statfs_magic_for(m);
  ks->f_type = magic;
  long bsize = (magic == MSDOS_SUPER_MAGIC) ? (long)fat32_bytes_per_cluster()
                                            : (long)PAGE_SIZE;
  ks->f_bsize = bsize;
  ks->f_frsize = bsize;
  ks->f_namelen = 255; /* FAT32 LFN / tmpfs / sysfs all cap at 255 */
}

/* sys_statfs(path, buf) — SYS_STATFS (137).  Resolves path to its mount via
 * vfs_resolve (longest-prefix mount-table match), same entry path as
 * sys_access.  fd-less; AT_FDCWD not applicable. */
int64_t sys_statfs(int64_t a1, int64_t a2, int64_t unused3, int64_t unused4,
                   int64_t unused5, int64_t unused6) {
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  const char __user *upath = (const char __user *__force)a1;
  struct statfs __user *ubuf = (struct statfs __user *)(uintptr_t)a2;
  if (!upath || !ubuf)
    return -EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return -EFAULT;
  char relpath[256];
  struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
  if (!m)
    return -ENOENT;
  struct statfs ks;
  fill_statfs(&ks, m);
  if (copy_to_user(ubuf, &ks, sizeof(ks)))
    return -EFAULT;
  return 0;
}

/* sys_fstatfs(fd, buf) — SYS_FSTATFS (138).  Resolves fd → file → inode →
 * mount (mount_of_inode), mirroring do_faccessat's AT_EMPTY_PATH fd path. */
int64_t sys_fstatfs(int64_t a1, int64_t a2, int64_t unused3, int64_t unused4,
                    int64_t unused5, int64_t unused6) {
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  int fd = (int)a1;
  struct statfs __user *ubuf = (struct statfs __user *)(uintptr_t)a2;
  if (!ubuf)
    return -EFAULT;
  if (fd < 0)
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
  struct mount_entry *m = f->inode ? mount_of_inode(f->inode) : NULL;
  struct statfs ks;
  fill_statfs(&ks, m);
  file_put(f);
  if (copy_to_user(ubuf, &ks, sizeof(ks)))
    return -EFAULT;
  return 0;
}

/* do_utimensat:utimensat 的共同实现。kpath 内核串;times 为 NULL 时 atime=
 * mtime=now(需写权限)。UTIME_NOW/OMIT 见 uapi。flags 仅 AT_SYMLINK_NOFOLLOW
 * 合法(本 OS 无 symlink,接受但语义同 follow;Q6 严格校验未知位)。 */
static int do_utimensat(int dirfd, const char *kpath, struct timespec *ktimes,
                        int flags) {
  if (flags & ~AT_SYMLINK_NOFOLLOW)
    return -EINVAL;

  uint64_t na, nm; /* atime/mtime ns;UINT64_MAX=OMIT 哨兵,不写该字段 */
  if (ktimes) {
    /* 校验 tv_nsec:合法值 ∈ [0,1e9) ∪ {UTIME_NOW,UTIME_OMIT}(Q6 严格)。
     * 不改写 ktimes——NOW/OMIT 的判定在下方 na/nm 计算时仍需原值。 */
    for (int i = 0; i < 2; i++) {
      if (ktimes[i].tv_nsec == UTIME_NOW || ktimes[i].tv_nsec == UTIME_OMIT)
        continue;
      if (ktimes[i].tv_nsec < 0 || ktimes[i].tv_nsec >= 1000000000L)
        return -EINVAL;
    }
    uint64_t now =
        __atomic_load_n(&wall_clock_boot_ns, __ATOMIC_RELAXED) + sched_clock();
    na = (ktimes[0].tv_nsec == UTIME_OMIT) ? UINT64_MAX
         : (ktimes[0].tv_nsec == UTIME_NOW)
             ? now
             : (uint64_t)ktimes[0].tv_sec * 1000000000ULL +
                   (uint64_t)ktimes[0].tv_nsec;
    nm = (ktimes[1].tv_nsec == UTIME_OMIT) ? UINT64_MAX
         : (ktimes[1].tv_nsec == UTIME_NOW)
             ? now
             : (uint64_t)ktimes[1].tv_sec * 1000000000ULL +
                   (uint64_t)ktimes[1].tv_nsec;
  } else {
    /* times=NULL:atime=mtime=now,需写权限(对齐 Linux)。 */
    uint64_t now =
        __atomic_load_n(&wall_clock_boot_ns, __ATOMIC_RELAXED) + sched_clock();
    na = nm = now;
  }

  struct inode *ip;
  int need_write_perm = !ktimes; /* times=NULL 需写权限 */
  if (kpath[0] == '/') {
    char relpath[256];
    struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
    if (!m)
      return -ENOENT;
    ip = path_walk(m, relpath); /* +1 */
  } else {
    struct inode *start = resolve_dirfd_start(dirfd);
    if (IS_ERR(start))
      return (int)PTR_ERR(start);
    ip = path_walk_from(start, kpath); /* +1 */
    inode_put(start);
  }
  if (!ip)
    return -ENOENT;
  if (need_write_perm) {
    int r = inode_permission(ip, W_OK);
    if (r) {
      inode_put(ip);
      return r;
    }
  }
  int which = ((na != UINT64_MAX) ? ATIME_BIT : 0) |
              ((nm != UINT64_MAX) ? MTIME_BIT : 0);
  int err;
  if (ip->i_op && ip->i_op->update_time)
    err = ip->i_op->update_time(ip, na, nm, 0, which);
  else
    err = generic_update_time(ip, na, nm, 0, which);
  inode_put(ip);
  return err;
}

int64_t sys_utimensat(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t unused5, int64_t unused6) {
  (void)unused5;
  (void)unused6;
  int dirfd = (int)a1;
  const char __user *upath = (const char __user *__force)a2;
  const struct timespec __user *utimes =
      (const struct timespec __user *)(uintptr_t)a3;
  int flags = (int)a4;

  char kpath[256];
  if (upath) {
    if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
      return -EFAULT;
  } else {
    /* path=NULL:作用于 dirfd 本身(对齐 Linux utimensat(2))。 */
    kpath[0] = '\0';
    flags |= AT_EMPTY_PATH;
  }

  struct timespec kt[2];
  struct timespec *kp = NULL;
  if (utimes) {
    if (copy_from_user(kt, utimes, sizeof(kt)))
      return -EFAULT;
    kp = kt;
  }
  return do_utimensat(dirfd, kpath, kp, flags);
}

// ===================== Thin wrappers (A group) =====================

// A1: mkdirat(dirfd, path, mode) — AT_FDCWD-only thin wrapper over sys_mkdir
// A1: mkdirat(dirfd, path, mode). Absolute path → sys_mkdir. Relative path →
// resolve parent from dirfd's directory inode via path_walk_parent_from.
int64_t sys_mkdirat(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                    int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  int dirfd = (int)arg1;
  const char __user *upath = (const char __user *__force)arg2;
  if (!upath)
    return (int64_t)-EFAULT;

  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return (int64_t)-EFAULT;
  if (kpath[0] == '/')
    return sys_mkdir(arg2, arg3, 0, 0, 0, 0);

  struct inode *start = resolve_dirfd_start(dirfd);
  if (IS_ERR(start))
    return (int64_t)PTR_ERR(start);
  char relpath[256], lastname[256];
  if (normalize_path(kpath, relpath, sizeof(relpath)) < 0) {
    inode_put(start);
    return (int64_t)-ENAMETOOLONG;
  }
  struct inode *parent = NULL;
  int rc = path_walk_parent_from(start, relpath, &parent, lastname,
                                 sizeof(lastname));
  if (rc) {
    inode_put(start);
    if (parent)
      inode_put(parent);
    return (int64_t)rc;
  }
  if (!parent->i_op || !parent->i_op->mkdir) {
    inode_put(start);
    inode_put(parent);
    return (int64_t)-EPERM;
  }
  int eff_mode = ((int)arg3 & 0777) & ~(int)current_proc->umask;
  rc = parent->i_op->mkdir(parent, lastname, eff_mode);
  inode_put(parent);
  if (rc != 0) {
    inode_put(start);
    return (int64_t)rc;
  }
  /* S08: 取回新建目录设 owner + umask 权限位(对齐 sys_mkdir)。start 仍持 +1,
   * 供 path_walk_from 解析。 */
  struct inode *nip = path_walk_from(start, relpath); /* +1 */
  inode_put(start);
  if (nip) {
    nip->mode = (nip->mode & ~0777) | (uint32_t)eff_mode;
    nip->uid = current_proc->uid;
    nip->gid = current_proc->gid;
    inode_put(nip);
  }
  return 0;
}

// A2: unlinkat(dirfd, path, flags). AT_REMOVEDIR → rmdir semantics, else
// unlink. Absolute path → sys_unlink/sys_rmdir. Relative path → resolve from
// dirfd.
int64_t sys_unlinkat(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                     int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  int dirfd = (int)arg1;
  int flags = (int)arg3;
  if (flags & ~AT_REMOVEDIR)
    return (int64_t)-EINVAL;
  const char __user *upath = (const char __user *__force)arg2;
  if (!upath)
    return (int64_t)-EFAULT;

  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return (int64_t)-EFAULT;
  if (kpath[0] == '/') {
    if (flags & AT_REMOVEDIR)
      return sys_rmdir(arg2, 0, 0, 0, 0, 0);
    return sys_unlink(arg2, 0, 0, 0, 0, 0);
  }

  struct inode *start = resolve_dirfd_start(dirfd);
  if (IS_ERR(start))
    return (int64_t)PTR_ERR(start);
  char relpath[256], lastname[256];
  if (normalize_path(kpath, relpath, sizeof(relpath)) < 0) {
    inode_put(start);
    return (int64_t)-ENAMETOOLONG;
  }
  struct inode *parent = NULL;
  int rc = path_walk_parent_from(start, relpath, &parent, lastname,
                                 sizeof(lastname));
  inode_put(start);
  if (rc) {
    if (parent)
      inode_put(parent);
    return (int64_t)rc;
  }
  if (flags & AT_REMOVEDIR) {
    if (!parent->i_op || !parent->i_op->rmdir) {
      inode_put(parent);
      return (int64_t)-EPERM;
    }
    rc = parent->i_op->rmdir(parent, lastname);
  } else {
    if (!parent->i_op || !parent->i_op->unlink) {
      inode_put(parent);
      return (int64_t)-EPERM;
    }
    rc = parent->i_op->unlink(parent, lastname);
  }
  inode_put(parent);
  return (int64_t)rc;
}

// A3: renameat(olddirfd, oldpath, newdirfd, newpath). Absolute path →
// sys_rename component. Relative paths → resolve each side from its dirfd.
// Cross-dirfd rename is allowed (both sides resolved to concrete parent
// inodes); cross-fs is not (the two parents may belong to different fstypes —
// caller's i_op->rename must handle a foreign new_parent; fat32/tmpfs rename is
// intra-fs only).
int64_t sys_renameat(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                     int64_t unused1, int64_t unused2) {
  (void)unused1;
  (void)unused2;
  int olddirfd = (int)arg1;
  int newdirfd = (int)arg3;
  const char __user *uold = (const char __user *__force)arg2;
  const char __user *unew = (const char __user *__force)arg4;
  if (!uold || !unew)
    return (int64_t)-EFAULT;

  char old_k[256], new_k[256];
  if (strncpy_from_user(old_k, uold, sizeof(old_k)) < 0)
    return (int64_t)-EFAULT;
  if (strncpy_from_user(new_k, unew, sizeof(new_k)) < 0)
    return (int64_t)-EFAULT;

  /* Both absolute → existing sys_rename (mount-table match + same-mount check).
   */
  if (old_k[0] == '/' && new_k[0] == '/')
    return sys_rename(arg2, arg4, 0, 0, 0, 0);

  struct inode *old_start = resolve_dirfd_start(olddirfd);
  if (IS_ERR(old_start))
    return (int64_t)PTR_ERR(old_start);
  struct inode *new_start = resolve_dirfd_start(newdirfd);
  if (IS_ERR(new_start)) {
    inode_put(old_start);
    return (int64_t)PTR_ERR(new_start);
  }

  char old_rel[256], old_name[256];
  char new_rel[256], new_name[256];
  if (normalize_path(old_k, old_rel, sizeof(old_rel)) < 0 ||
      normalize_path(new_k, new_rel, sizeof(new_rel)) < 0) {
    inode_put(old_start);
    inode_put(new_start);
    return (int64_t)-ENAMETOOLONG;
  }

  struct inode *old_parent = NULL, *new_parent = NULL;
  int rc = path_walk_parent_from(old_start, old_rel, &old_parent, old_name,
                                 sizeof(old_name));
  if (rc) {
    if (old_parent)
      inode_put(old_parent);
    inode_put(old_start);
    inode_put(new_start);
    return (int64_t)rc;
  }
  rc = path_walk_parent_from(new_start, new_rel, &new_parent, new_name,
                             sizeof(new_name));
  inode_put(old_start);
  inode_put(new_start);
  if (rc) {
    if (new_parent)
      inode_put(new_parent);
    inode_put(old_parent);
    return (int64_t)rc;
  }

  if (!old_parent->i_op || !old_parent->i_op->rename) {
    inode_put(old_parent);
    inode_put(new_parent);
    return (int64_t)-EPERM;
  }
  rc = old_parent->i_op->rename(old_parent, old_name, new_parent, new_name);
  inode_put(old_parent);
  inode_put(new_parent);
  return (int64_t)rc;
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
  // S06: dup never produces a cloexec fd (POSIX). Clear the slot's bit in case
  // a prior occupant left it set and the slot was recycled.
  fd_set_cloexec(proc->proc->files, new_fd, 0);
  spin_unlock(fdlk);
  return (int64_t)new_fd;
}

// A5: dup3(oldfd, newfd, flags) — like dup2 but with O_CLOEXEC support.
// S06: cloexec is per-fd (bitmap), so only `newfd` is affected; `oldfd` is
// untouched. The victim's bitmap entry is replaced explicitly (slot reused).
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

  // only the new fd is cloexec; old_fd is untouched. dup3 replaces the
  // victim's bitmap entry, so set/clear it explicitly (the slot is reused).
  fd_set_cloexec(proc->proc->files, new_fd, (flags & O_CLOEXEC) ? 1 : 0);

  spin_unlock(fdlk);
  if (victim) {
    synchronize_rcu();
    file_put(victim);
  }
  return (int64_t)new_fd;
}

// A8: gettimeofday(tv, tz) — thin wrapper over clock_gettime(CLOCK_REALTIME)
// tz is always ignored (Linux returns 0 for tz, most callers pass NULL).
// CLOCK_REALTIME = wall_clock_boot_ns (RTC epoch anchored at boot) +
// sched_clock() (monotonic nanoseconds since boot).
int64_t sys_gettimeofday(int64_t arg1, int64_t arg2, int64_t unused1,
                         int64_t unused2, int64_t unused3, int64_t unused4) {
  struct timeval __user *tv = (struct timeval __user *)(uintptr_t)arg1;
  (void)arg2; // timezone always ignored

  if (!tv)
    return 0; // Linux: tv=NULL is valid (just don't fill it)

  uint64_t ns =
      __atomic_load_n(&wall_clock_boot_ns, __ATOMIC_RELAXED) + sched_clock();
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
  // nodename reflects the live hostname (sethostname/hostname_set) so
  // llvm-libc's gethostname() — which reads uname.nodename, Linux having no
  // SYS_gethostname — agrees with the OS-specific SYS_GETHOSTNAME path.
  // hostname_get NUL-terminates only when n < maxlen; force-cap the field.
  hostname_get(kbuf.nodename, __NEW_UTS_LEN);
  kbuf.nodename[__NEW_UTS_LEN - 1] = '\0';
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

      // Arm per-request reply state BEFORE enqueue: sys_resp publishes
      // req_result/req_replied under our scheduler_lock; clearing them after
      // enqueue could clobber an already-delivered reply from a fast target
      // on another CPU (lost wake → 3s -ETIMEDOUT, bug.md Bug 1).
      proc->req_target_pid = target_pid;
      proc->req_reply_buf = arg;
      proc->req_reply_len = arg_size;
      proc->req_result = 0;
      proc->req_replied = 0;
      proc->wait_timed_out = 0;

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

      // Block caller on WAIT_REQ_REPLY. Arm the wait under our own
      // scheduler_lock, in the same critical section. Without this, the target
      // (evdev on another CPU) can receive the REQ, run sys_resp() and try to
      // wake us between the wake above and setting BLOCKED below — sys_resp's
      // wake-check then sees us not-yet-BLOCKED and drops the wake, stranding
      // us until the 3s timeout. Now that sys_recv's lost-wake is fixed, evdev
      // replies fast, so this caller-side window is the more likely failure;
      // close it the same way.
      //
      // req_replied is set by sys_resp under this lock before its wake-check;
      // it was cleared before the enqueue above and is re-checked under the
      // lock. If the reply already landed, stay RUNNING and return without
      // sleeping (lost-wake guard). See sys_req for the full rationale.
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

    // Arm per-request reply state BEFORE enqueue — same lost-wake rationale
    // as the inline path above (sys_resp publishes under our scheduler_lock;
    // a post-enqueue clear could clobber a fast reply).
    proc->req_target_pid = target_pid;
    proc->req_reply_buf = arg;
    proc->req_reply_len = arg_size;
    proc->req_result = 0;
    proc->req_replied = 0;
    proc->wait_timed_out = 0;

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
  f->flags = O_RDWR; // S06: cloexec is per-fd, set via the bitmap below
  f->shm = shm;
  fd_install(proc->proc->files, fd, f);
  fd_set_cloexec(proc->proc->files, fd, (flags & MFD_CLOEXEC) ? 1 : 0);
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

// ===================== BSD syscall: fallocate =====================
// sys_fallocate(fd, mode, offset, len). x86-64 ABI: rdi=fd rsi=mode
// rdx=offset r10=len. 仅支持 mode=0(posix_fallocate 默认)：普通文件经
// i_op->setattr grow+zero(fat32/tmpfs setattr 内部分配并清零、drop stale
// page cache)；memfd(SHM) 委派 sys_ftruncate 的 grow 分支(遵 F_SEAL_GROW)。
// 其它 mode(KEEP_SIZE/PUNCH_HOLE/...)一律 -EOPNOTSUPP(FAT32 连续无洞)。
int64_t sys_fallocate(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                      int64_t unused1, int64_t unused2) {
  int fd = (int)arg1;
  int mode = (int)arg2;
  int64_t off = (int64_t)arg3;
  int64_t len = (int64_t)arg4;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  if (off < 0 || len <= 0)
    return (int64_t)-EINVAL;
  uint64_t total = (uint64_t)off + (uint64_t)len;
  if (total < (uint64_t)off) /* overflow */
    return (int64_t)-EINVAL;
  if (mode != 0)
    return (int64_t)-EOPNOTSUPP;

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
    if (total <= ip->size) { /* FAT32 连续无洞,已分配 */
      ret = 0;
      goto out;
    }
    if (!ip->i_op || !ip->i_op->setattr) {
      ret = -EOPNOTSUPP;
      goto out;
    }
    ret = (int64_t)ip->i_op->setattr(ip, total); /* grow + zero-fill */
  } else if (f->type == FD_SHM) {
    struct shm *shm = f->shm;
    if (!shm) {
      ret = -EBADF;
      goto out;
    }
    uint64_t cur = shm->page_list ? (uint64_t)shm->num_pages * PAGE_SIZE
                                  : (uint64_t)shm->npages * PAGE_SIZE;
    if (total <= cur) {
      ret = 0;
      goto out;
    }
    /* target>cur → sys_ftruncate 只走 grow 分支(遵 F_SEAL_GROW)。
     * 释放本函数的引用后委派,避免双引用。 */
    file_put(f);
    return sys_ftruncate((int64_t)fd, (int64_t)total, 0, 0, 0, 0);
  } else {
    ret = -EINVAL; /* pipe/socket/dev/dir */
  }
out:
  file_put(f);
  return ret;
}

// ===================== BSD syscall: fadvise64 =====================
// sys_fadvise64(fd, offset, len, advice). x86-64 ABI: rdi=fd rsi=offset
// rdx=len r10=advice. 全部 POSIX_FADV_* advice 为 advisory no-op(无 readahead
// 基建,无逐 range 安全丢页 API):仅校验 fd + advice 范围后返 0。DONTNEED 不
// 真丢页(技术债见 doc/design/todo.md)。
int64_t sys_fadvise64(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                      int64_t unused1, int64_t unused2) {
  int fd = (int)arg1;
  int advice = (int)arg4;
  (void)arg2;
  (void)arg3;

  if (advice < 0 || advice > 5) /* POSIX_FADV_NORMAL..NOREUSE */
    return (int64_t)-EINVAL;
  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  rcu_read_unlock();
  if (!f)
    return (int64_t)-EBADF;
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
  int fd = alloc_fd(proc->proc->files, 0);
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
  region->shm_obj = NULL;
  region->fd = -1;
  region->offset = 0;
  region->flags = KMAP_PHYSICAL;
  region->inode = NULL;
  region->shm_private_src = NULL;
  region->next = NULL;
  vma_insert_sorted(proc->mm, region);

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

  // Exact-start match only (mirrors sys_munmap behavior).
  mmap_region *r = vma_find(proc->mm, vaddr);
  if (!r || r->vaddr != vaddr)
    return (int64_t)-EINVAL;

  size_t npages = r->size / PAGE_SIZE;

  unmap_user_pages(
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3),
      r->vaddr, r->vaddr + r->size, npages);

  struct page *page = bfc_frames + (r->phys / PAGE_SIZE);
  bfc_free_page(page, npages);

  // Unlink from the sorted list.
  mmap_region **pp = &proc->mm->mmap_regions;
  while (*pp != r)
    pp = &(*pp)->next;
  *pp = r->next;
  // S12: uniform invariant — drop any file-backed refs (DMA regions are
  // KMAP_PHYSICAL and carry none, so these are no-ops here).
  if (r->inode)
    inode_put(r->inode);
  if (r->shm_private_src)
    shm_put(r->shm_private_src);
  kfree(r);
  return 0;
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
    case SEEK_DATA:
    case SEEK_HOLE: {
      // No sparse tracking: the whole file is data, the tail beyond EOF is a
      // single hole. Directories have no data/hole notion → -EINVAL (Linux).
      if (f->type == FD_DIR) {
        ret = -EINVAL;
        goto out;
      }
      if (offset < 0) {
        ret = -EINVAL;
        goto out;
      }
      if ((uint64_t)offset >= ip->size) {
        ret = -ENXIO;
        goto out;
      }
      new_offset = (whence == SEEK_DATA) ? offset : (int64_t)ip->size;
      break;
    }
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
    xtask *t = task_get(pid);
    if (!t->proc || !t->proc->signal ||
        t->proc->signal->parent_pid != current_task->pid)
      return (int64_t)-ESRCH;
    if (t->proc->sid != current_proc->sid)
      return (int64_t)-EPERM;
    t->proc->pgid = pgid;
  } else {
    task_get(pid)->proc->pgid = pgid;
  }
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

// ===================== getcpu =====================
int64_t sys_getcpu(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  uint32_t __user *cpu = (uint32_t __user *)arg1;
  uint32_t __user *node = (uint32_t __user *)arg2;
  if (cpu) {
    uint32_t c = (uint32_t)get_cpu_local()->cpu_id;
    if (copy_to_user(cpu, &c, sizeof(c)))
      return (int64_t)-EFAULT;
  }
  if (node) {
    uint32_t n = 0;
    if (copy_to_user(node, &n, sizeof(n)))
      return (int64_t)-EFAULT;
  }
  return 0;
}

// ===================== sched_get_priority_max / min =====================
int64_t sys_sched_get_priority_max(int64_t arg1, int64_t unused1,
                                   int64_t unused2, int64_t unused3,
                                   int64_t unused4, int64_t unused5) {
  int policy = (int)arg1;
  (void)policy;
  // SCHED_FIFO/SCHED_RR priority range: 1-99, SCHED_OTHER: 0
  return 99;
}

int64_t sys_sched_get_priority_min(int64_t arg1, int64_t unused1,
                                   int64_t unused2, int64_t unused3,
                                   int64_t unused4, int64_t unused5) {
  int policy = (int)arg1;
  (void)policy;
  return 1;
}

// ===================== sched_rr_get_interval =====================
int64_t sys_sched_rr_get_interval(int64_t arg1, int64_t arg2, int64_t unused1,
                                  int64_t unused2, int64_t unused3,
                                  int64_t unused4) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 100000000; // 100ms timeslice
  if (copy_to_user((void __user *)arg2, &ts, sizeof(ts)))
    return (int64_t)-EFAULT;
  return 0;
}

// ===================== sched_setparam / getparam =====================
int64_t sys_sched_setparam(int64_t arg1, int64_t arg2, int64_t unused1,
                           int64_t unused2, int64_t unused3, int64_t unused4) {
  pid_t pid = (pid_t)arg1;
  xtask *t = (pid == 0) ? current_task : task_get(pid);
  if (pid != 0 && (pid < 0 || pid >= MAX_PROC || t->pid != pid))
    return (int64_t)-ESRCH;
  struct sched_param {
    int sched_priority;
  } param;
  if (copy_from_user(&param, (void __user *)arg2, sizeof(param)))
    return (int64_t)-EFAULT;
  t->sched_priority = param.sched_priority;
  return 0;
}

int64_t sys_sched_getparam(int64_t arg1, int64_t arg2, int64_t unused1,
                           int64_t unused2, int64_t unused3, int64_t unused4) {
  pid_t pid = (pid_t)arg1;
  xtask *t = (pid == 0) ? current_task : task_get(pid);
  if (pid != 0 && (pid < 0 || pid >= MAX_PROC || t->pid != pid))
    return (int64_t)-ESRCH;
  struct sched_param {
    int sched_priority;
  } param;
  param.sched_priority = t->sched_priority;
  if (copy_to_user((void __user *)arg2, &param, sizeof(param)))
    return (int64_t)-EFAULT;
  return 0;
}

// ===================== sched_setscheduler / getscheduler =====================
int64_t sys_sched_setscheduler(int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t unused1, int64_t unused2,
                               int64_t unused3) {
  pid_t pid = (pid_t)arg1;
  xtask *t = (pid == 0) ? current_task : task_get(pid);
  if (pid != 0 && (pid < 0 || pid >= MAX_PROC || t->pid != pid))
    return (int64_t)-ESRCH;
  int policy = (int)arg2;
  struct sched_param {
    int sched_priority;
  } param;
  if (copy_from_user(&param, (void __user *)arg3, sizeof(param)))
    return (int64_t)-EFAULT;
  int old_policy = t->policy;
  t->policy = policy;
  t->sched_priority = param.sched_priority;
  return (int64_t)old_policy;
}

int64_t sys_sched_getscheduler(int64_t arg1, int64_t unused1, int64_t unused2,
                               int64_t unused3, int64_t unused4,
                               int64_t unused5) {
  pid_t pid = (pid_t)arg1;
  xtask *t = (pid == 0) ? current_task : task_get(pid);
  if (pid != 0 && (pid < 0 || pid >= MAX_PROC || t->pid != pid))
    return (int64_t)-ESRCH;
  return (int64_t)t->policy;
}

// ===================== sched_setaffinity / getaffinity =====================
int64_t sys_sched_setaffinity(int64_t arg1, int64_t arg2, int64_t arg3,
                              int64_t unused1, int64_t unused2,
                              int64_t unused3) {
  pid_t pid = (pid_t)arg1;
  size_t cpusetsize = (size_t)arg2;
  xtask *t = (pid == 0) ? current_task : task_get(pid);
  if (pid != 0 && (pid < 0 || pid >= MAX_PROC || t->pid != pid))
    return (int64_t)-ESRCH;
  uint64_t mask = 0;
  size_t copy_sz = (cpusetsize < sizeof(mask)) ? cpusetsize : sizeof(mask);
  if (copy_from_user(&mask, (void __user *)arg3, copy_sz))
    return (int64_t)-EFAULT;
  t->cpumask = mask;
  return 0;
}

int64_t sys_sched_getaffinity(int64_t arg1, int64_t arg2, int64_t arg3,
                              int64_t unused1, int64_t unused2,
                              int64_t unused3) {
  pid_t pid = (pid_t)arg1;
  size_t cpusetsize = (size_t)arg2;
  xtask *t = (pid == 0) ? current_task : task_get(pid);
  if (pid != 0 && (pid < 0 || pid >= MAX_PROC || t->pid != pid))
    return (int64_t)-ESRCH;
  size_t copy_sz =
      (cpusetsize < sizeof(t->cpumask)) ? cpusetsize : sizeof(t->cpumask);
  if (copy_to_user((void __user *)arg3, &t->cpumask, copy_sz))
    return (int64_t)-EFAULT;
  return 0;
}

// ===================== prctl =====================
int64_t sys_prctl(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t unused) {
  int option = (int)arg1;
  switch (option) {
  case PR_SET_NAME: {
    char comm[16];
    __builtin_memset(comm, 0, sizeof(comm));
    if (strncpy_from_user(comm, (const char __user *)arg2, sizeof(comm) - 1) <
        0)
      return (int64_t)-EFAULT;
    __strncpy(current_task->comm, comm, sizeof(current_task->comm) - 1);
    current_task->comm[sizeof(current_task->comm) - 1] = '\0';
    return 0;
  }
  case PR_GET_NAME: {
    char comm[16];
    __builtin_memset(comm, 0, sizeof(comm));
    __strncpy(comm, current_task->comm, sizeof(comm) - 1);
    comm[sizeof(comm) - 1] = '\0';
    if (copy_to_user((void __user *)arg2, comm, sizeof(comm)))
      return (int64_t)-EFAULT;
    return 0;
  }
  case PR_SET_PDEATHSIG:
    // stub: no parent-death signal support
    return 0;
  case PR_GET_PDEATHSIG:
    return (int64_t)-EINVAL;
  case PR_SET_DUMPABLE:
    // stub: core dumps not supported, but accept the call
    return 0;
  case PR_GET_DUMPABLE:
    return 1; // always dumpable
  case PR_SET_PTRACER:
    // stub: accept any tracer
    return 0;
  case PR_GET_AUXV:
    return (int64_t)-EINVAL;
  case PR_SET_VMA:
    // stub: PR_SET_VMA_ANON_NAME is a no-op (no /proc/self/maps support)
    return 0;
  default:
    return (int64_t)-EINVAL;
  }
}

// ===================== chdir / fchdir / getcwd =====================
int64_t sys_getcwd(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  char __user *buf = (char __user *)arg1;
  size_t size = (size_t)arg2;

  if (!buf || !size)
    return (int64_t)-EINVAL;

  proc *bp = current_proc;
  size_t len = __strlen(bp->cwd) + 1; // include null terminator
  if (len > size)
    return (int64_t)-ERANGE;

  if (copy_to_user(buf, bp->cwd, len))
    return (int64_t)-EFAULT;
  return (int64_t)len;
}

int64_t sys_chdir(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5) {
  const char __user *pathname = (const char __user *)arg1;

  // Resolve the path to verify it exists and is a directory
  char kpath[256];
  __builtin_memset(kpath, 0, sizeof(kpath));
  if (strncpy_from_user(kpath, pathname, sizeof(kpath) - 1) < 0)
    return (int64_t)-EFAULT;
  kpath[sizeof(kpath) - 1] = '\0';

  // Build absolute path relative to current cwd
  char abs_path[512];
  proc *bp = current_proc;
  if (kpath[0] == '/') {
    __strncpy(abs_path, kpath, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';
  } else {
    size_t cwd_len = __strlen(bp->cwd);
    size_t klen = __strlen(kpath);
    // cwd + '/' + kpath + null
    if (cwd_len + 1 + klen + 1 > sizeof(abs_path))
      return (int64_t)-ENAMETOOLONG;
    __strncpy(abs_path, bp->cwd, sizeof(abs_path) - 1);
    size_t pos = __strlen(abs_path);
    if (pos > 0 && abs_path[pos - 1] != '/') {
      abs_path[pos++] = '/';
    }
    for (size_t i = 0; i <= klen; i++) {
      abs_path[pos + i] = kpath[i];
    }
  }

  // Verify the path is a directory via vfs_open_kern
  struct inode *ip = vfs_open_kern(abs_path);
  if (!ip)
    return (int64_t)-ENOENT;
  if (!S_ISDIR(ip->mode)) {
    inode_put(ip);
    return (int64_t)-ENOTDIR;
  }
  inode_put(ip);

  // Store the normalized absolute path
  size_t abs_len = __strlen(abs_path);
  if (abs_len + 1 > sizeof(bp->cwd))
    return (int64_t)-ENAMETOOLONG;
  __strncpy(bp->cwd, abs_path, sizeof(bp->cwd) - 1);
  bp->cwd[sizeof(bp->cwd) - 1] = '\0';

  return 0;
}

int64_t sys_fchdir(int64_t arg1, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4, int64_t unused5) {
  int fd = (int)arg1;

  // Look up fd
  struct file *f;
  rcu_read_lock();
  f = fd_lookup(current_proc->files, fd);
  if (f)
    file_get(f);
  rcu_read_unlock();
  if (!f)
    return (int64_t)-EBADF;

  // fd must be a directory
  if (f->type != FD_DIR) {
    file_put(f);
    return (int64_t)-ENOTDIR;
  }

  struct inode *ip = f->inode;
  if (!ip || !S_ISDIR(ip->mode)) {
    file_put(f);
    return (int64_t)-ENOTDIR;
  }

  // Use the mount point path for this directory.
  // For mount roots (e.g. "/dev", "/sys") this gives the correct path.
  // For subdirectories within a mount (FAT32), the OS cannot reconstruct the
  // full path without a dentry tree; callers should prefer chdir(pathname).
  // We use mount_of_inode() which returns the mount entry whose mntpoint
  // we store as the new cwd.
  struct mount_entry *m = mount_of_inode(ip);
  file_put(f);

  if (!m)
    return (int64_t)-ENOENT;

  size_t mlen = __strlen(m->mntpoint);
  if (mlen + 1 > sizeof(current_proc->cwd))
    return (int64_t)-ENAMETOOLONG;
  __strncpy(current_proc->cwd, m->mntpoint, sizeof(current_proc->cwd) - 1);
  current_proc->cwd[sizeof(current_proc->cwd) - 1] = '\0';

  return 0;
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
  case SYS_PIPE2:
    return sys_pipe2(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_FLOCK:
    return sys_flock(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_fallocate:
    return sys_fallocate(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_fadvise64:
    return sys_fadvise64(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_SIGALTSTACK:
    return sys_sigaltstack(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_STATX:
    return sys_statx(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_STATFS:
    return sys_statfs(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FSTATFS:
    return sys_fstatfs(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_ACCEPT4:
    return sys_accept4(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_PPOLL:
    return sys_ppoll(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // Thread syscalls
  case SYS_EXIT_GROUP:
    return sys_exit_group(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_TGKILL:
    return sys_tgkill(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_TKILL:
    return sys_tkill(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_RT_SIGPROCMASK:
    return sys_sigprocmask(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SET_TID_ADDRESS:
    return sys_set_tid_address(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                               tf->r9);
  case SYS_CLONE:
    return sys_clone(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8);
  case SYS_FUTEX:
    return sys_futex(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_MREMAP:
    return sys_mremap(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SET_ROBUST_LIST:
    return sys_set_robust_list(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                               tf->r9);
  case SYS_GET_ROBUST_LIST:
    return sys_get_robust_list(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                               tf->r9);
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
  case SYS_SETRESUID:
    return sys_setresuid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETRESGID:
    return sys_setresgid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETREUID:
    return sys_setreuid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_SETREGID:
    return sys_setregid(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_GETGROUPS:
    return sys_getgroups(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  case SYS_FACCESSAT2:
    return sys_faccessat2(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
  // getcpu (group 5)
  case SYS_GETCPU:
    return sys_getcpu(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // sched_* (group 6)
  case SYS_SCHED_SETPARAM:
    return sys_sched_setparam(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                              tf->r9);
  case SYS_SCHED_GETPARAM:
    return sys_sched_getparam(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                              tf->r9);
  case SYS_SCHED_SETSCHEDULER:
    return sys_sched_setscheduler(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                                  tf->r9);
  case SYS_SCHED_GETSCHEDULER:
    return sys_sched_getscheduler(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                                  tf->r9);
  case SYS_SCHED_GET_PRIORITY_MAX:
    return sys_sched_get_priority_max(tf->rdi, tf->rsi, tf->rdx, tf->r10,
                                      tf->r8, tf->r9);
  case SYS_SCHED_GET_PRIORITY_MIN:
    return sys_sched_get_priority_min(tf->rdi, tf->rsi, tf->rdx, tf->r10,
                                      tf->r8, tf->r9);
  case SYS_SCHED_RR_GET_INTERVAL:
    return sys_sched_rr_get_interval(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                                     tf->r9);
  case SYS_SCHED_SETAFFINITY:
    return sys_sched_setaffinity(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                                 tf->r9);
  case SYS_SCHED_GETAFFINITY:
    return sys_sched_getaffinity(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8,
                                 tf->r9);
  // prctl (group 7)
  case SYS_PRCTL:
    return sys_prctl(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  // chdir / fchdir / getcwd (group 8)
  case SYS_GETCWD:
    return sys_getcwd(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_CHDIR:
    return sys_chdir(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
  case SYS_FCHDIR:
    return sys_fchdir(tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
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
