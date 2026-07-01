#ifndef KERNEL_XCORE_XTASK_H
#define KERNEL_XCORE_XTASK_H

#include <stdint.h>
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/alloc.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "common/types.h"
#include "kernel/xcore/atomic.h"

typedef enum proc_state_t { UNUSED, READY, RUNNING, BLOCKED, ZOMBIE, REAPING } proc_state_t;
typedef enum wait_event_t { WAIT_NONE, WAIT_RECV, WAIT_REQ_REPLY, WAIT_CHILD, WAIT_PIPE, WAIT_MSG_REPLY, WAIT_POLL, WAIT_FUTEX } wait_event_t;

#define RECV_MSG_SIZE   64
#define RECV_QUEUE_SIZE 16
#define MAX_PROC 64

struct proc;  // forward declaration — Xcore does not interpret contents

typedef struct xtask_t {
    pid_t pid;                  // offset 0
    proc_state_t state;         // offset 4
    uint64_t k_rsp;             // offset 8 (switch_to: movq %rsp, 8(%rdi))
    uint64_t k_stack_top;       // offset 16
    uint64_t cr3;               // offset 24 (switch_to: movq 24(%rsi), %rax)
    uint64_t entry;             // user entry RIP
    wait_event_t wait_event;    // block reason
    pid_t tgid;                 // thread group ID
    struct mm_t *mm;            // address space pointer (NULL for idle)
    int assigned_cpu;           // which CPU this process runs on
    uint8_t *iopm;              // IOPM bitmap (NULL = deny all), 8KB if allocated

    list_node_t run_node;       // per-CPU run_queue
    list_node_t wait_node;      // per-CPU timer_queue
    uint64_t wait_deadline;     // sched_clock() nanosecond deadline
    uint8_t  wait_timed_out;    // 1 = timer expired wakeup, 0 = notify wakeup
    uint8_t  recv_intr;         // set by wake_process when WAIT_RECV

    // === unified recv queue ===
    uint8_t  recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE];
    uint32_t recv_head;
    uint32_t recv_tail;
    spinlock_t recv_lock;

    // === REQ state ===
    pid_t    req_caller_pid;
    void __user *req_reply_buf;
    size_t   req_reply_len;
    int32_t  req_result;
    pid_t    req_target_pid;

    // === MSG state ===
    void __user *msg_reply_buf;
    size_t   msg_reply_len;
    pid_t    msg_caller_pid;
    int32_t  msg_result;
    pid_t    msg_target_pid;

    // === CPU time accounting ===
    uint64_t cpu_time_ns;
    uint64_t last_sched;

    // === threading support (appended at end, preserves existing offsets) ===
    uint64_t fs_base;           // TLS base (FS_BASE MSR mirror), loaded by __trapret
    Page    *fpu_page;          // fxsave area page (创建时预分配：xcore_fpu_alloc 在
                                // process_create_elf/sys_fork 时调用 bfc_alloc_page(1))。
                                // Stores Page* (NOT data pointer) — use page_to_phys/phys_to_virt
                                // to obtain the fxsave area virtual address. Type prevents feeding
                                // a Page* to fxsave/fxrstor by accident. NULL = idle (无 FPU 状态)。

    // === exit code (valid when state == ZOMBIE) ===
    // Lives in xtask_t (static array, slot lifetime by tasks_lock) not proc_t
    // (kmalloc'd) so waitpid can read it safely without holding a proc_t ref.
    int32_t  exit_code;

    // === pointer to BSD extension data (Xcore does not interpret contents) ===
    struct proc *proc;  // NULL = idle/task without POSIX semantics
} xtask_t;

// STATIC_ASSERT: verify first 8 fields offset match old task_t exactly
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
STATIC_ASSERT(offsetof(xtask_t, pid) == 0, "xtask_t.pid offset must be 0");
STATIC_ASSERT(offsetof(xtask_t, state) == 4, "xtask_t.state offset must be 4");
STATIC_ASSERT(offsetof(xtask_t, k_rsp) == 8, "xtask_t.k_rsp offset must be 8");
STATIC_ASSERT(offsetof(xtask_t, k_stack_top) == 16, "xtask_t.k_stack_top offset must be 16");
STATIC_ASSERT(offsetof(xtask_t, cr3) == 24, "xtask_t.cr3 offset must be 24");

// fs_base offset is consumed by arch/x64/trapentry.S (wrmsr FS_BASE on
// return to user mode). Export as a linker symbol so asm can load it
// without hardcoding the constant (struct layout may drift).
extern const uint64_t xtask_fs_base_offset;

extern xtask_t tasks[MAX_PROC];
extern spinlock_t tasks_lock;
extern pid_t init_pid;

#endif // KERNEL_XCORE_XTASK_H
