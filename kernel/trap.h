#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H

#include "arch/x64/trap.h"
#include "kernel/proc.h"   // pid_t

typedef void (*irq_handler_t)(trapframe_t *);
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

extern "C" {
void register_irq(int vec, irq_handler_t fn);
void trap_dispatch(trapframe_t *tf);
void syscall_dispatch(trapframe_t *tf);
void isr_init();

// Syscalls
uint64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_recv(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_req(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_resp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_exit(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_waitpid(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t);
uint64_t sys_spawn(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t);
uint64_t sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
uint64_t sys_munmap(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t);
uint64_t sys_serial_write(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t);
uint64_t sys_fb_info(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_shm_create(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_shm_attach(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_pipe(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t);
uint64_t sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t);
uint64_t sys_close(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_load_dev(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t);
uint64_t sys_lookup_dev(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_gettime(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_clock(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_msg(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_msg_resp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_ioperm(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dup2(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_fcntl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dma_alloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dma_free(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Register a driver PID for a device type (kernel-internal, not a syscall)
int register_dev(int dev_type, pid_t pid);

// Remove a PID from dev_table (called by proc_reap)
void dev_table_cleanup(pid_t pid);

// Remove a PID from irq_owner (called by proc_reap)
void irq_owner_cleanup(pid_t pid);

// Wake a process blocked on WAIT_PIPE (used by pipe close and proc_reap)
// Enqueues a RECV_NOTIFY message and wakes if in WAIT_RECV
void wake_process(pid_t pid);
}

#endif // KERNEL_TRAP_H
