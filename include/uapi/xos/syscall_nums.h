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

// ===================== Linux x86-64 syscall numbers =====================
// 对齐 /usr/include/x86_64-linux-gnu/asm/unistd_64.h。
// 内核分发表与用户态共用本宏;llvm-libc 经宿主 <sys/syscall.h> 取 __NR_* 直达,
// 内核收到的号即 Linux 号。仅定义本 OS 已实现的 syscall;未实现的 Linux 号
// 留空(dispatch default 返 -ENOSYS)。
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_IOCTL 16
#define SYS_PIPE 22
#define SYS_SCHED_YIELD 24
#define SYS_PAUSE 34
#define SYS_DUP2 33
#define SYS_ALARM 37
#define SYS_GETPID 39
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_SENDMSG 46
#define SYS_RECVMSG 47
#define SYS_SHUTDOWN 48
#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_SOCKETPAIR 53
#define SYS_CLONE 56
#define SYS_FORK 57
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_KILL 62
#define SYS_FCNTL 72
#define SYS_FSYNC 74
#define SYS_TRUNCATE 76
#define SYS_FTRUNCATE 77
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_UNLINK 87
#define SYS_MKNOD 133
#define SYS_SIGALTSTACK 131
#define SYS_UMASK 95
#define SYS_SETHOSTNAME 170
#define SYS_IOPERM 173
#define SYS_ARCH_PRCTL 158
#define SYS_GETTID 186
#define SYS_SET_TID_ADDRESS 218
#define SYS_GETDENTS64 217
#define SYS_FUTEX 202
#define SYS_EXIT_GROUP 231
#define SYS_EPOLL_WAIT 232
#define SYS_EPOLL_CTL 233
#define SYS_TGKILL 234
#define SYS_CLOCK_GETTIME 228
#define SYS_EPOLL_PWAIT 281
#define SYS_TIMERFD_CREATE 283
#define SYS_TIMERFD_SETTIME 286
#define SYS_SIGNALFD4 289
#define SYS_EVENTFD2 290
#define SYS_EPOLL_CREATE1 291
#define SYS_OPENAT 257
#define SYS_NEWFSTATAT 262
#define SYS_EPOLL_CREATE 213
#define SYS_GETRANDOM 318
#define SYS_MEMFD_CREATE 319
#define SYS_SETSID 112
#define SYS_GETSID 124
#define SYS_SETPGID 109
#define SYS_GETPGID 121
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_GETUID 102
#define SYS_GETEUID 107
#define SYS_GETGID 104
#define SYS_GETEGID 108
#define SYS_SETUID 105
#define SYS_SETGID 106
#define SYS_RT_SIGPENDING 127
#define SYS_SYNC 162
#define SYS_MOUNT 165

// ---- ENOSYS stubs (C group) ----
#define SYS_SENDFILE 40
#define SYS_LINK 86
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91
#define SYS_CHOWN 92
#define SYS_FCHOWN 93
#define SYS_LINKAT 265
#define SYS_SYMLINKAT 266
#define SYS_READLINKAT 267
#define SYS_FCHMODAT 268
#define SYS_FCHOWNAT 260
#define SYS_UTIMENSAT 280
#define SYS_CLOCK_SETTIME 227
#define SYS_GETITIMER 36
#define SYS_SETITIMER 38

// ---- trivial-return stubs (C2 group: return 0, not -ENOSYS) ----
#define SYS_ACCESS 21
#define SYS_FACCESSAT 269

// ---- Thin wrappers (A group) ----
#define SYS_DUP 32
#define SYS_DUP3 292
#define SYS_MKDIRAT 258
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_RECVFROM 45
#define SYS_SENDTO 44
#define SYS_GETTIMEOFDAY 96

// ---- Simple kernel implementations (B group) ----
#define SYS_PREAD64 17
#define SYS_PWRITE64 18
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_UNAME 63

// ---- Socket thin wrappers (F group) ----
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SETSOCKOPT 54
#define SYS_GETSOCKOPT 55

// ===================== OS-specific syscalls (1024+) =====================
// 给 Linux 预留空间:1024 之前不占任何 OS 独有号;新增独有号往后追加。
#define SYS_OS_BASE 1024
#define SYS_REQ (SYS_OS_BASE + 0)
#define SYS_RESP (SYS_OS_BASE + 1)
#define SYS_RECV (SYS_OS_BASE + 2)
#define SYS_MSG (SYS_OS_BASE + 3)
#define SYS_MSG_RESP (SYS_OS_BASE + 4)
#define SYS_NOTIFY (SYS_OS_BASE + 5)
#define SYS_IRQ_BIND (SYS_OS_BASE + 6)
#define SYS_DMA_ALLOC (SYS_OS_BASE + 7)
#define SYS_DMA_FREE (SYS_OS_BASE + 8)
#define SYS_PCI_DEV_INFO (SYS_OS_BASE + 9)
#define SYS_BLOCK_ASYNC (SYS_OS_BASE + 10)
#define SYS_INSTALL_FD (SYS_OS_BASE + 11)
#define SYS_DEV_CREATE (SYS_OS_BASE + 12)
#define SYS_DEV_SET_META (SYS_OS_BASE + 13)
#define SYS_IPCFD_CREATE (SYS_OS_BASE + 14)
#define SYS_IPCFD_READ (SYS_OS_BASE + 15)
#define SYS_SYSCONF (SYS_OS_BASE + 16)
#define SYS_DEBUG_MEMSTAT (SYS_OS_BASE + 17)
#define SYS_PTHREAD_SET_CANCEL_HANDLER (SYS_OS_BASE + 18)
#define SYS_GETHOSTNAME (SYS_OS_BASE + 19)
#define SYS_PTHREAD_SETUP (SYS_OS_BASE + 20)
#define SYS_FDEV_PID (SYS_OS_BASE + 21)
#define SYS_OS_MAX SYS_FDEV_PID

// ===================== recv_msg (shared between kernel and user)
// =====================
#define RECV_IRQ 0
#define RECV_REQ 1
#define RECV_NOTIFY 2
#define RECV_MSG 3
#define RECV_IOCTL 4 // ioctl variable-length request (arg > 48B path)

typedef struct recv_msg {
  uint32_t type; // RECV_IRQ / RECV_REQ / RECV_NOTIFY / RECV_MSG / RECV_IOCTL
  uint32_t src;  // IRQ number or sender PID
  union {
    uint8_t data[56]; // RECV_IRQ / RECV_REQ / RECV_NOTIFY
    struct {          // RECV_MSG only
      void *kmaddr;   // kernel kmalloc buffer (opaque to user)
      size_t len;     // data length
    } msg;
    struct {             // RECV_IOCTL only (variable-length ioctl proxy path)
      uint32_t cmd;      // ioctl command number
      uint32_t arg_size; // arg data length
      void *kmaddr;      // kernel kmalloc buffer (arg data copy)
      size_t len;        // = arg_size (convenience for driver)
      uint32_t minor; // device minor for routing (evdev reads msg->ioctl.minor)
    } ioctl;
  };
} recv_msg;

// Guard the recv_msg ABI: it must serialize into RECV_MSG_SIZE (64) bytes.
#ifdef __cplusplus
static_assert(sizeof(recv_msg) == 64,
              "recv_msg must stay 64 bytes (RECV_MSG_SIZE)");
#else
_Static_assert(sizeof(recv_msg) == 64,
               "recv_msg must stay 64 bytes (RECV_MSG_SIZE)");
#endif

// ===================== PCI device info =====================
typedef struct pci_dev_info_bar {
  uint64_t phys;
  uint64_t size;
  uint8_t type; // 0=MMIO32, 1=IO, 2=MMIO64
} pci_dev_info_bar;

typedef struct pci_dev_info {
  uint16_t vendor_id;
  uint16_t device_id;
  uint16_t class_code;
  uint8_t num_bars;
  struct pci_dev_info_bar bars[6];
} pci_dev_info;

// ===================== Block I/O constants =====================
#define BLOCK_DIR_READ 0
#define BLOCK_DIR_WRITE 1

// ===================== lseek constants =====================
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* COMMON_SYSCALL_NUMS_H */
