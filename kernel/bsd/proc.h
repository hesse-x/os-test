/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_PROC_H
#define KERNEL_BSD_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/xtask.h"

#include <xos/signal.h>

typedef struct proc {
  struct xtask *xtask; // reverse reference to scheduling entity (1:1 binding)

  // === POSIX process semantics ===
  int32_t exit_code; // exit code, valid when ZOMBIE
  pid_t sid;         // session ID
  pid_t pgid;        // process group ID
  struct pty *ctty;  // controlling terminal

  // === signals (per-task + thread-group shared) ===
  uint64_t sig_pending;     // per-task private pending (tgkill/pthread_kill)
  sigset_t sig_blocked;     // per-task signal block mask
  siginfo_t sig_force_info; // force_sig scratch (existing)
  struct signal_struct *signal; // thread-group shared (fork: independent copy;
                                // CLONE_SIGHAND: ref++)

  // === fd table (dynamically allocated, separated from mm) ===
  struct files *files; // fork: deep copy; clone(CLONE_FILES): ref++

  // === threading support ===
  void *clear_tid_addr; // S03: CLONE_CHILD_CLEARTID user address (64-bit; was
                        // pid_t, truncating higher-half user pointers). The
                        // kernel writes int 0 here on thread exit + futex_wake.
  list_node futex_node; // futex bucket list node
  uint64_t
      futex_uaddr; // user address being waited on (0 = not waiting on futex)

  // === pthread cancel (Phase 4) ===
  uint64_t cancel_handler; // __pthread_cancel_check function address, 0 = not
                           // registered

  // === POSIX identity & permissions (group 1-2) ===
  // FAT32 has no on-disk permission bits, so uid/gid/mode live only in the
  // in-memory inode cache. These fields gate the getters/setters and umask.
  // uint32_t (not uid_t/gid_t/mode_t) keeps kernel/driver/bsd_types.h's
  // byte-identical mirror free of user-side sys/types.h includes.
  uint32_t uid;   // real UID (default 0)
  uint32_t euid;  // effective UID (default 0)
  uint32_t suid;  // saved-set UID (Linux permission ladder: euid!=0 can only
                  // raise euid back to uid or suid)
  uint32_t gid;   // real GID (default 0)
  uint32_t egid;  // effective GID (default 0)
  uint32_t sgid;  // saved-set GID (Linux permission ladder)
  uint32_t umask; // file creation mask (default 0022)

  // === clone exit signal (S19) ===
  // Low byte of clone flags. The thread group leader's exit notifies the parent
  // with this signal (do_exit step 7). 0 = do not notify (CLONE_THREAD forces
  // 0; a thread exit is reported via clear_tid_addr + futex, not a signal).
  // fork/proc_create default to SIGCHLD. uint8_t suffices: NSIG=65,
  // SIGRTMAX=64.
  uint8_t exit_signal;
  // M2-A: set at exec commit. setpgid(child,...) after the child has exec'd
  // returns EACCES (POSIX); this flag is the signal that the child crossed the
  // point-of-no-return and may no longer be reparented into a new pgrp. Packed
  // into the alignment padding before rlimit_nofile_cur (keeps sizeof(proc)
  // and the files/signal offset asserts unchanged).
  uint8_t did_exec;

  // === RLIMIT_NOFILE (per-process) ===
  // prlimit64 set/get round-trip. The OS does not enforce rlimits (the fd table
  // is a fixed MAX_FD array), but a lowered soft limit must be reported back on
  // get so callers see the value they set. 0 = "use the MAX_FD default" (so
  // proc_create's zeroed kmalloc reads as the 1024 default until explicitly
  // set). rlim_t is u64 (matches musl <sys/resource.h>).
  uint64_t rlimit_nofile_cur; // 0 = default MAX_FD
  uint64_t rlimit_nofile_max; // 0 = default MAX_FD

  // === Working directory (cwd) ===
  // FAT32 has no dentry tree, so cwd is stored as an absolute path string.
  // Initialized to "/" by proc_create. 256 bytes covers typical path limits.
  char cwd[256];

  // === robust-futex list (musl pthread robust mutexes) ===
  // Per-thread head registered via set_robust_list(2). 0 = not registered
  // (the in-tree pthread never registers, so exit_robust_list is a no-op for
  // it). On thread exit the kernel walks the list and marks still-held robust
  // locks with FUTEX_OWNER_DIED + wakes one waiter.
  void *robust_list_head;
  size_t
      robust_list_len; // set_robust_list passes len; must equal sizeof(struct
                       // robust_list_head); 0 when head is NULL
} proc;

// ABI drift guard: kernel/driver/bsd_types.h maintains a parallel proc for
// driver callbacks. If either definition drifts, the STATIC_ASSERT below will
// fail at compile time. The two must stay byte-for-byte identical.
//
// `files` is the field driver callbacks reach via proc->proc->files — its
// offset is the one that actually bit us historically (a stale driver-side
// copy inlined signal_struct and shifted files ~1000 bytes, causing OOB reads
// that silently returned garbage pointers). Pin it explicitly.
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
STATIC_ASSERT(
    offsetof(proc, files) == 184,
    "proc.files offset changed — update kernel/driver/bsd_types.h to match");
STATIC_ASSERT(
    offsetof(proc, signal) == 176,
    "proc.signal must be a POINTER to a separately-allocated signal_struct, "
    "not an inline struct — inlining shifts the offset of files");
STATIC_ASSERT(sizeof(proc) == 552,
              "proc size changed — update kernel/driver/bsd_types.h to match");
#undef STATIC_ASSERT

// Process lifecycle (BSD layer is the sole entry for process creation,
// calls Xcore KPI xtask_alloc then wraps with POSIX data)
proc *proc_create(void);     // calls xtask_alloc + kmalloc proc + bidirectional
                             // binding + files_create
void proc_free(proc *bp);    // files_put + xtask_free + kfree
void proc_reap(xtask *proc); // POSIX cleanup: close fds, free proc (called
                             // from sched_task_reap)
void proc_reap_idle(void);   // idle hook: scan for orphaned zombies

// sync_file fd install (plan2): driver calls this instead of touching the fd
// table directly. The fd holds a ref on `fence` (caller takes it); released on
// close by file_put's FD_SYNC_FILE case. drm_fence is opaque here.
struct drm_fence;
int bsd_sync_file_fd_install(xtask *proc, struct drm_fence *fence);

// Process creation (kernel/bsd/proc_create.c)
xtask *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);

// Build child kernel stack from parent trapframe (used by sys_fork/sys_clone)
uint64_t build_kstack_from_tf(uint64_t k_stack_top, trapframe *parent_tf,
                              uint64_t new_rax);

// capable(cap): single chokepoint for privilege checks (CAP_* in
// xos/capability.h). Today equivalent to euid==0; future per-cap routing to a
// capability bitmap changes only the implementation, not call sites.
bool capable(int cap);

// sys_clone (Phase 3b)
int64_t sys_clone(int64_t flags, int64_t stack, int64_t parent_tid,
                  int64_t child_tid, int64_t tls);
void vfork_complete(xtask *child);

// Convenience macros (gradually replace current_task, eventually delete
// current_task alias)
#define current_xtask                                                          \
  get_cpu_local()->_cur_proc // Xcore perspective (defined in arch/x64/smp.h)
#define current_proc (current_xtask->proc) // BSD perspective

#endif // KERNEL_BSD_PROC_H
