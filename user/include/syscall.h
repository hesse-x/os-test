/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

/*
 * User-space semantic syscall wrappers (sys_getpid, sys_recv, ...).
 *
 * These issue the raw syscall via the __syscallN inline-assembly wrappers from
 * xos/syscall_asm.h and translate the unified return convention
 * (>=0 success, -errno failure) into the libc-style (-1 + errno) convention.
 *
 * This is the userspace counterpart to the kernel's UAPI headers; it is NOT a
 * standard POSIX header (glibc has no <syscall.h> at this path) and is meant
 * for libc internals and programs that issue syscalls directly.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/prctl.h> // PR_* constants (sys_prctl)
#include <xos/sched.h> // SCHED_* constants
#include <xos/signal.h>
#include <xos/syscall.h> // struct kernel_mem_stats (UAPI, shared layout)
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>
#include <xos/thread.h> // struct thread_clone_info (sys_pthread_setup)
#include <xos/time.h>   // struct timespec (sys_clock_gettime / sys_ppoll)

#ifdef __cplusplus
extern "C" {
#endif

// errno (the macro + __errno_location decl) is provided by <errno.h>, included
// above — libc internals get it from there rather than redefining it.
// Redefining errno here made iwyu oscillate ("add syscall.h / remove errno.h" ↔
// reverse) because errno then had two canonical providers.

#ifdef __cplusplus
}
#endif

// ===================== Semantic wrappers (user-space only)
// =====================

// --- pid/yield (always succeed) ---
static inline int64_t sys_getpid() { return __syscall0(SYS_GETPID); }

#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#endif
#ifndef ARCH_GET_FS
#define ARCH_GET_FS 0x1003
#endif

static inline int64_t sys_arch_prctl(int64_t code, int64_t addr) {
  return __syscall2(SYS_ARCH_PRCTL, code, addr);
}

static inline void sys_yield() { __syscall0(SYS_SCHED_YIELD); }

// --- IPC status-only: recv/req/resp/msg/msg_resp/irq_bind ---
static inline int sys_recv(void *buf, void *data_buf, size_t data_buf_len,
                           uint32_t timeout_ms) {
  int64_t r = __syscall4(SYS_RECV, (int64_t)(uintptr_t)buf,
                         (int64_t)(uintptr_t)data_buf, (int64_t)data_buf_len,
                         (int64_t)timeout_ms);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_req(int32_t pid, void *request, void *reply) {
  int64_t r = __syscall3(SYS_REQ, (int64_t)pid, (int64_t)(uintptr_t)request,
                         (int64_t)(uintptr_t)reply);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_resp(void *reply, size_t reply_len, int32_t result) {
  int64_t r = __syscall3(SYS_RESP, (int64_t)(uintptr_t)reply,
                         (int64_t)reply_len, (int64_t)result);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_msg(int32_t target_pid, void *msg_buf, size_t msg_len,
                          void *reply_buf, size_t reply_len) {
  int64_t r = __syscall5(SYS_MSG, (int64_t)target_pid,
                         (int64_t)(uintptr_t)msg_buf, (int64_t)msg_len,
                         (int64_t)(uintptr_t)reply_buf, (int64_t)reply_len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_msg_resp(void *resp_buf, size_t resp_len) {
  int64_t r =
      __syscall2(SYS_MSG_RESP, (int64_t)(uintptr_t)resp_buf, (int64_t)resp_len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_irq_bind(int irq) {
  int64_t r = __syscall1(SYS_IRQ_BIND, (int64_t)irq);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- exit (does not return) ---
static inline void sys_exit(int32_t exit_code) {
  __syscall1(SYS_EXIT, (int64_t)exit_code);
  // does not return
}

// --- fork/waitpid/execve ---
// 统一走 wait4(§4.3):wait4(pid,wstatus,options,NULL) ≡ waitpid
static inline int64_t sys_waitpid(int32_t pid, int32_t *exit_code,
                                  int options) {
  int64_t r = __syscall4(SYS_WAIT4, (int64_t)pid, (int64_t)(uintptr_t)exit_code,
                         (int64_t)options, 0);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

static inline int64_t sys_fork(void) { return __syscall0(SYS_FORK); }

static inline int sys_execve(const char *pathname, char *const argv[],
                             char *const envp[]) {
  int64_t r = __syscall3(SYS_EXECVE, (int64_t)(uintptr_t)pathname,
                         (int64_t)(uintptr_t)argv, (int64_t)(uintptr_t)envp);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- mmap/munmap ---
static inline void *sys_mmap(void *addr, size_t size, int prot, int flags,
                             int fd, uint64_t offset) {
  int64_t r =
      __syscall6(SYS_MMAP, (int64_t)(uintptr_t)addr, (int64_t)size,
                 (int64_t)prot, (int64_t)flags, (int64_t)fd, (int64_t)offset);
  if (r < 0) {
    errno = -(int)r;
    return MAP_FAILED;
  }
  return (void *)(uintptr_t)r;
}

static inline int sys_munmap(void *addr, size_t size) {
  int64_t r = __syscall2(SYS_MUNMAP, (int64_t)(uintptr_t)addr, (int64_t)size);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// mremap stub: kernel returns -ENOSYS. Mirrors Linux mremap signature
// (old, old_size, new_size, flags, new_addr). On failure sets errno + returns
// MAP_FAILED, matching musl pthread_getattr_np.c:19's
// `mremap()==MAP_FAILED && errno==ENOMEM` loop (which then exits cleanly —
// errno here is ENOSYS, not ENOMEM, so no infinite probe loop).
static inline void *sys_mremap(void *old, size_t old_size, size_t new_size,
                               int flags, void *new_addr) {
  int64_t r = __syscall5(SYS_MREMAP, (int64_t)(uintptr_t)old, (int64_t)old_size,
                         (int64_t)new_size, (int64_t)flags,
                         (int64_t)(uintptr_t)new_addr);
  if (r < 0) {
    errno = -(int)r;
    return MAP_FAILED;
  }
  return (void *)(uintptr_t)r;
}

static inline int sys_mprotect(void *addr, size_t size, int prot) {
  int64_t r = __syscall3(SYS_MPROTECT, (int64_t)(uintptr_t)addr, (int64_t)size,
                         (int64_t)prot);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline long sys_sysconf(int name) {
  int64_t r = __syscall1(SYS_SYSCONF, (int64_t)name);
  // sysconf is special: it does NOT set errno on unsupported names (POSIX),
  // and kernel sys_sysconf returns -1 directly (not a negative errno) for
  // unknown names. Treat any negative return as "unsupported → -1".
  if (r < 0)
    return -1;
  return (long)r;
}

// --- memfd_create/ftruncate ---
static inline int sys_pipe(int *fd_ptr) {
  int64_t r = __syscall1(SYS_PIPE, (int64_t)(uintptr_t)fd_ptr);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_pipe2(int *fd_ptr, int flags) {
  int64_t r = __syscall2(SYS_PIPE2, (int64_t)(uintptr_t)fd_ptr, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- write/read/lseek ---
static inline int64_t sys_write(int fd, const void *buf, size_t len) {
  int64_t r =
      __syscall3(SYS_WRITE, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

static inline int64_t sys_read(int fd, void *buf, size_t len) {
  int64_t r =
      __syscall3(SYS_READ, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

// --- close/notify ---
static inline int sys_close(int fd) {
  int64_t r = __syscall1(SYS_CLOSE, (int64_t)fd);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_notify(int32_t pid) {
  int64_t r = __syscall1(SYS_NOTIFY, (int64_t)pid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- ioperm/dup2/fcntl ---
static inline int sys_ioperm(unsigned long from, unsigned long num,
                             int turn_on) {
  int64_t r =
      __syscall3(SYS_IOPERM, (int64_t)from, (int64_t)num, (int64_t)turn_on);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_dup2(int old_fd, int new_fd) {
  int64_t r = __syscall2(SYS_DUP2, (int64_t)old_fd, (int64_t)new_fd);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_dup(int old_fd) {
  int64_t r = __syscall1(SYS_DUP, (int64_t)old_fd);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_fcntl(int fd, int cmd, int64_t arg) {
  int64_t r = __syscall3(SYS_FCNTL, (int64_t)fd, (int64_t)cmd, arg);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- dma_alloc/dma_free ---
static inline int sys_dma_alloc(size_t size, void **vaddr, uint64_t *paddr) {
  int64_t r = __syscall3(SYS_DMA_ALLOC, (int64_t)size,
                         (int64_t)(uintptr_t)vaddr, (int64_t)(uintptr_t)paddr);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_dma_free(void *vaddr) {
  int64_t r = __syscall1(SYS_DMA_FREE, (int64_t)(uintptr_t)vaddr);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- pci_dev_info ---
static inline int sys_pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func,
                                   struct pci_dev_info *out) {
  int64_t r = __syscall4(SYS_PCI_DEV_INFO, (int64_t)bus, (int64_t)dev,
                         (int64_t)func, (int64_t)(uintptr_t)out);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- block_async (returns cookie on success) ---
// Async block I/O: returns cookie (>0) on success, -1 on error (errno set).
// Completion delivered via RECV_NOTIFY with cookie+result+lba+count in data.
static inline int sys_block_async(uint32_t lba, void *buf, uint32_t count,
                                  uint8_t dir) {
  int64_t r = __syscall4(SYS_BLOCK_ASYNC, (int64_t)lba, (int64_t)(uintptr_t)buf,
                         (int64_t)count, (int64_t)dir);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- install_fd (returns fd on success) ---
// sys_install_fd(fs_pid, fs_fd, offset, flags, file_size) — SYS_INSTALL_FD
// Register an FD_FILE fd in the kernel fd_table.
// Returns: fd (>=3) on success, -1 on failure (errno set)
static inline int sys_install_fd(int32_t fs_pid, int32_t fs_fd, uint64_t offset,
                                 int flags, uint64_t file_size) {
  int64_t r = __syscall5(SYS_INSTALL_FD, (int64_t)fs_pid, (int64_t)fs_fd,
                         (int64_t)offset, (int64_t)flags, (int64_t)file_size);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- lseek (returns offset on success) ---
static inline int64_t sys_lseek(int fd, int64_t offset, int whence) {
  int64_t r =
      __syscall3(SYS_LSEEK, (int64_t)fd, (int64_t)offset, (int64_t)whence);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

// --- memfd_create/ftruncate ---
static inline int sys_memfd_create(const char *name, unsigned int flags) {
  int64_t r =
      __syscall2(SYS_MEMFD_CREATE, (int64_t)(uintptr_t)name, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_ftruncate(int fd, int64_t size) {
  int64_t r = __syscall2(SYS_FTRUNCATE, (int64_t)fd, (int64_t)size);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- Signal syscalls ---
struct sigaction; // forward declaration
static inline int sys_kill(int32_t pid, int sig) {
  int64_t r = __syscall2(SYS_KILL, (int64_t)pid, (int64_t)sig);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sigaction(int sig, const struct sigaction *act,
                                struct sigaction *oldact) {
  int64_t r =
      __syscall4(SYS_RT_SIGACTION, (int64_t)sig, (int64_t)(uintptr_t)act,
                 (int64_t)(uintptr_t)oldact, (int64_t)sizeof(sigset_t));
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sigreturn(void) {
  int64_t r = __syscall0(SYS_RT_SIGRETURN);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sigaltstack(const stack_t *ss, stack_t *old_ss) {
  int64_t r = __syscall2(SYS_SIGALTSTACK, (int64_t)(uintptr_t)ss,
                         (int64_t)(uintptr_t)old_ss);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_debug_memstat(struct kernel_mem_stats *buf, int len) {
  int64_t r =
      __syscall2(SYS_DEBUG_MEMSTAT, (int64_t)(uintptr_t)buf, (int64_t)len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- VFS syscalls ---
// S08: open passes the creation mode (3rd arg) through to the kernel so
// sys_open's umask application sees the real mode. __syscall2 here would leave
// rdx (the kernel's arg3) holding whatever garbage it had on entry, so the
// created file's permission bits were garbage (different per call path).
static inline int sys_open(const char *path, int flags, int mode) {
  int64_t r = __syscall3(SYS_OPEN, (int64_t)(uintptr_t)path, (int64_t)flags,
                         (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_stat(const char *path, void *stat_buf) {
  int64_t r = __syscall2(SYS_STAT, (int64_t)(uintptr_t)path,
                         (int64_t)(uintptr_t)stat_buf);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- Linux 薄封装(§4.1/§4.2):at 变体,仅支持 AT_FDCWD ---
// 固定 4 参(非变参):grep openat user/ 为空,当前无调用方,避免 va_arg 陷阱
static inline int sys_openat(int dirfd, const char *path, int flags, int mode) {
  int64_t r = __syscall4(SYS_OPENAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                         (int64_t)flags, (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_newfstatat(int dirfd, const char *path, void *buf,
                                 int flags) {
  int64_t r =
      __syscall4(SYS_NEWFSTATAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                 (int64_t)(uintptr_t)buf, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// statx(dirfd, path, flags, mask, buf) — 唯一的元数据 syscall；stat/fstat 等
// legacy 接口在 libc 内全部经由此实现（见 user/lib/file.cc）。
static inline int sys_statx(int dirfd, const char *path, int flags,
                            unsigned int mask, void *buf) {
  int64_t r =
      __syscall5(SYS_STATX, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                 (int64_t)flags, (int64_t)mask, (int64_t)(uintptr_t)buf);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// S07: *at dirfd-relative variants (sys_mkdirat/sys_unlinkat/sys_renameat).
static inline int sys_mkdirat(int dirfd, const char *path, int mode) {
  int64_t r = __syscall3(SYS_MKDIRAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                         (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_unlinkat(int dirfd, const char *path, int flags) {
  int64_t r = __syscall3(SYS_UNLINKAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                         (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_renameat(int olddirfd, const char *oldpath, int newdirfd,
                               const char *newpath) {
  int64_t r =
      __syscall4(SYS_RENAMEAT, (int64_t)olddirfd, (int64_t)(uintptr_t)oldpath,
                 (int64_t)newdirfd, (int64_t)(uintptr_t)newpath);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- Linux 薄封装(§4.4):clock_gettime(clk,&ts) ---
static inline int sys_clock_gettime(int clk, struct timespec *ts) {
  int64_t r =
      __syscall2(SYS_CLOCK_GETTIME, (int64_t)clk, (int64_t)(uintptr_t)ts);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- OS 独有(§4.5):pthread 创建线程前预置 thread_clone_info ---
static inline int sys_pthread_setup(struct thread_clone_info *ci) {
  int64_t r = __syscall1(SYS_PTHREAD_SETUP, (int64_t)(uintptr_t)ci);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_mkdir(const char *path, uint32_t mode) {
  int64_t r = __syscall2(SYS_MKDIR, (int64_t)(uintptr_t)path, (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_mknod(const char *path, uint32_t mode, uint64_t dev) {
  int64_t r = __syscall3(SYS_MKNOD, (int64_t)(uintptr_t)path, (int64_t)mode,
                         (int64_t)dev);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_unlink(const char *path) {
  int64_t r = __syscall1(SYS_UNLINK, (int64_t)(uintptr_t)path);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// access(2)/faccessat(2)/utimensat(2) — path-based inode 元数据/时间戳
// syscall 薄封装(对齐 sys_unlink 模式)。errno 转换在 user/lib/file.cc 的
// POSIX 封装层。
static inline int sys_access(const char *path, int mode) {
  int64_t r = __syscall2(SYS_ACCESS, (int64_t)(uintptr_t)path, (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_faccessat(int dirfd, const char *path, int mode,
                                int flags) {
  int64_t r =
      __syscall4(SYS_FACCESSAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                 (int64_t)mode, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// sys_faccessat2 — SYS_FACCESSAT2 (439). LLVM libc hard-requires this entry
// (faccessat.cpp #errors without it); the kernel handler is a verbatim alias
// of faccessat with flags honoured.
static inline int sys_faccessat2(int dirfd, const char *path, int mode,
                                 int flags) {
  int64_t r =
      __syscall4(SYS_FACCESSAT2, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                 (int64_t)mode, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// sys_statfs / sys_fstatfs — filesystem statistics (SYS_STATFS 137 /
// SYS_FSTATFS 138). buf is struct statfs * (xos/statfs.h); passed as void *
// here to avoid pulling the layout header into every syscall.h consumer.
static inline int sys_statfs(const char *path, void *buf) {
  int64_t r =
      __syscall2(SYS_STATFS, (int64_t)(uintptr_t)path, (int64_t)(uintptr_t)buf);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_fstatfs(int fd, void *buf) {
  int64_t r = __syscall2(SYS_FSTATFS, (int64_t)fd, (int64_t)(uintptr_t)buf);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_utimensat(int dirfd, const char *path,
                                const struct timespec times[2], int flags) {
  int64_t r =
      __syscall4(SYS_UTIMENSAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                 (int64_t)(uintptr_t)times, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// chmod(2)/fchmod(2)/fchmodat(2) — 落盘仅内存(与 utimensat 一致);setuid 位
// 清除规则见 kernel/bsd/syscall.c apply_chmod。errno 转换在 POSIX 封装层。
static inline int sys_chmod(const char *path, unsigned int mode) {
  int64_t r = __syscall2(SYS_CHMOD, (int64_t)(uintptr_t)path, (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int sys_fchmod(int fd, unsigned int mode) {
  int64_t r = __syscall2(SYS_FCHMOD, (int64_t)fd, (int64_t)mode);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int sys_fchmodat(int dirfd, const char *path, unsigned int mode,
                               int flags) {
  int64_t r = __syscall4(SYS_FCHMODAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                         (int64_t)mode, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// chown(2)/fchown(2)/fchownat(2) — 权限简化为 root-only(CAP_CHOWN);
// (uid_t)-1/(gid_t)-1 = 该字段不变。errno 转换在 POSIX 封装层。
static inline int sys_chown(const char *path, unsigned int owner,
                            unsigned int group) {
  int64_t r = __syscall3(SYS_CHOWN, (int64_t)(uintptr_t)path, (int64_t)owner,
                         (int64_t)group);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int sys_fchown(int fd, unsigned int owner, unsigned int group) {
  int64_t r =
      __syscall3(SYS_FCHOWN, (int64_t)fd, (int64_t)owner, (int64_t)group);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int sys_fchownat(int dirfd, const char *path, unsigned int owner,
                               unsigned int group, int flags) {
  int64_t r = __syscall5(SYS_FCHOWNAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                         (int64_t)owner, (int64_t)group, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_rename(const char *oldpath, const char *newpath) {
  int64_t r = __syscall2(SYS_RENAME, (int64_t)(uintptr_t)oldpath,
                         (int64_t)(uintptr_t)newpath);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// symlink(2)/readlink(2) — path-based 链接/读取软链 target(§3.3)。errno
// 转换在 user/lib/file.cc 的 POSIX 封装层。
static inline int sys_symlink(const char *target, const char *linkpath) {
  int64_t r = __syscall2(SYS_SYMLINK, (int64_t)(uintptr_t)target,
                         (int64_t)(uintptr_t)linkpath);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int sys_symlinkat(const char *target, int newdirfd,
                                const char *linkpath) {
  int64_t r = __syscall3(SYS_SYMLINKAT, (int64_t)(uintptr_t)target,
                         (int64_t)newdirfd, (int64_t)(uintptr_t)linkpath);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int64_t sys_readlink(const char *path, char *buf, size_t bufsiz) {
  int64_t r = __syscall3(SYS_READLINK, (int64_t)(uintptr_t)path,
                         (int64_t)(uintptr_t)buf, (int64_t)bufsiz);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}
static inline int64_t sys_readlinkat(int dirfd, const char *path, char *buf,
                                     size_t bufsiz) {
  int64_t r =
      __syscall4(SYS_READLINKAT, (int64_t)dirfd, (int64_t)(uintptr_t)path,
                 (int64_t)(uintptr_t)buf, (int64_t)bufsiz);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

// link(2)/linkat(2) — path-based 硬链接(§3.4 nlink 全链路)。errno 转换在
// user/lib/file.cc 的 POSIX 封装层。
static inline int sys_link(const char *oldpath, const char *newpath) {
  int64_t r = __syscall2(SYS_LINK, (int64_t)(uintptr_t)oldpath,
                         (int64_t)(uintptr_t)newpath);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}
static inline int sys_linkat(int olddirfd, const char *oldpath, int newdirfd,
                             const char *newpath, int flags) {
  int64_t r = __syscall5(SYS_LINKAT, (int64_t)olddirfd,
                         (int64_t)(uintptr_t)oldpath, (int64_t)newdirfd,
                         (int64_t)(uintptr_t)newpath, (int64_t)flags);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_rmdir(const char *path) {
  int64_t r = __syscall1(SYS_RMDIR, (int64_t)(uintptr_t)path);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_dev_create(const char *name, int shm_fd, uint32_t minor) {
  int64_t r = __syscall3(SYS_DEV_CREATE, (int64_t)(uintptr_t)name,
                         (int64_t)shm_fd, (int64_t)minor);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_getdents(int fd, void *buf, size_t len) {
  int64_t r = __syscall3(SYS_GETDENTS64, (int64_t)fd, (int64_t)(uintptr_t)buf,
                         (int64_t)len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- ioctl/fstat/fdev_pid ---
static inline int64_t sys_ioctl(int fd, uint32_t cmd, uint64_t arg) {
  int64_t r = __syscall3(SYS_IOCTL, (int64_t)fd, (int64_t)cmd, arg);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

static inline int64_t sys_fstat(int fd, uint64_t buf) {
  int64_t r = __syscall2(SYS_FSTAT, (int64_t)fd, buf);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

static inline int64_t sys_fdev_pid(int fd) {
  int64_t r = __syscall1(SYS_FDEV_PID, (int64_t)fd);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

// --- Session/pgid syscalls ---
static inline int64_t sys_setsid() {
  int64_t r = __syscall0(SYS_SETSID);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

static inline int sys_setpgid(uint64_t pid, uint64_t pgid) {
  int64_t r = __syscall2(SYS_SETPGID, (int64_t)pid, (int64_t)pgid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int64_t sys_getpgid(uint64_t pid) {
  int64_t r = __syscall1(SYS_GETPGID, (int64_t)pid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

static inline int64_t sys_getsid(uint64_t pid) {
  int64_t r = __syscall1(SYS_GETSID, (int64_t)pid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return r;
}

// --- Thread syscalls (Phase 4 pthread support) ---
static inline int64_t sys_clone(uint64_t flags, uint64_t stack,
                                uint64_t parent_tid, uint64_t child_tid,
                                uint64_t tls) {
  return __syscall5(SYS_CLONE, (int64_t)flags, (int64_t)stack,
                    (int64_t)parent_tid, (int64_t)child_tid, (int64_t)tls);
}

static inline int sys_futex(uint32_t *uaddr, int op, uint32_t val,
                            const void *timeout, uint32_t *uaddr2,
                            uint32_t val3) {
  int64_t r = __syscall6(SYS_FUTEX, (int64_t)(uintptr_t)uaddr, (int64_t)op,
                         (int64_t)val, (int64_t)(uintptr_t)timeout,
                         (int64_t)(uintptr_t)uaddr2, (int64_t)val3);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_tgkill(int32_t tgid, int32_t tid, int sig) {
  int64_t r = __syscall3(SYS_TGKILL, (int64_t)tgid, (int64_t)tid, (int64_t)sig);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_tkill(int32_t tid, int sig) {
  int64_t r = __syscall2(SYS_TKILL, (int64_t)tid, (int64_t)sig);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline void sys_exit_group(int32_t status) {
  __syscall1(SYS_EXIT_GROUP, (int64_t)status);
  __builtin_unreachable();
}

static inline int sys_set_tid_address(uint64_t tidptr) {
  int64_t r = __syscall1(SYS_SET_TID_ADDRESS, (int64_t)tidptr);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int64_t sys_gettid(void) { return __syscall0(SYS_GETTID); }

static inline int sys_sigprocmask(int how, const sigset_t *set,
                                  sigset_t *oldset) {
  int64_t r =
      __syscall4(SYS_RT_SIGPROCMASK, (int64_t)how, (int64_t)(uintptr_t)set,
                 (int64_t)(uintptr_t)oldset, (int64_t)sizeof(sigset_t));
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_pthread_set_cancel_handler(uint64_t handler) {
  int64_t r = __syscall1(SYS_PTHREAD_SET_CANCEL_HANDLER, (int64_t)handler);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

// --- POSIX identity & permissions (group 1) ---
// Identity getters never fail; return the raw value.
static inline int64_t sys_getuid(void) { return __syscall0(SYS_GETUID); }
static inline int64_t sys_geteuid(void) { return __syscall0(SYS_GETEUID); }
static inline int64_t sys_getgid(void) { return __syscall0(SYS_GETGID); }
static inline int64_t sys_getegid(void) { return __syscall0(SYS_GETEGID); }
static inline int64_t sys_getppid(void) { return __syscall0(SYS_GETPPID); }
static inline int64_t sys_getpgrp(void) { return __syscall0(SYS_GETPGRP); }
// umask returns the previous mask (always succeeds).
static inline int64_t sys_umask(int mode) {
  return __syscall1(SYS_UMASK, (int64_t)mode);
}

static inline int sys_setuid(uint32_t uid) {
  int64_t r = __syscall1(SYS_SETUID, (int64_t)uid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_setgid(uint32_t gid) {
  int64_t r = __syscall1(SYS_SETGID, (int64_t)gid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_gethostname(char *buf, size_t len) {
  int64_t r =
      __syscall2(SYS_GETHOSTNAME, (int64_t)(uintptr_t)buf, (int64_t)len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sethostname(const char *name, size_t len) {
  int64_t r =
      __syscall2(SYS_SETHOSTNAME, (int64_t)(uintptr_t)name, (int64_t)len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- alarm / pause (group 2) ---
// alarm returns the seconds remaining on the previous alarm (0 if none).
static inline int64_t sys_alarm(unsigned seconds) {
  return __syscall1(SYS_ALARM, (int64_t)seconds);
}

// pause returns -1 with EINTR when interrupted by a signal.
static inline int sys_pause(void) {
  int64_t r = __syscall0(SYS_PAUSE);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// --- truncate / fsync / sync (group 3) ---
static inline int sys_truncate(const char *path, int64_t len) {
  int64_t r = __syscall2(SYS_TRUNCATE, (int64_t)(uintptr_t)path, len);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_fsync(int fd) {
  int64_t r = __syscall1(SYS_FSYNC, (int64_t)fd);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sync(void) {
  __syscall0(SYS_SYNC);
  return 0;
}

// --- POSIX signal (group 4) ---
// sigpending: read the set of pending signals (per-task + shared, including
// blocked) into *set. Returns 0 on success, -1/errno on failure.
static inline int sys_sigpending(sigset_t *set) {
  int64_t r = __syscall2(SYS_RT_SIGPENDING, (int64_t)(uintptr_t)set,
                         (int64_t)sizeof(sigset_t));
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_mount(const char *source, const char *target,
                            const char *fstype, unsigned long flags,
                            const void *data) {
  int64_t r = __syscall5(SYS_MOUNT, (int64_t)(uintptr_t)source,
                         (int64_t)(uintptr_t)target, (int64_t)(uintptr_t)fstype,
                         (int64_t)flags, (int64_t)(uintptr_t)data);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int64_t sys_dev_set_meta(const char *name, const char *subsys,
                                       const char *devtype, const void *props) {
  return __syscall4(SYS_DEV_SET_META, (int64_t)(uintptr_t)name,
                    (int64_t)(uintptr_t)subsys, (int64_t)(uintptr_t)devtype,
                    (int64_t)(uintptr_t)props);
}

// ===================== chdir / fchdir / getcwd (group 5) =====================
static inline int sys_getcwd(char *buf, size_t size) {
  int64_t r = __syscall2(SYS_GETCWD, (int64_t)(uintptr_t)buf, (int64_t)size);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_chdir(const char *path) {
  int64_t r = __syscall1(SYS_CHDIR, (int64_t)(uintptr_t)path);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_fchdir(int fd) {
  int64_t r = __syscall1(SYS_FCHDIR, (int64_t)fd);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// ===================== sched_* (group 6) =====================
struct sched_param {
  int sched_priority;
};

static inline int sys_sched_setparam(pid_t pid,
                                     const struct sched_param *param) {
  int64_t r =
      __syscall2(SYS_SCHED_SETPARAM, (int64_t)pid, (int64_t)(uintptr_t)param);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sched_getparam(pid_t pid, struct sched_param *param) {
  int64_t r =
      __syscall2(SYS_SCHED_GETPARAM, (int64_t)pid, (int64_t)(uintptr_t)param);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sched_setscheduler(pid_t pid, int policy,
                                         const struct sched_param *param) {
  int64_t r = __syscall3(SYS_SCHED_SETSCHEDULER, (int64_t)pid, (int64_t)policy,
                         (int64_t)(uintptr_t)param);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_sched_getscheduler(pid_t pid) {
  int64_t r = __syscall1(SYS_SCHED_GETSCHEDULER, (int64_t)pid);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_sched_get_priority_max(int policy) {
  int64_t r = __syscall1(SYS_SCHED_GET_PRIORITY_MAX, (int64_t)policy);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_sched_get_priority_min(int policy) {
  int64_t r = __syscall1(SYS_SCHED_GET_PRIORITY_MIN, (int64_t)policy);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

static inline int sys_sched_rr_get_interval(pid_t pid, struct timespec *tp) {
  int64_t r = __syscall2(SYS_SCHED_RR_GET_INTERVAL, (int64_t)pid,
                         (int64_t)(uintptr_t)tp);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sched_setaffinity(pid_t pid, size_t cpusetsize,
                                        const uint64_t *mask) {
  int64_t r = __syscall3(SYS_SCHED_SETAFFINITY, (int64_t)pid,
                         (int64_t)cpusetsize, (int64_t)(uintptr_t)mask);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

static inline int sys_sched_getaffinity(pid_t pid, size_t cpusetsize,
                                        uint64_t *mask) {
  int64_t r = __syscall3(SYS_SCHED_GETAFFINITY, (int64_t)pid,
                         (int64_t)cpusetsize, (int64_t)(uintptr_t)mask);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// ===================== getcpu (group 6) =====================
static inline int sys_getcpu(uint32_t *cpu, uint32_t *node) {
  int64_t r =
      __syscall2(SYS_GETCPU, (int64_t)(uintptr_t)cpu, (int64_t)(uintptr_t)node);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// ===================== prctl (group 7) =====================
static inline int sys_prctl(int option, uint64_t arg2, uint64_t arg3,
                            uint64_t arg4, uint64_t arg5) {
  int64_t r = __syscall5(SYS_PRCTL, (int64_t)option, (int64_t)arg2,
                         (int64_t)arg3, (int64_t)arg4, (int64_t)arg5);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// ===================== ppoll (group 8) =====================
// Linux signature: ppoll(fds, nfds, timeout, sigmask, sigsetsize)
struct pollfd; // forward decl; include <poll.h> for full definition
static inline int sys_ppoll(struct pollfd *fds, uint64_t nfds,
                            const struct timespec *timeout,
                            const sigset_t *sigmask, size_t sigsetsize) {
  int64_t r = __syscall5(SYS_PPOLL, (int64_t)(uintptr_t)fds, (int64_t)nfds,
                         (int64_t)(uintptr_t)timeout,
                         (int64_t)(uintptr_t)sigmask, (int64_t)sigsetsize);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return (int)r;
}

#endif // USER_SYSCALL_H
