#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H

#include "arch/x64/trap.h"
#include "kernel/proc.h"   // pid_t
#include <stddef.h>        // size_t

typedef void (*irq_handler_t)(trapframe_t *);
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

extern "C" {
void register_irq(int vec, irq_handler_t fn);
void trap_dispatch(trapframe_t *tf);
void syscall_dispatch(trapframe_t *tf);
void isr_init();
void xhci_poll();

// Syscalls (all 6-arg signatures — arg6 unused by most, compiler optimizes away)
uint64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_recv(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_req(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_resp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_exit(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_waitpid(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_spawn(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
uint64_t sys_munmap(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_fb_info(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_shm_create(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_shm_attach(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_pipe(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t, uint64_t);
uint64_t sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t, uint64_t);
uint64_t sys_close(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_load_dev(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dev_msg(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t);
uint64_t sys_gettime(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_clock(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_msg(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_msg_resp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_ioperm(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dup2(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_fcntl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dma_alloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_dma_free(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_pci_dev_info(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_block_read(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_block_write(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_block_async(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_open_dev(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_install_fd_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Socket syscalls (declared in kernel/socket.cc)
uint64_t sys_socket(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_bind(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_listen(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_accept(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_connect(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_socketpair(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_sendmsg(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_recvmsg(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_shutdown(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_poll(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Socket internal helpers (for sys_write/sys_read FD_SOCKET dispatch)
int64_t sock_write(struct unix_sock *sock, const void *buf, size_t len);
int64_t sock_read(struct unix_sock *sock, void *buf, size_t len);
void sock_close(struct unix_sock *sock);

// Notify a process: enqueue recv_msg + wake if WAIT_RECV
void notify_and_wake(pid_t target_pid, recv_msg *msg);

// Register a driver PID for a device type (kernel-internal, not a syscall)
int register_dev(int dev_type, pid_t pid);

// Register a kernel pre-allocated SHM region (called by xhci_init)
void register_kernel_shm(int shm_id, struct shm *shm);

// Look up a device driver PID (kernel-internal, used by ISR)
pid_t lookup_dev(int dev_type);

// Remove a PID from dev_table (called by proc_reap)
void dev_table_cleanup(pid_t pid);

// Remove a PID from irq_owner (called by proc_reap)
void irq_owner_cleanup(pid_t pid);

// Wake a process blocked on WAIT_PIPE (used by pipe close and proc_reap)
// Enqueues a RECV_NOTIFY message and wakes if in WAIT_RECV
void wake_process(pid_t pid);

// Kernel-internal: send a message to a user-space process and wait for reply.
// Called from syscall context (current_proc is the caller).
// req/req_len: message buffer in kernel space; resp/resp_len: reply buffer (may be NULL).
// Returns: 0 on success, negative errno on error.
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                    void *resp, size_t resp_len);
}

#endif // KERNEL_TRAP_H
