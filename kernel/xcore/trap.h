#ifndef KERNEL_XCORE_TRAP_H
#define KERNEL_XCORE_TRAP_H

#include "arch/x64/trap.h"
#include "kernel/xcore/xtask.h"
#include "xos/syscall_nums.h"
#include <stddef.h>

// Hook registration points: BSD layer registers during init, Xcore calls at trap_dispatch end
typedef void (*signal_check_fn)(xtask_t *t, trapframe_t *tf);
extern signal_check_fn signal_check_hook;

typedef int (*fault_handler_fn)(uint64_t vaddr, xtask_t *t);
extern fault_handler_fn fault_handler;

typedef void (*reap_fn)(void);
extern reap_fn reap_hook;

// Per-process cleanup (called from task_reap)
typedef void (*proc_reap_fn)(xtask_t *t);
extern proc_reap_fn proc_reap_hook;

// devtmpfs cleanup for a PID (called from task_reap / mm_release)
typedef void (*devtmpfs_cleanup_fn)(pid_t pid);
extern devtmpfs_cleanup_fn devtmpfs_cleanup_hook;

// BSD syscall dispatch (called from xcall_dispatch for non-Xcore syscalls)
typedef int64_t (*syscall_dispatch_fn)(trapframe_t *tf);
extern syscall_dispatch_fn syscall_dispatch_hook;

// Signal pending check (called from IPC to detect interruptible waits)
typedef bool (*signal_pending_fn)(xtask_t *t);
extern signal_pending_fn signal_pending_hook;

// Force signal delivery (called from trap exception handling)
typedef void (*force_sig_fn)(xtask_t *t, int sig, int si_code, void *si_addr);
extern force_sig_fn force_sig_hook;

// Driver timer poll hook (called periodically from timer IRQ handler)
typedef void (*timer_poll_fn)(void);
extern timer_poll_fn timer_poll_hook;

// trap entry and syscall dispatch entry
void trap_dispatch(trapframe_t *tf);
void xcall_dispatch(trapframe_t *tf);
void isr_init(void);
void sig_init(void);

// Xcore IPC syscall function declarations
int64_t sys_getpid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_yield(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_recv(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_req(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_resp(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_irq_bind(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_notify(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_msg(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_msg_resp(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_msg_to(pid_t target_pid, void *msg_buf, size_t msg_len,
                   void *reply_buf, size_t reply_len);
int64_t sys_gettime(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_clock(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_ioperm(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_gettid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// IRQ registration
#define MAX_IRQ_HANDLERS 256
typedef void (*irq_handler_t)(trapframe_t *);
typedef int64_t (*syscall_fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
void register_irq(int vec, irq_handler_t fn);
void unregister_irq(int vec);
int irq_owner_check(int irq);
void irq_owner_cleanup(pid_t pid);
int irq_has_handler(int irq);
extern pid_t irq_owner[MAX_IRQ_HANDLERS];

// IPC primitives (exported by Xcore layer)
void notify_and_wake(pid_t target_pid, recv_msg_t *msg);
void wake_process(pid_t pid);
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                    void *resp, size_t resp_len);

// SHM (kernel internal)
uint64_t shm_alloc_pages(uint64_t npages);
struct shm *shm_create_internal(uint64_t npages);
struct shm *shm_get(struct shm *shm);
void shm_put(struct shm *shm);
uint64_t shm_add_page(struct shm *shm);
extern uint64_t sig_trampoline_phys;

const char *syscall_name(uint64_t nr);

#endif // KERNEL_XCORE_TRAP_H
