#ifndef KERNEL_BSD_PROC_H
#define KERNEL_BSD_PROC_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/xcore/xtask.h"
#include "kernel/xcore/list.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/signal.h"
#include "common/signal.h"

typedef struct proc {
    struct xtask_t *xtask;      // reverse reference to scheduling entity (1:1 binding)

    // === POSIX process semantics ===
    int32_t exit_code;          // exit code, valid when ZOMBIE
    pid_t sid;                  // session ID
    pid_t pgid;                 // process group ID
    struct pty *ctty;           // controlling terminal

    // === signals (per-task + thread-group shared) ===
    uint64_t      sig_pending;  // per-task private pending (tgkill/pthread_kill)
    sigset_t      sig_blocked;  // per-task signal block mask
    siginfo_t     sig_force_info;  // force_sig scratch (existing)
    struct signal_struct *signal;  // thread-group shared (fork: independent copy; CLONE_SIGHAND: ref++)

    // === fd table (dynamically allocated, separated from mm_t) ===
    struct files_t *files;      // fork: deep copy; clone(CLONE_FILES): ref++

    // === threading support ===
    pid_t    clear_tid_addr;    // CLONE_CHILD_CLEARTID user address (0 = none)
    list_node_t futex_node;     // futex bucket list node
    uint64_t futex_uaddr;       // user address being waited on (0 = not waiting on futex)
} proc_t;

// ABI drift guard: kernel/driver/bsd_types.h maintains a parallel proc_t for
// driver callbacks. If either definition drifts, the STATIC_ASSERT below will
// fail at compile time. The two must stay byte-for-byte identical.
//
// `files` is the field driver callbacks reach via proc->proc->files — its
// offset is the one that actually bit us historically (a stale driver-side
// copy inlined signal_struct and shifted files ~1000 bytes, causing OOB reads
// that silently returned garbage pointers). Pin it explicitly.
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
STATIC_ASSERT(offsetof(proc_t, files) == 184,
              "proc_t.files offset changed — update kernel/driver/bsd_types.h to match");
STATIC_ASSERT(offsetof(proc_t, signal) == 176,
              "proc_t.signal must be a POINTER to a separately-allocated signal_struct, "
              "not an inline struct — inlining shifts the offset of files");
STATIC_ASSERT(sizeof(proc_t) == 224,
              "proc_t size changed — update kernel/driver/bsd_types.h to match");
#undef STATIC_ASSERT

// Process lifecycle (BSD layer is the sole entry for process creation,
// calls Xcore KPI xtask_alloc then wraps with POSIX data)
proc_t *proc_create(void);      // calls xtask_alloc + kmalloc proc + bidirectional binding + files_create
void proc_free(proc_t *bp);     // files_put + xtask_free + kfree
void proc_reap(xtask_t *proc);  // POSIX cleanup: close fds, free proc (called from task_reap)
void proc_reap_idle(void);      // idle hook: scan for orphaned zombies

// Process creation (kernel/bsd/proc_create.c)
xtask_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);

// Build child kernel stack from parent trapframe (used by sys_fork/sys_clone)
uint64_t build_kstack_from_tf(uint64_t k_stack_top, trapframe_t *parent_tf, uint64_t new_rax);

// sys_clone (阶段 3b)
int64_t sys_clone(int64_t flags, int64_t stack, int64_t parent_tid,
                  int64_t child_tid, int64_t tls, int64_t _u6);

// Convenience macros (gradually replace current_task, eventually delete current_task alias)
#define current_xtask  get_cpu_local()->_cur_proc  // Xcore perspective (defined in arch/x64/smp.h)
#define current_proc  (current_xtask->proc)          // BSD perspective

#endif // KERNEL_BSD_PROC_H
