/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_SYSCALL_H
#define KERNEL_BSD_SYSCALL_H

#include "arch/x64/trap.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>
#include <stdint.h>

// Syscall dispatch entry
int64_t syscall_dispatch(trapframe *tf);

// BSD layer syscall function declarations (grouped by subsystem)
// fd operations
int64_t sys_pipe(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_write(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_read(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_close(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_dup2(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fcntl(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// process semantics
int64_t sys_exit(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_exit_group(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
// do_exit_with_code: common exit body, receives an **already-encoded**
// exit_code (D13). sys_exit (user-space code<<8) and signal.c death-by-signal
// (sig & 0x7f) encode separately before calling it.
int64_t do_exit_with_code(int32_t encoded_exit_code);
int64_t sys_waitpid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fork(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_execve(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_mmap(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_munmap(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// VFS
int64_t sys_ioctl(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fstat(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_fdev_pid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_lseek(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_memfd_create(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_ftruncate(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// DMA/PCI/Block
int64_t sys_dma_alloc(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_dma_free(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_pci_dev_info(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_block_async(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_install_fd_impl(int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t);
int64_t sys_debug_memstat(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// signals
int64_t sys_kill(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_sigaction(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_sigreturn(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
void check_pending_signals(trapframe *tf);
void force_sig(xtask *proc, int sig, int si_code, void *si_addr);
void deliver_signal_to(xtask *target, int sig);
int pgsignal(pid_t pgid, int sig);

// Session/pgid
int64_t sys_setsid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_setpgid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_getpgid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_getsid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Socket (declared in socket.h, not repeated here)

#endif // KERNEL_BSD_SYSCALL_H
