#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H

#include "arch/x64/trap.h"
#include "kernel/proc.h"   // pid_t
#include <stddef.h>        // size_t

typedef void (*irq_handler_t)(trapframe_t *);
typedef int64_t (*syscall_fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

void register_irq(int vec, irq_handler_t fn);
void unregister_irq(int vec);
void trap_dispatch(trapframe_t *tf);
void syscall_dispatch(trapframe_t *tf);
void isr_init();
void xhci_poll();

// Syscalls (all 6-arg signatures — arg6 unused by most, compiler optimizes away)
int64_t sys_getpid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_yield(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_recv(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_req(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_resp(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_irq_bind(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_exit(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_notify(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_waitpid(int64_t arg1, int64_t arg2, int64_t, int64_t, int64_t, int64_t);
int64_t sys_mmap(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4, int64_t arg5, int64_t arg6);
int64_t sys_munmap(int64_t arg1, int64_t arg2, int64_t, int64_t, int64_t, int64_t);
int64_t sys_shm_create(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_shm_attach(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_pipe(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_write(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_read(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_close(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_gettime(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_clock(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_msg(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_msg_resp(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_ioperm(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_dup2(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fcntl(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_dma_alloc(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_dma_free(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_pci_dev_info(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_block_async(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_install_fd_impl(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_lseek(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_memfd_create(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_ftruncate(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_debug_memstat(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// VFS extended syscalls
int64_t sys_ioctl(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fstat(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fdev_pid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Signal syscalls
int64_t sys_kill(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_sigaction(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_sigreturn(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Fork/execve syscalls
int64_t sys_fork(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_execve(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Session/pgid syscalls
int64_t sys_setsid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_setpgid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_getpgid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_getsid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Socket syscalls (declared in kernel/socket.c)
int64_t sys_socket(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_bind(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_listen(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_accept(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_connect(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_socketpair(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_sendmsg(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_recvmsg(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_shutdown(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_poll(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Socket internal helpers (for sys_write/sys_read FD_SOCKET dispatch)
int64_t sock_write(struct unix_sock *sock, const void *buf, size_t len);
int64_t sock_read(struct unix_sock *sock, void *buf, size_t len);
void sock_close(struct unix_sock *sock);

// Notify a process: enqueue recv_msg_t + wake if WAIT_RECV
void notify_and_wake(pid_t target_pid, recv_msg_t *msg);

// Register a kernel pre-allocated SHM region (called by xhci_init)
void register_kernel_shm(int shm_id, struct shm *shm);

// Remove a PID from irq_owner (called by proc_reap)
void irq_owner_cleanup(pid_t pid);

// Check if an IRQ vector is owned by a user-space driver (returns owner PID, or -1 if free)
int irq_owner_check(int irq);

// Wake a process blocked on WAIT_PIPE (used by pipe close and proc_reap)
// Enqueues a RECV_NOTIFY message and wakes if in WAIT_RECV
void wake_process(pid_t pid);

// Wake blocked pipe peers based on fd direction flags
static inline void wake_pipe_peers(pipe_t *p, int fd_flags) {
    if (fd_flags & (O_WRONLY | O_RDWR)) {
        if (p->read_pid >= 0) wake_process(p->read_pid);
    }
    if (fd_flags & (O_RDONLY | O_RDWR)) {
        if (p->write_pid >= 0) wake_process(p->write_pid);
    }
}

// Kernel-internal: send a message to a user-space process and wait for reply.
// Called from syscall context (current_task is the caller).
// req/req_len: message buffer in kernel space; resp/resp_len: reply buffer (may be NULL).
// Returns: 0 on success, negative errno on error.
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                    void *resp, size_t resp_len);

// Signal trampoline: shared physical page mapped at SIG_TRAMPOLINE_ADDR in every process
extern uint64_t sig_trampoline_phys;
void sig_init();
void check_pending_signals(trapframe_t *tf);

#endif // KERNEL_TRAP_H
