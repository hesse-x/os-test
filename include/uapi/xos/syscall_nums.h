/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_SYSCALL_NUMS_H
#define COMMON_SYSCALL_NUMS_H

#include <stddef.h>
#include <stdint.h>
#include <xos/ioctl.h>

// ===================== Syscall numbers (NR_SYSCALL=69, 0-68 continuous)
// =====================
#define SYS_GETPID 0
#define SYS_YIELD 1
#define SYS_RECV 2
#define SYS_REQ 3
#define SYS_RESP 4
#define SYS_IRQ_BIND 5
#define SYS_EXIT 6
#define SYS_WAITPID 7
#define SYS_MMAP 8
#define SYS_MUNMAP 9
#define SYS_PIPE 10
#define SYS_WRITE 11
#define SYS_READ 12
#define SYS_CLOSE 13
#define SYS_NOTIFY 14
#define SYS_GETTIME 15
#define SYS_CLOCK 16
#define SYS_MSG 17
#define SYS_MSG_RESP 18
#define SYS_IOPERM 19
#define SYS_DUP2 20
#define SYS_FCNTL 21
#define SYS_DMA_ALLOC 22
#define SYS_DMA_FREE 23
#define SYS_PCI_DEV_INFO 24
#define SYS_BLOCK_ASYNC 25
#define SYS_INSTALL_FD 26
#define SYS_SOCKET 27
#define SYS_BIND 28
#define SYS_LISTEN 29
#define SYS_ACCEPT 30
#define SYS_CONNECT 31
#define SYS_SOCKETPAIR 32
#define SYS_SENDMSG 33
#define SYS_RECVMSG 34
#define SYS_SHUTDOWN 35
#define SYS_POLL 36
#define SYS_LSEEK 37
#define SYS_MEMFD_CREATE 38
#define SYS_FTRUNCATE 39
#define SYS_KILL 40
#define SYS_SIGACTION 41
#define SYS_SIGRETURN 42
#define SYS_DEBUG_MEMSTAT 43

// ===================== VFS syscall numbers =====================
#define SYS_OPEN 44
#define SYS_STAT 45
#define SYS_MKDIR 46
#define SYS_UNLINK 47
#define SYS_RMDIR 48
#define SYS_DEV_CREATE 49
#define SYS_GETDENTS 50
#define SYS_IOCTL 51
#define SYS_FSTAT 52
#define SYS_FDEV_PID 53
#define SYS_FORK 54
#define SYS_EXECVE 55
#define SYS_SETSID 56
#define SYS_SETPGID 57
#define SYS_GETPGID 58
#define SYS_GETSID 59

// ===================== Thread syscalls (阶段 3a/3b) =====================
#define SYS_CLONE 60
#define SYS_FUTEX 61
#define SYS_ARCH_PRCTL 62
#define SYS_TGKILL 63
#define SYS_EXIT_GROUP 64
#define SYS_SET_TID_ADDRESS 65
#define SYS_GETTID 66
#define SYS_SIGPROCMASK 67
#define SYS_PTHREAD_SET_CANCEL_HANDLER 68

// ===================== recv_msg (shared between kernel and user)
// =====================
#define RECV_IRQ 0
#define RECV_REQ 1
#define RECV_NOTIFY 2
#define RECV_MSG 3

typedef struct recv_msg {
  uint32_t type; // RECV_IRQ / RECV_REQ / RECV_NOTIFY / RECV_MSG
  uint32_t src;  // IRQ number or sender PID
  union {
    uint8_t data[56]; // RECV_IRQ / RECV_REQ / RECV_NOTIFY
    struct {          // RECV_MSG only
      void *kmaddr;   // kernel kmalloc buffer (opaque to user)
      size_t len;     // data length
    } msg;
  };
} recv_msg_t;

// ===================== PCI device info =====================
typedef struct pci_dev_info_bar {
  uint64_t phys;
  uint64_t size;
  uint8_t type; // 0=MMIO32, 1=IO, 2=MMIO64
} pci_dev_info_bar_t;

typedef struct pci_dev_info {
  uint16_t vendor_id;
  uint16_t device_id;
  uint16_t class_code;
  uint8_t num_bars;
  struct pci_dev_info_bar bars[6];
} pci_dev_info_t;

// ===================== Block I/O constants =====================
#define BLOCK_DIR_READ 0
#define BLOCK_DIR_WRITE 1

// ===================== lseek constants =====================
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* COMMON_SYSCALL_NUMS_H */
