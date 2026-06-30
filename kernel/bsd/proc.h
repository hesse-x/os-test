#ifndef KERNEL_BSD_PROC_H
#define KERNEL_BSD_PROC_H

#include <stdint.h>
#include "kernel/xcore/xtask.h"
#include "kernel/bsd/types.h"
#include "common/signal.h"

typedef struct proc {
    struct xtask_t *xtask;      // reverse reference to scheduling entity (1:1 binding)

    // === POSIX process semantics ===
    int32_t exit_code;          // exit code, valid when ZOMBIE
    pid_t sid;                  // session ID
    pid_t pgid;                 // process group ID
    struct pty *ctty;           // controlling terminal

    // === signals ===
    struct signal_state {
        uint64_t      pending;
        sigset_t      blocked;
        struct sigaction action[NSIG];
    } sig;
    siginfo_t sig_force_info;

    // === fd table (dynamically allocated, separated from mm_t) ===
    struct files_t *files;
} proc_t;

// Process lifecycle (BSD layer is the sole entry for process creation,
// calls Xcore KPI xtask_alloc then wraps with POSIX data)
proc_t *proc_create(void);      // calls xtask_alloc + kmalloc proc + bidirectional binding + files_create
void proc_free(proc_t *bp);     // files_put + xtask_free + kfree
void proc_reap(xtask_t *proc);  // POSIX cleanup: close fds, free proc (called from task_reap)
void proc_reap_idle(void);      // idle hook: scan for orphaned zombies

// Process creation (kernel/bsd/proc_create.c)
xtask_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);

// Convenience macros (gradually replace current_task, eventually delete current_task alias)
#define current_xtask  get_cpu_local()->_cur_proc  // Xcore perspective (defined in arch/x64/smp.h)
#define current_proc  (current_xtask->proc)          // BSD perspective

#endif // KERNEL_BSD_PROC_H
