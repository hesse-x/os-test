/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_XTASK_H
#define KERNEL_XCORE_XTASK_H

#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/sparse.h"
#include <stdint.h>
#include <xos/types.h>

typedef enum proc_state {
  UNUSED,
  READY,
  RUNNING,
  BLOCKED,
  ZOMBIE,
  REAPING
} proc_state;
typedef enum wait_event {
  WAIT_NONE,
  WAIT_RECV,
  WAIT_REQ_REPLY,
  WAIT_CHILD,
  WAIT_MSG_REPLY,
  WAIT_POLL,
  WAIT_FUTEX,
  WAIT_PAUSE, // pause(): block until any signal; may carry an alarm deadline
} wait_event;

#define RECV_MSG_SIZE 64
#define RECV_QUEUE_SIZE 16
#define MAX_PROC 1024

struct proc; // forward declaration — Xcore does not interpret contents
struct file; // forward declaration — ipcfd_file pointer (defined in
             // kernel/bsd/types.h)

typedef struct xtask {
  pid_t pid;             // offset 0
  proc_state state;      // offset 4
  uint64_t k_rsp;        // offset 8 (switch_to: movq %rsp, 8(%rdi))
  uint64_t k_stack_top;  // offset 16
  uint64_t cr3;          // offset 24 (switch_to: movq 24(%rsi), %rax)
  uint64_t entry;        // user entry RIP
  wait_event wait_event; // block reason
  pid_t tgid;            // thread group ID
  struct mm *mm;         // address space pointer (NULL for idle)
  int assigned_cpu;      // which CPU this process runs on
  uint8_t *iopm;         // IOPM bitmap (NULL = deny all), 8KB if allocated

  list_node run_node;      // per-CPU run_queue
  list_node wait_node;     // per-CPU timer_queue
  uint64_t wait_deadline;  // sched_clock() nanosecond deadline
  uint64_t alarm_deadline; // 0 = no alarm; else sched_clock() ns absolute
                           // (POSIX alarm(); checked by timer_handler)
  uint8_t wait_timed_out;  // 1 = timer expired wakeup, 0 = notify wakeup

  // === unified recv queue ===
  uint8_t recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE];
  uint32_t recv_head;
  uint32_t recv_tail;
  spinlock recv_lock;

  // === REQ state ===
  pid_t req_caller_pid;
  void __user *req_reply_buf;
  size_t req_reply_len;
  int32_t req_result;
  pid_t req_target_pid;
  uint8_t
      req_replied; // set under caller's scheduler_lock by sys_resp before the
                   // wake-check; caller re-checks it under the same lock after
                   // arming WAIT_REQ_REPLY to close the lost-wake window
                   // (result==0 on success is ambiguous as a "replied" signal)

  // === MSG state ===
  void __user *msg_reply_buf;
  size_t msg_reply_len;
  pid_t msg_caller_pid;
  int32_t msg_result;
  pid_t msg_target_pid;
  uint8_t msg_replied; // set under caller's scheduler_lock by sys_msg_resp;
                       // same lost-wake guard as req_replied for WAIT_MSG_REPLY

  // === ipcfd (evdev downstream-IPC fd) ===
  // Points at the FD_IPC file bound to this task's recv queue (NULL if none).
  // Set by sys_ipcfd_create (holds a file reference); cleared by the fd's
  // close/file_put path.  Enqueue paths (sys_req/notify/resp/msg_to/msg_resp)
  // check this to wake the ipcfd wq alongside WAIT_RECV (evdev_refact.md
  // §5.6).  Stored as struct file* with a refcount, NOT a task pid — the wq
  // lives in the file, and the reference keeps the file alive while a
  // pending enqueue might wake it.
  struct file *ipcfd_file;

  // === CPU time accounting ===
  uint64_t cpu_time_ns;
  uint64_t last_sched;

  // === threading support (appended at end, preserves existing offsets) ===
  uint64_t fs_base;      // TLS base (FS_BASE MSR mirror), loaded by __trapret
  struct page *fpu_page; // fxsave area page (pre-allocated at creation time:
                         // xcore_fpu_alloc calls bfc_alloc_page(1) during
                         // process_create_elf/sys_fork).  Stores struct page *
                         // (NOT data pointer) — use page_to_phys/phys_to_virt
                         // to obtain the fxsave area virtual address. Type
                         // prevents feeding a struct page * to fxsave/fxrstor
                         // by accident. NULL = idle (no FPU state).

  // === exit code (valid when state == ZOMBIE) ===
  // Lives in xtask (static array, slot lifetime by tasks_lock) not proc
  // (kmalloc'd) so waitpid can read it safely without holding a proc ref.
  int32_t exit_code;

  // === thread cleanup ownership (set by clone, read by sched_task_reap) ===
  int detached;             // 1 = sched_task_reap owns TLS/stack unmap
  uint64_t tls_page;        // user vaddr of TLS+TCB page (0 if N/A)
  size_t tls_total;         // size of TLS+TCB mapping
  uint64_t user_stack_base; // user vaddr of stack base (incl guard)
  size_t user_stack_size;   // stack+guard total size

  uint8_t need_resched; // 1 = current task must yield, checked at sched exit
  // === pointer to BSD extension data (Xcore does not interpret contents) ===
  struct proc *proc; // NULL = idle/task without POSIX semantics
} xtask;

// STATIC_ASSERT: verify first 8 fields offset match old task_t exactly
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
STATIC_ASSERT(offsetof(xtask, pid) == 0, "xtask.pid offset must be 0");
STATIC_ASSERT(offsetof(xtask, state) == 4, "xtask.state offset must be 4");
STATIC_ASSERT(offsetof(xtask, k_rsp) == 8, "xtask.k_rsp offset must be 8");
STATIC_ASSERT(offsetof(xtask, k_stack_top) == 16,
              "xtask.k_stack_top offset must be 16");
STATIC_ASSERT(offsetof(xtask, cr3) == 24, "xtask.cr3 offset must be 24");

// fs_base offset is consumed by arch/x64/trapentry.S (wrmsr FS_BASE on
// return to user mode). Export as a linker symbol so asm can load it
// without hardcoding the constant (struct layout may drift).
extern const uint64_t xtask_fs_base_offset;

// ===================== Process table: pointer array (dynamic)
// ===================== tasks[i] points to dynamically allocated xtask (from
// xtask_cache slab), NULL = empty slot.  Retains pid == array-index semantics
// (112+ instances of tasks[pid]->field unchanged).  Slot reuse:
//   REAPING slot: kmem_cache_free then re-alloc via xtask_alloc
//   NULL slot: alloc directly
// pid allocation: incrementing next_pid + circular reuse (avoids immediate
// pid reuse after exit causing stale waitpid).
extern xtask *tasks[MAX_PROC];
extern spinlock tasks_lock;
extern pid_t next_pid;
extern pid_t init_pid;

// task_get: pid -> xtask* unified access helper.
// debug: validates pid is valid and slot non-NULL (encodes lock-free-read
// semantics constraint); panics with location on failure.
// release: bare pointer, zero overhead (mirrors CLAUDE.md debug/release
// dual mode).
// Usage: tasks[pid].field -> task_get(pid)->field;
//        &tasks[pid] (assign xtask*) -> task_get(pid);
//        &tasks[pid].run_node (field addr) -> &task_get(pid)->run_node.
static inline xtask *task_get(pid_t pid) {
#ifndef NDEBUG
  ASSERT(pid >= 0 && pid < MAX_PROC);
  ASSERT(tasks[pid] != NULL);
#endif
  return tasks[pid];
}

// Prevent blindly increasing MAX_PROC: pointer array is 8KB (1024*8);
// exceeding 4096 would require RCU-complex dynamic expansion (see "won't do").
// Larger capacity should follow the pidhash route (route B).
_Static_assert(MAX_PROC <= 4096, "MAX_PROC too large — consider pidhash route");

#endif // KERNEL_XCORE_XTASK_H
