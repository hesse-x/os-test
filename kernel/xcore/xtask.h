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
  WAIT_PIPE,
  WAIT_MSG_REPLY,
  WAIT_POLL,
  WAIT_FUTEX
} wait_event;

#define RECV_MSG_SIZE 64
#define RECV_QUEUE_SIZE 16
#define MAX_PROC 1024

struct proc; // forward declaration — Xcore does not interpret contents

typedef struct xtask {
  pid_t pid;               // offset 0
  proc_state state;      // offset 4
  uint64_t k_rsp;          // offset 8 (switch_to: movq %rsp, 8(%rdi))
  uint64_t k_stack_top;    // offset 16
  uint64_t cr3;            // offset 24 (switch_to: movq 24(%rsi), %rax)
  uint64_t entry;          // user entry RIP
  wait_event wait_event; // block reason
  pid_t tgid;              // thread group ID
  struct mm *mm;         // address space pointer (NULL for idle)
  int assigned_cpu;        // which CPU this process runs on
  uint8_t *iopm;           // IOPM bitmap (NULL = deny all), 8KB if allocated

  list_node run_node;   // per-CPU run_queue
  list_node wait_node;  // per-CPU timer_queue
  uint64_t wait_deadline; // sched_clock() nanosecond deadline
  uint8_t wait_timed_out; // 1 = timer expired wakeup, 0 = notify wakeup
  uint8_t recv_intr;      // set by wake_process when WAIT_RECV

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

  // === MSG state ===
  void __user *msg_reply_buf;
  size_t msg_reply_len;
  pid_t msg_caller_pid;
  int32_t msg_result;
  pid_t msg_target_pid;

  // === CPU time accounting ===
  uint64_t cpu_time_ns;
  uint64_t last_sched;

  // === threading support (appended at end, preserves existing offsets) ===
  uint64_t fs_base; // TLS base (FS_BASE MSR mirror), loaded by __trapret
  Page *fpu_page; // fxsave area page (创建时预分配：xcore_fpu_alloc 在
                  // process_create_elf/sys_fork 时调用 bfc_alloc_page(1))。
                  // Stores Page* (NOT data pointer) — use
                  // page_to_phys/phys_to_virt to obtain the fxsave area virtual
                  // address. Type prevents feeding a Page* to fxsave/fxrstor by
                  // accident. NULL = idle (无 FPU 状态)。

  // === exit code (valid when state == ZOMBIE) ===
  // Lives in xtask (static array, slot lifetime by tasks_lock) not proc
  // (kmalloc'd) so waitpid can read it safely without holding a proc ref.
  int32_t exit_code;

  // === thread cleanup ownership (set by clone, read by sched_task_reap) ===
  int detached;             // 1 = sched_task_reap owns tls/stack unmap
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

// ===================== Process table: 指针数组（动态化）=====================
// tasks[i] 指向动态分配的 xtask（来自 xtask_cache 专用 slab），NULL = 空槽。
// 保留 pid == 数组下标 语义（112+ 处 tasks[pid]->field 不破坏）。slot 复用：
// REAPING 槽在 xtask_alloc 内 kmem_cache_free 后重新 alloc，NULL 槽直接 alloc。
// pid 分配：递增 next_pid + 环形复用（避免 exit 后 pid 立即复用致 waitpid
// 陈旧）。
extern xtask *tasks[MAX_PROC];
extern spinlock tasks_lock;
extern pid_t next_pid;
extern pid_t init_pid;

// task_get: pid → xtask* 的统一访问 helper。
// debug：校验 pid 合法且槽非空（编码"无锁读语义"约束），不合格 panic 定位。
// release：裸指针零开销（呼应 CLAUDE.md debug/release 双模式）。
// 用法：原 tasks[pid].field → task_get(pid)->field；
//       原 &tasks[pid]（赋值 xtask*）→ task_get(pid)；
//       原 &tasks[pid].run_node（取字段地址）→ &task_get(pid)->run_node。
static inline xtask *task_get(pid_t pid) {
#ifndef NDEBUG
  ASSERT(pid >= 0 && pid < MAX_PROC);
  ASSERT(tasks[pid] != NULL);
#endif
  return tasks[pid];
}

// 防盲目调大 MAX_PROC：指针数组常驻 8KB（1024*8），超过 4096 会滑向"动态扩容"
// 的 RCU 复杂度（见方案"不做的事"）。需要更大容量应走 pidhash 路线（路线 B）。
_Static_assert(MAX_PROC <= 4096, "MAX_PROC too large — consider pidhash route");

#endif // KERNEL_XCORE_XTASK_H
