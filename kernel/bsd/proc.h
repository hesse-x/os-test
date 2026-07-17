/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_PROC_H
#define KERNEL_BSD_PROC_H

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
  pid_t clear_tid_addr; // CLONE_CHILD_CLEARTID user address (0 = none)
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
  // (alarm_deadline lives in xtask — the Xcore timer handler needs to read it
  // without crossing into the BSD layer.)
  uint32_t uid;   // real UID (default 0)
  uint32_t euid;  // effective UID (default 0)
  uint32_t gid;   // real GID (default 0)
  uint32_t egid;  // effective GID (default 0)
  uint32_t umask; // file creation mask (default 0022)
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
STATIC_ASSERT(sizeof(proc) == 256,
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
struct drm_fence *bsd_sync_file_fd_fence(xtask *proc, int fd);

// Process creation (kernel/bsd/proc_create.c)
xtask *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);

// Build child kernel stack from parent trapframe (used by sys_fork/sys_clone)
uint64_t build_kstack_from_tf(uint64_t k_stack_top, trapframe *parent_tf,
                              uint64_t new_rax);

// sys_clone (Phase 3b)
int64_t sys_clone(int64_t flags, int64_t stack, int64_t parent_tid,
                  int64_t child_tid, int64_t tls, int64_t arg6);

// Convenience macros (gradually replace current_task, eventually delete
// current_task alias)
#define current_xtask                                                          \
  get_cpu_local()->_cur_proc // Xcore perspective (defined in arch/x64/smp.h)
#define current_proc (current_xtask->proc) // BSD perspective

#endif // KERNEL_BSD_PROC_H
