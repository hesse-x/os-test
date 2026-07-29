/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// musl's <fcntl.h> (consumed via the shim) gates AT_EMPTY_PATH / F_SEAL_* etc.
// behind _GNU_SOURCE||_BSD_SOURCE; file.cc uses AT_EMPTY_PATH (fstat/fstatat).
// Define _DEFAULT_SOURCE before any include so AT_EMPTY_PATH is visible (musl
// features.h turns _DEFAULT_SOURCE into _BSD_SOURCE=1). _DEFAULT_SOURCE rather
// than _GNU_SOURCE: _GNU_SOURCE would also pull musl's __NEED_struct_iovec
// (via <fcntl.h>'s #ifdef _GNU_SOURCE block), colliding with <xos/socket.h>'s
// struct iovec. fcntl_worklist §3e feature-guard gap.
#define _DEFAULT_SOURCE

// libc file I/O: all file operations go through syscalls directly.
// Kernel handles FAT32, devtmpfs, pipes, sockets, etc.
// No libc-side fd_table — kernel's proc->fd_table is the single source of
// truth. open/openat/fcntl/creat/posix_fadvise/posix_fallocate are provided by
// musl src/fcntl/*.c (musl_fcntl_objs); chdir/getcwd/unlinkat/renameat by musl
// src/unistd/*.c (musl_unistd_objs). The kernel resolves relative paths against
// bp->cwd (vfs_resolve_user for plain syscalls, resolve_dirfd_start for *at
// syscalls), so no libc-side cwd copy is needed.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h> // IWYU pragma: keep
#include <syscall.h>
#include <termios.h>
#include <unistd.h>

#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/ioctl.h>
#include <xos/socket.h>
#include <xos/statx.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

// ===================== Working directory =====================
// chdir / getcwd are provided by musl src/unistd/*.c (musl_unistd_objs). The
// kernel's sys_chdir/sys_getcwd are the single source of truth for bp->cwd; no
// libc-side cwd copy is maintained. Relative paths in the plain syscalls
// (sys_open/sys_unlink/sys_mkdir/sys_rename/...) resolve against bp->cwd via
// vfs_resolve_user, and the *at syscalls resolve AT_FDCWD via
// resolve_dirfd_start (kernel/bsd/vfs.c).

// open/openat/fcntl/creat/posix_fadvise/posix_fallocate are provided by musl
// src/fcntl/*.c (musl_fcntl_objs). musl's open/openat route to SYS_open/
// SYS_openat; the kernel resolves relatives against bp->cwd. No libc-side
// wrapper is needed.

// read/write/close/pipe/pipe2 are provided by musl src/unistd
// (musl_unistd_objs, merged into libc.a/libc.so). This file's fd helpers below
// call close/lseek, which the linker resolves to the musl definitions in the
// same archive.

// fcntl is provided by musl src/fcntl/fcntl.c (musl_fcntl_objs, fcntl_worklist
// §3d). musl's fcntl handles F_SETFL (|O_LARGEFILE), F_SETLKW (cancellable),
// F_GETOWN (via F_GETOWN_EX + raw fallback), F_DUPFD_CLOEXEC (with fallback),
// and routes F_SETLK/F_GETLK/F_*OWN_EX/F_OFD_* through syscall(SYS_fcntl).
// The kernel's sys_fcntl implements every cmd the repo's old wrapper did
// (fcntl_worklist §二 aligned all constants). fcntl.c declares _GNU_SOURCE
// itself, so F_GETOWN_EX / struct f_owner_ex are visible at its build.

// ===================== FD_DEV helpers =====================

// notify_fd — notify device driver via fd (uses sys_fdev_pid to find target)
int ipc_notify_fd(int fd) {
  int64_t target_pid = sys_fdev_pid(fd);
  if (target_pid < 0)
    return -1;
  if (target_pid == 0) {
    errno = ENODEV;
    return -1;
  }
  return sys_notify(target_pid);
}

// msg_fd — send variable-length message to device driver via fd
int ipc_msg_fd(int fd, const void *msg_buf, size_t msg_len, void *reply_buf,
               size_t reply_len) {
  int64_t target_pid = sys_fdev_pid(fd);
  if (target_pid < 0)
    return -1;
  if (target_pid == 0) {
    errno = ENODEV;
    return -1;
  }
  return sys_msg(target_pid, (void *)msg_buf, msg_len, reply_buf, reply_len);
}

// poll — wait for events (kernel-implemented via SYS_POLL)
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
  int64_t ret = __syscall3(SYS_POLL, (int64_t)(uintptr_t)fds, (int64_t)nfds,
                           (int64_t)timeout_ms);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

// ppoll — poll with a timespec timeout and an atomic signal-mask swap.
// Thin wrapper over the kernel's SYS_PPOLL (sys_ppoll in socket.c), which
// already does the timespec→ms conversion and the temporary sigmask
// replacement. libwayland-client dlopen's this symbol, so it must be
// exported from libc.so even though poll(2) above stays on SYS_POLL.
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout_ts,
          const sigset_t *sigmask) {
  return sys_ppoll(fds, nfds, timeout_ts, sigmask, sizeof(sigset_t));
}

// dup/dup2 are provided by musl src/unistd (musl_unistd_objs).

// getcwd is provided by musl src/unistd/getcwd.c (musl_unistd_objs): musl calls
// SYS_getcwd, which returns bp->cwd (the kernel's single source of truth).

// lseek is provided by musl src/unistd (musl_unistd_objs); seekdir/rewinddir
// below call it, resolved from the same archive.

// ===================== statx（内核唯一元数据 syscall）=====================
/* statx→stat 缩窄转换：内核只暴露 statx，struct stat 接口全部经此转换。 */
static void statx_to_stat(const struct statx *sx, struct stat *st) {
  __builtin_memset(st, 0, sizeof(*st));
  st->st_dev = makedev(sx->stx_dev_major, sx->stx_dev_minor);
  st->st_ino = sx->stx_ino;
  st->st_nlink = sx->stx_nlink;
  st->st_mode = sx->stx_mode;
  st->st_uid = sx->stx_uid;
  st->st_gid = sx->stx_gid;
  st->st_rdev = makedev(sx->stx_rdev_major, sx->stx_rdev_minor);
  st->st_size = (off_t)sx->stx_size;
  st->st_blksize = (blksize_t)sx->stx_blksize;
  st->st_blocks = (blkcnt_t)sx->stx_blocks;
  st->st_atim.tv_sec = sx->stx_atime.tv_sec;
  st->st_atim.tv_nsec = sx->stx_atime.tv_nsec;
  st->st_mtim.tv_sec = sx->stx_mtime.tv_sec;
  st->st_mtim.tv_nsec = sx->stx_mtime.tv_nsec;
  st->st_ctim.tv_sec = sx->stx_ctime.tv_sec;
  st->st_ctim.tv_nsec = sx->stx_ctime.tv_nsec;
}

int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *stx) {
  if (!stx) {
    errno = EFAULT;
    return -1;
  }
  return sys_statx(dirfd, path, flags, mask, stx);
}

/* 路径类 stat 公共体：直接透传 statx，相对路径由内核 resolve_dirfd_start
 * (AT_FDCWD→bp->cwd) 解析。flags = 0（stat）或 AT_SYMLINK_NOFOLLOW（lstat，
 * 本 OS 无 symlink，语义相同）。 */
static int do_stat_path(const char *path, int flags, struct stat *st) {
  if (!path || !st) {
    errno = EFAULT;
    return -1;
  }
  struct statx sx;
  if (sys_statx(AT_FDCWD, path, flags, STATX_BASIC_STATS, &sx) != 0)
    return -1;
  statx_to_stat(&sx, st);
  return 0;
}

int stat(const char *path, struct stat *st) {
  return do_stat_path(path, 0, st);
}

int lstat(const char *path, struct stat *st) {
  return do_stat_path(path, AT_SYMLINK_NOFOLLOW, st);
}

// ===================== access / faccessat / utimensat =====================
// access is provided by musl src/unistd (musl_unistd_objs); on x86-64 musl
// routes it to syscall(SYS_access), which this kernel resolves via
// vfs_resolve_user (cwd-relative). faccessat below is retained (musl's
// AT_EACCESS clone path is not adopted this batch — see user/CMakeLists.txt
// MUSL_UNISTD_EXCLUDE). faccessat(dirfd,path,mode,flags):access 的 dirfd
// 相对变体。AT_FDCWD ≡ 当前根(内核无 per-process CWD)。flags 透传
// AT_EACCESS/AT_SYMLINK_NOFOLLOW/ AT_EMPTY_PATH(内核校验)。
LIBC_EXPORT int faccessat(int dirfd, const char *path, int mode, int flags) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  return sys_faccessat(dirfd, path, mode, flags);
}

// utimensat(dirfd,path,times,flags):设 path 的 atime/mtime。times 各项
// tv_nsec=UTIME_NOW(=now)/UTIME_OMIT(不变);times=NULL → atime=mtime=now
// (需写权限)。flags 仅 AT_SYMLINK_NOFOLLOW(本 OS 无 symlink,接受同语义)。
int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
  if (path && !path[0]) {
    errno = EINVAL;
    return -1;
  }
  return sys_utimensat(dirfd, path, times, flags);
}

// ===================== fchmod / fchmodat / fchown / fchownat
// ===================== 落盘仅内存(与 utimensat 一致);setuid 位清除见
// kernel/bsd/syscall.c。 errno 转换在 syscall.h 薄封装;此处仅 NULL-path
// 防护(fchmod/fchown 走 fd, 无 path)。
int fchmod(int fd, mode_t mode) { return sys_fchmod(fd, (unsigned int)mode); }

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  return sys_fchmodat(dirfd, path, (unsigned int)mode, flags);
}

// fchown/fchownat are provided by musl src/unistd (musl_unistd_objs). musl's
// fchown has a /proc/self/fd EBADF fallback (via __procfdname, src/internal/
// procfdname.c — also compiled into musl_unistd_objs); this kernel's sys_fchown
// is real, so the fallback branch is dead but the symbol resolves. fchownat is
// a pure syscall(SYS_fchownat) passthrough.

// symlink/symlinkat/readlink/readlinkat/link/linkat/unlink/unlinkat/renameat
// are provided by musl src/unistd (musl_unistd_objs). On x86-64 musl routes the
// plain forms to syscall(SYS_symlink/SYS_readlink/SYS_link/SYS_unlink) and the
// *at forms to syscall(SYS_*at) with AT_FDCWD passed straight through; the
// kernel resolves AT_FDCWD against bp->cwd (resolve_dirfd_start) and plain
// relatives via vfs_resolve_user. unlinkat's AT_REMOVEDIR flag is honored by
// the kernel's sys_unlinkat.

// rename is provided by musl src/stdio/rename.c (musl_unistd_objs): musl
// routes it to SYS_rename, resolved cwd-relative via vfs_resolve_user.

// rmdir is provided by musl src/unistd (musl_unistd_objs); on x86-64 musl
// routes it to syscall(SYS_rmdir), resolved cwd-relative via vfs_resolve_user.

// ===================== isatty =====================
// Retained (musl isatty.c excluded): musl probes TIOCGWINSZ, but this kernel's
// serial tty only answers TCGETS — so musl's isatty would mis-report the serial
// console as not-a-tty. TCGETS works for both PTY and serial.
LIBC_EXPORT int isatty(int fd) {
  // Use ioctl TCGETS to detect tty devices
  long rc = sys_ioctl(fd, TCGETS, 0);
  return (rc == 0) ? 1 : 0;
}

// ===================== tcgetattr / tcsetattr =====================
int tcgetattr(int fd, struct termios *termios_p) {
  long rc = sys_ioctl(fd, TCGETS, (uint64_t)termios_p);
  return (int)rc;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
  uint32_t cmd;
  switch (optional_actions) {
  case TCSANOW:
    cmd = TCSETS;
    break;
  case TCSADRAIN:
    cmd = TCSETSW;
    break;
  case TCSAFLUSH:
    cmd = TCSETSF;
    break;
  default:
    errno = EINVAL;
    return -1;
  }
  long rc = sys_ioctl(fd, cmd, (uint64_t)termios_p);
  return (int)rc;
}

// ===================== ttyname =====================
LIBC_EXPORT char *ttyname(int fd) {
  if (!isatty(fd))
    return NULL;
  static char name[32];
  int index = -1;
  long rc = sys_ioctl(fd, TIOCGPTN, (uint64_t)&index);
  if (rc < 0 || index < 0)
    return NULL;
  // Build "/dev/ptsN"
  const char *prefix = "/dev/pts";
  int pos = 0;
  for (int i = 0; prefix[i]; i++)
    name[pos++] = prefix[i];
  if (index == 0) {
    name[pos++] = '0';
  } else {
    char tmp[8];
    int tpos = 0;
    int n = index;
    while (n > 0) {
      tmp[tpos++] = '0' + (n % 10);
      n /= 10;
    }
    for (int i = tpos - 1; i >= 0; i--)
      name[pos++] = tmp[i];
  }
  name[pos] = '\0';
  return name;
}

// ===================== ioctl =====================
//
// Buffer strategy — hybrid stack/heap:
//   ≤64B  → stack (zero alloc overhead)
//   >64B  → heap via static reusable buffer (realloc grows-on-demand)
//   >4KB  → rejected (kernel-side limit safety)
//
// The reusable heap buffer lives until process exit, avoiding repeated
// malloc/free churn for large ioctls (EVIOCGNAME(256), DRM structs, etc.).
// Not thread-safe (same convention as strtok/ttyname in this libc).
int ioctl(int fd, uint32_t cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  uint64_t arg = va_arg(ap, uint64_t);
  va_end(ap);

  uint16_t arg_size = _IOC_SIZE(cmd);
  // Legacy ioctl commands (TCGETS, TCSETS, TIOCGPGRP, etc.) don't encode
  // direction/size in the _IOC format — _IOC_SIZE=0, _IOC_DIR=_IOC_NONE.
  // The buf intermediary relies on these fields to copy data, so it silently
  // drops data transfer for legacy commands. Pass the user pointer directly
  // so the kernel's copy_to_user/copy_from_user handle it correctly.
  if (arg_size == 0) {
    long rc = sys_ioctl(fd, cmd, arg);
    return (int)rc;
  }

  // Cap at a reasonable max — no individual ioctl struct should need more
  if (arg_size > 4096) {
    errno = EINVAL;
    return -1;
  }

  // Choose buffer: stack for small, heap (reusable) for large
  uint8_t stack_buf[64];
  void *buf;

  if (arg_size <= sizeof(stack_buf)) {
    buf = stack_buf;
    __builtin_memset(buf, 0, arg_size);
  } else {
    // Reusable heap buffer — allocate once, grow on demand, never freed
    // (process teardown reclaims it).
    static void *heap_buf = NULL;
    static size_t heap_cap = 0;

    if (heap_cap < arg_size) {
      void *nb = realloc(heap_buf, arg_size);
      if (!nb) {
        errno = ENOMEM;
        return -1;
      }
      heap_buf = nb;
      heap_cap = arg_size;
    }
    buf = heap_buf;
    // Only zero the portion we'll use (reused buffer may have stale data).
    __builtin_memset(buf, 0, arg_size);
  }

  // Copy-in: user arg → buf (only if direction includes WRITE)
  if ((_IOC_DIR(cmd) & _IOC_WRITE) && arg != 0 && arg_size > 0)
    __builtin_memcpy(buf, (const void *)arg, arg_size);

  long rc = sys_ioctl(fd, cmd, (uint64_t)(uintptr_t)buf);
  if (rc < 0)
    return (int)rc;

  // Copy-out: buf → user arg (only if direction includes READ)
  if ((_IOC_DIR(cmd) & _IOC_READ) && arg != 0 && arg_size > 0)
    __builtin_memcpy((void *)arg, buf, arg_size);

  return (int)rc;
}

// ===================== fstat（经 statx AT_EMPTY_PATH）=====================
int fstat(int fd, struct stat *st) {
  if (!st) {
    errno = EFAULT;
    return -1;
  }
  struct statx sx;
  if (sys_statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx) != 0)
    return -1;
  statx_to_stat(&sx, st);
  return 0;
}

// S07: fstatat(dirfd, path, st, flags) — 直接透传 statx。AT_EMPTY_PATH +
// 空路径 → 内核 stat dirfd 本身；相对路径由内核 resolve_dirfd_start
// (AT_FDCWD→bp->cwd) 解析。
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
  if (!st) {
    errno = EFAULT;
    return -1;
  }
  struct statx sx;
  if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
    if (sys_statx(dirfd, "", flags, STATX_BASIC_STATS, &sx) != 0)
      return -1;
  } else if (sys_statx(dirfd, path, flags, STATX_BASIC_STATS, &sx) != 0) {
    return -1;
  }
  statx_to_stat(&sx, st);
  return 0;
}

// ===================== mkdir (via sys_mkdir syscall) =====================
int mkdir(const char *path, mode_t mode) {
  (void)mode; // FAT32 doesn't support permissions
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  return sys_mkdir(path, 0);
}

// S07: mkdirat(dirfd, path, mode) — thin pass-through; the kernel's
// sys_mkdirat resolves AT_FDCWD→bp->cwd via resolve_dirfd_start. (musl's
// mkdirat lives in src/stat/, not pulled, so the repo keeps this wrapper.)
int mkdirat(int dirfd, const char *path, mode_t mode) {
  (void)mode; // FAT32 doesn't support permissions
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  return sys_mkdirat(dirfd, path, mode);
}
