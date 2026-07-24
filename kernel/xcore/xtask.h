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
#include <xos/thread.h>
#include <xos/types.h>

typedef enum proc_state {
  UNUSED,
  READY,
  RUNNING,
  BLOCKED,
  ZOMBIE,
  REAPING,
  STOPPED // S01: stopped by SIGSTOP/SIGTSTP/SIGTTOU/SIGTTIN (job control). Not
          // scheduled, not on run_queue, not reapable. Resumed only by SIGCONT
          // (do_cont). Distinct from BLOCKED (which awaits a resource wake).
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
  WAIT_SLEEP, // nanosleep/clock_nanosleep: block until deadline or signal
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

  list_node run_node;     // per-CPU run_queue
  list_node wait_node;    // per-CPU timer_queue
  uint64_t wait_deadline; // sched_clock() nanosecond deadline
  // S03: alarm moved per-task → per-process (signal_struct.alarm_deadline).
  // POSIX alarm() is process-wide; the old per-task field only delivered
  // SIGALRM to the arming thread.
  uint8_t wait_timed_out; // 1 = timer expired wakeup, 0 = notify wakeup

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

  // (frame_opt.md 块四) deepest kernel-stack usage observed for this task
  // (k_stack_top - lowest RSP).  Updated by kstack_highwater_check() at the
  // schedule() entry choke point; warns when usage approaches the 16KB limit.
  uint64_t stack_highwater;

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

  // === job-control stop (S01, valid when state == STOPPED) ===
  // exit_code holds the WIFSTOPPED encoding ((stopsig << 8) | 0x7f) while
  // stopped; stop_signo is the bare signal for clarity. stop_reported gates
  // one-shot WUNTRACED reporting (cleared by do_cont / re-set by do_stop).
  int32_t stop_signo;
  uint8_t stop_reported;

  // === job-control continue (S19, valid after do_cont) ===
  // Set by do_cont when a STOPPED child is resumed (CLD_CONTINUED notify).
  // sys_waitpid(WCONTINUED) reports the child once, then clears it. One-shot,
  // mirroring stop_reported. When S01's do_cont lands it arms this; until then
  // WCONTINUED reporting stays a safe no-op (no writer).
  uint8_t cont_pending;

  // === thread cleanup ownership (set by clone, read by sched_task_reap) ===
  struct thread_clone_info
      pending_pthread_setup; // §4.5:由 sys_pthread_setup
                             // 预置,sys_clone(CLONE_THREAD) 消费即清
  int detached;              // 1 = sched_task_reap owns TLS/stack unmap
  uint64_t tls_page;         // user vaddr of TLS+TCB page (0 if N/A)
  size_t tls_total;          // size of TLS+TCB mapping
  uint64_t user_stack_base;  // user vaddr of stack base (incl guard)
  size_t user_stack_size;    // stack+guard total size

  uint8_t need_resched; // 1 = current task must yield, checked at sched exit

  // Linux TIF_SIGPENDING equivalent: a per-task boolean cached by
  // recalc_sigpending() (bsd/signal.c) so interruptible blocking waits can
  // cheaply test "any deliverable signal pending?" via signal_pending() without
  // touching BSD signal_struct state. Set/cleared only by the BSD signal path
  // (delivery, sigprocmask, rt_sigreturn, check_pending_signals drain); xcore
  // treats it as an opaque cached bit and never writes it.
  uint8_t sig_pending;

  // === sigaltstack (S04, per-thread) ===
  // Linux stores sas_ss_* per task_struct so each thread has its own alternate
  // signal stack. Mirrored here (not in the thread-group-shared signal_struct,
  // which would leak one thread's altstack to its siblings). fork/clone clear
  // these on the child (children do not inherit an altstack).
  void *sas_ss_sp;       // altstack base (user vaddr), NULL when disabled
  size_t sas_ss_size;    // altstack size in bytes (0 when disabled)
  uint32_t sas_ss_flags; // SS_ONSTACK (kernel-set while handler runs) /
                         // SS_DISABLE / SS_AUTODISARM

  // === pointer to BSD extension data (Xcore does not interpret contents) ===
  struct proc *proc; // NULL = idle/task without POSIX semantics

  // === exit handshake (SMP) ===
  // 0 → the dying CPU may STILL be executing this task's do_exit tail
  //     (parent notify / waiter wake) and its final schedule()/switch_to on
  //     this task's kernel stack, and may still write xtask fields (k_rsp
  //     store in switch_to, cpu-time accounting).  do_exit wakes the parent
  //     BEFORE the final switch, so a concurrent waitpid can reach REAPING
  //     while the dying CPU is still active — slot reuse at that point frees
  //     the kernel stack under a running CPU and lets its late writes land in
  //     the reused xtask object (bug.md §4 heap corruption).
  // 1 → the final context switch away from this task has completed (set by
  //     the abandoning CPU at its next schedule() entry, via
  //     cpu_local.pending_dead).  xtask_alloc must not reuse a REAPING slot
  //     until exit_done == 1.
  volatile uint32_t exit_done;

  // === SA_RESTART restart conduit (02) ===
  // Set by xcall_dispatch when a syscall returns -ERESTART (orig syscall nr);
  // consumed+cleared by check_pending_signals at delivery to rewind rip and
  // restore rax. Cleared on every syscall entry so a stale value from a prior
  // call cannot drive a bogus restart on a non-syscall entry path (page fault)
  // whose tf->rax coincidentally equals -ERESTART.
  int64_t restart_nr;
  uint8_t restart_armed;
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

// signal_pending(): Linux TIF_SIGPENDING test. A cheap per-task boolean cached
// by the BSD signal path's recalc_sigpending() so interruptible blocking waits
// (driver/xcore) can ask "should this sleep be interrupted by a signal?"
// without pulling in BSD signal_struct state. Lives in xcore because it is a
// scheduling concern, not a signal-subsystem implementation detail — drivers
// may call it without a layering violation. Returns false for tasks without a
// BSD proc.
static inline bool signal_pending(xtask *t) {
  return t && t->proc && t->sig_pending;
}

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
