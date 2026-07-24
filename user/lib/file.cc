/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc file I/O: all file operations go through syscalls directly.
// Kernel handles FAT32, devtmpfs, pipes, sockets, etc.
// No libc-side fd_table — kernel's proc->fd_table is the single source of
// truth.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>  // snprintf (rename cwd 相对化)
#include <stdlib.h> // IWYU pragma: keep  // malloc/calloc/free in getdir/dup
#include <string.h> // strlen (rename cwd 相对化)
#include <syscall.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/ioctl.h>
#include <xos/socket.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

// ===================== Working directory (per-process) =====================
static char cwd_path[256] = "/";

const char *__get_cwd(void) { return cwd_path; }

uint64_t fd_file_size(int fd) {
  // Use sys_lseek(SEEK_END) to get file size from kernel
  int64_t size = sys_lseek(fd, 0, SEEK_END);
  if (size < 0)
    return 0;
  // Restore position to beginning
  sys_lseek(fd, 0, SEEK_SET);
  return (uint64_t)size;
}

// ===================== open =====================

int open(const char *path, int flags, ...) {
  // S08: open(O_CREAT, mode) — POSIX: the mode arg is the third (variadic)
  // parameter, present only when O_CREAT is set, and is masked by umask in the
  // kernel. Read it here and forward to sys_open; dropping it left rdx (the
  // kernel's arg3) holding garbage, so created files got garbage permission
  // bits (different per call path).
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  // Handle relative paths by prepending cwd
  char abs_path[256];
  if (path && path[0] != '/') {
    abs_path[0] = '\0';
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/') {
      abs_path[cwdi++] = '/';
    }
    int pi = 0;
    while (path[pi] && cwdi < 254) {
      abs_path[cwdi++] = path[pi++];
    }
    abs_path[cwdi] = '\0';
    path = abs_path;
  }

  // All paths go through sys_open — kernel dispatches FAT32 and devtmpfs
  // internally
  int fd = sys_open(path, flags, (int)mode);
  return fd;
}

// S07: resolve a *at(dirfd, path, ...) path for the user-side CWD model.
// AT_FDCWD → prepend the userspace cwd_path (kernel has no CWD), returning a
// buffer the caller must keep alive until the syscall. A real dirfd → return
// `path` unchanged; the kernel resolves it relative to dirfd. Absolute paths
// are returned unchanged. `buf` must be >=256 bytes.
static const char *resolve_at_path(int dirfd, const char *path, char *buf) {
  if (!path || path[0] == '/' || dirfd != AT_FDCWD)
    return path;
  int cwdi = 0;
  while (cwd_path[cwdi] && cwdi < 254) {
    buf[cwdi] = cwd_path[cwdi];
    cwdi++;
  }
  if (cwdi > 0 && buf[cwdi - 1] != '/')
    buf[cwdi++] = '/';
  int pi = 0;
  while (path[pi] && cwdi < 254)
    buf[cwdi++] = path[pi++];
  buf[cwdi] = '\0';
  return buf;
}

// S07: openat(dirfd, path, flags, mode). AT_FDCWD → cwd-relative (absolute path
// to the kernel); a real dirfd → kernel resolves the relative path from it.
int openat(int dirfd, const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  char buf[256];
  const char *p = resolve_at_path(dirfd, path, buf);
  /* For AT_FDCWD the path is now absolute → sys_open (mode applied). For a real
   * dirfd pass the original relative path + dirfd to sys_openat. */
  if (dirfd == AT_FDCWD)
    return sys_open(p, flags, (int)mode);
  return sys_openat(dirfd, p, flags, (int)mode);
}

// ===================== read =====================

ssize_t read(int fd, void *buf, size_t count) {
  return (ssize_t)sys_read(fd, buf, count);
}

// ===================== write =====================

ssize_t write(int fd, const void *buf, size_t count) {
  return (ssize_t)sys_write(fd, buf, count);
}

// ===================== close =====================

int close(int fd) { return sys_close(fd); }

// ===================== pipe =====================

int pipe(int fd[2]) { return sys_pipe(fd); }

// ===================== chdir =====================

int chdir(const char *path) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }

  // Resolve path: if it doesn't start with '/', prepend cwd
  char abs_path[256];
  if (path[0] != '/') {
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/') {
      abs_path[cwdi++] = '/';
    }
    int pi = 0;
    while (path[pi] && cwdi < 254) {
      abs_path[cwdi++] = path[pi++];
    }
    abs_path[cwdi] = '\0';
    path = abs_path;
  }

  // Normalize: remove trailing slash (except for "/")
  char norm[256];
  int ni = 0;
  while (path[ni] && ni < 255) {
    norm[ni] = path[ni];
    ni++;
  }
  norm[ni] = '\0';
  while (ni > 1 && norm[ni - 1] == '/') {
    norm[ni - 1] = '\0';
    ni--;
  }

  // Validate path exists via stat
  struct stat st;
  if (stat(norm, &st) != 0)
    return -1;

  // Update cwd
  int i = 0;
  while (norm[i] && i < 255) {
    cwd_path[i] = norm[i];
    i++;
  }
  cwd_path[i] = '\0';
  return 0;
}

// ===================== fcntl =====================

int fcntl(int fd, int cmd, ...) {
  int64_t r;
  va_list ap;

  if (cmd == F_GETFL) {
    r = sys_fcntl(fd, F_GETFL, 0);
  } else if (cmd == F_SETFL) {
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    r = sys_fcntl(fd, cmd, arg);
  } else if (cmd == F_GETFD) {
    r = sys_fcntl(fd, F_GETFD, 0);
  } else if (cmd == F_SETFD) {
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    r = sys_fcntl(fd, cmd, arg);
  } else if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
    va_start(ap, cmd);
    int min_fd = va_arg(ap, int);
    va_end(ap);
    r = sys_fcntl(fd, cmd, min_fd);
  } else if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW ||
             cmd == F_OFD_GETLK || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW) {
    va_start(ap, cmd);
    struct flock *lk = va_arg(ap, struct flock *);
    va_end(ap);
    r = sys_fcntl(fd, cmd, (int64_t)(uintptr_t)lk);
  } else if (cmd == F_SETOWN || cmd == F_SETSIG) {
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    r = sys_fcntl(fd, cmd, arg);
  } else if (cmd == F_GETOWN) {
    /* F_GETOWN legitimately returns a negative pgid, so it must bypass
     * sys_fcntl's "r < 0 → errno" mapping (matches glibc: raw syscall). */
    r = __syscall3(SYS_FCNTL, (int64_t)fd, (int64_t)cmd, 0);
  } else if (cmd == F_GETSIG) {
    r = sys_fcntl(fd, cmd, 0);
  } else if (cmd == F_SETOWN_EX || cmd == F_GETOWN_EX) {
    va_start(ap, cmd);
    struct f_owner_ex *ox = va_arg(ap, struct f_owner_ex *);
    va_end(ap);
    r = sys_fcntl(fd, cmd, (int64_t)(uintptr_t)ox);
  } else if (cmd == F_GETPIPE_SZ) {
    r = sys_fcntl(fd, cmd, 0);
  } else if (cmd == F_SETPIPE_SZ) {
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    r = sys_fcntl(fd, cmd, arg);
  } else {
    errno = EINVAL;
    return -1;
  }

  // sys_fcntl already maps kernel -errno → errno and returns -1 on failure, so
  // no errno remapping here (re-deriving errno from the -1 return would yield
  // EPERM and clobber the real error). cmds taking a pointer (F_*LK/F_OFD_*)
  // pass it through arg3 unchanged.
  return (int)r;
}

// ===================== FD_DEV helpers =====================

// notify_fd — notify device driver via fd (uses sys_fdev_pid to find target)
int notify_fd(int fd) {
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
int msg_fd(int fd, const void *msg_buf, size_t msg_len, void *reply_buf,
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

// dup2 — duplicate fd
int dup2(int old_fd, int new_fd) { return sys_dup2(old_fd, new_fd); }

// dup — duplicate fd (lowest available slot)
int dup(int old_fd) { return sys_dup(old_fd); }

// ===================== getcwd =====================
char *getcwd(char *buf, size_t size) {
  if (!buf) {
    errno = EFAULT;
    return NULL;
  }
  const char *cwd = __get_cwd();
  int len = 0;
  while (cwd[len])
    len++;
  if ((size_t)len >= size) {
    errno = ERANGE;
    return NULL;
  }
  int i;
  for (i = 0; cwd[i]; i++)
    buf[i] = cwd[i];
  buf[i] = '\0';
  return buf;
}

// ===================== lseek =====================
off_t lseek(int fd, off_t offset, int whence) {
  return (off_t)sys_lseek(fd, offset, whence);
}

// ===================== stat (via sys_stat syscall) =====================
int stat(const char *path, struct stat *st) {
  if (!path || !st) {
    errno = EFAULT;
    return -1;
  }

  // Resolve path to absolute
  char abs_path[256];
  if (path[0] != '/') {
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/')
      abs_path[cwdi++] = '/';
    int pi = 0;
    while (path[pi] && cwdi < 254)
      abs_path[cwdi++] = path[pi++];
    abs_path[cwdi] = '\0';
    path = abs_path;
  }

  int r = sys_stat(path, st);
  return r;
}

// ===================== access =====================
int access(const char *path, int mode) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }

  // F_OK: just check if file exists via stat
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  (void)mode; // ignore R_OK/W_OK/X_OK — no permissions in FAT32
  return 0;
}

// ===================== unlink (via sys_unlink syscall) =====================
int unlink(const char *path) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }

  char abs_path[256];
  if (path[0] != '/') {
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/')
      abs_path[cwdi++] = '/';
    int pi = 0;
    while (path[pi] && cwdi < 254)
      abs_path[cwdi++] = path[pi++];
    abs_path[cwdi] = '\0';
    path = abs_path;
  }

  int r = sys_unlink(path);
  return r;
}

// S07: unlinkat(dirfd, path, flags). AT_REMOVEDIR → rmdir semantics.
// AT_FDCWD → cwd-relative; real dirfd → kernel resolves relative to it.
int unlinkat(int dirfd, const char *path, int flags) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  char buf[256];
  const char *p = resolve_at_path(dirfd, path, buf);
  if (dirfd == AT_FDCWD) {
    if (flags & AT_REMOVEDIR)
      return sys_rmdir(p);
    return sys_unlink(p);
  }
  return sys_unlinkat(dirfd, p, flags);
}

// ===================== rename (via sys_rename syscall) =====================
int rename(const char *oldpath, const char *newpath) {
  if (!oldpath || !newpath) {
    errno = EFAULT;
    return -1;
  }
  const char *old_abs = oldpath;
  const char *new_abs = newpath;
  char old_abs_path[256], new_abs_path[256];
  if (oldpath[0] != '/') {
    /* cwd 相对化(照 file.cc 既有 unlink/mkdir wrapper 惯例) */
    getcwd(old_abs_path, sizeof(old_abs_path));
    size_t cl = strlen(old_abs_path);
    if (cl + 1 + strlen(oldpath) + 1 > sizeof(old_abs_path)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    snprintf(old_abs_path + cl, sizeof(old_abs_path) - cl, "/%s", oldpath);
    old_abs = old_abs_path;
  }
  if (newpath[0] != '/') {
    getcwd(new_abs_path, sizeof(new_abs_path));
    size_t cl = strlen(new_abs_path);
    if (cl + 1 + strlen(newpath) + 1 > sizeof(new_abs_path)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    snprintf(new_abs_path + cl, sizeof(new_abs_path) - cl, "/%s", newpath);
    new_abs = new_abs_path;
  }
  if (sys_rename(old_abs, new_abs) < 0)
    return -1;
  return 0;
}

// S07: renameat(olddirfd, oldpath, newdirfd, newpath). Each side independently:
// AT_FDCWD → cwd-relative (absolute to kernel); real dirfd → kernel resolves.
int renameat(int olddirfd, const char *oldpath, int newdirfd,
             const char *newpath) {
  if (!oldpath || !newpath) {
    errno = EFAULT;
    return -1;
  }
  char old_buf[256], new_buf[256];
  const char *op = resolve_at_path(olddirfd, oldpath, old_buf);
  const char *np = resolve_at_path(newdirfd, newpath, new_buf);
  if (olddirfd == AT_FDCWD && newdirfd == AT_FDCWD)
    return sys_rename(op, np);
  return sys_renameat(olddirfd, op, newdirfd, np);
}
int rmdir(const char *path) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }

  char abs_path[256];
  if (path[0] != '/') {
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/')
      abs_path[cwdi++] = '/';
    int pi = 0;
    while (path[pi] && cwdi < 254)
      abs_path[cwdi++] = path[pi++];
    abs_path[cwdi] = '\0';
    path = abs_path;
  }

  int r = sys_rmdir(path);
  return r;
}

// ===================== isatty =====================
int isatty(int fd) {
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
char *ttyname(int fd) {
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

// ===================== fstat =====================
int fstat(int fd, struct stat *st) {
  if (!st) {
    errno = EFAULT;
    return -1;
  }
  long rc = sys_fstat(fd, (uint64_t)st);
  return (int)rc;
}

// S07: fstatat(dirfd, path, st, flags). AT_EMPTY_PATH + empty path →
// fstat(dirfd). AT_FDCWD → cwd-relative; a real dirfd → kernel resolves
// relative to it.
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
  if (!st) {
    errno = EFAULT;
    return -1;
  }
  if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0')
    return fstat(dirfd, st);
  char buf[256];
  const char *p = resolve_at_path(dirfd, path, buf);
  if (dirfd == AT_FDCWD)
    return sys_stat(p, st);
  return sys_newfstatat(dirfd, p, st, flags);
}

// ===================== mkdir (via sys_mkdir syscall) =====================
int mkdir(const char *path, mode_t mode) {
  (void)mode; // FAT32 doesn't support permissions
  if (!path) {
    errno = EFAULT;
    return -1;
  }

  char abs_path[256];
  if (path[0] != '/') {
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/')
      abs_path[cwdi++] = '/';
    int pi = 0;
    while (path[pi] && cwdi < 254)
      abs_path[cwdi++] = path[pi++];
    abs_path[cwdi] = '\0';
    path = abs_path;
  }

  int r = sys_mkdir(path, 0);
  return r;
}

// S07: mkdirat(dirfd, path, mode). AT_FDCWD → cwd-relative; real dirfd →
// kernel.
int mkdirat(int dirfd, const char *path, mode_t mode) {
  (void)mode;
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  char buf[256];
  const char *p = resolve_at_path(dirfd, path, buf);
  if (dirfd == AT_FDCWD)
    return sys_mkdir(p, 0);
  return sys_mkdirat(dirfd, p, mode);
}

// ===================== opendir / readdir / closedir =====================
#include <dirent.h>
#include <xos/dirent.h>

#define GETDENTS_BUF_SIZE 4096

struct __dir {
  int dd_fd;
  uint8_t *buf;
  size_t buf_pos;
  size_t buf_len;
  struct dirent result; /* S05: per-DIR result buffer (readdir is reentrant
                         * across distinct DIR streams; the old static buffer
                         * made concurrent readdir() on two DIRs clobber each
                         * other). */
};

DIR *opendir(const char *name) {
  if (!name) {
    errno = EFAULT;
    return NULL;
  }

  // Resolve path
  char abs_path[256];
  if (name[0] != '/') {
    int cwdi = 0;
    while (cwd_path[cwdi] && cwdi < 254) {
      abs_path[cwdi] = cwd_path[cwdi];
      cwdi++;
    }
    if (cwdi > 0 && abs_path[cwdi - 1] != '/')
      abs_path[cwdi++] = '/';
    int pi = 0;
    while (name[pi] && cwdi < 254)
      abs_path[cwdi++] = name[pi++];
    abs_path[cwdi] = '\0';
    name = abs_path;
  }

  int fd = open(name, O_RDONLY);
  if (fd < 0)
    return NULL;

  struct __dir *dir = (struct __dir *)calloc(1, sizeof(struct __dir));
  if (!dir) {
    close(fd);
    errno = ENOMEM;
    return NULL;
  }

  dir->buf = (uint8_t *)malloc(GETDENTS_BUF_SIZE);
  if (!dir->buf) {
    close(fd);
    free(dir);
    errno = ENOMEM;
    return NULL;
  }

  dir->dd_fd = fd;
  dir->buf_pos = 0;
  dir->buf_len = 0;
  return (DIR *)dir;
}

struct dirent *readdir(DIR *dirp) {
  struct __dir *dir = (struct __dir *)dirp;
  if (!dir) {
    errno = EBADF;
    return NULL;
  }

  // If buffer is exhausted, refill via sys_getdents
  if (dir->buf_pos >= dir->buf_len) {
    int n = sys_getdents(dir->dd_fd, dir->buf, GETDENTS_BUF_SIZE);
    if (n <= 0)
      return NULL; // EOF or error
    dir->buf_pos = 0;
    dir->buf_len = (size_t)n;
  }
  // Parse current dirent64 entry
  struct dirent64 *d64 = (struct dirent64 *)(dir->buf + dir->buf_pos);
  struct dirent *result = &dir->result;
  result->d_ino = (ino_t)d64->d_ino;
  result->d_off = (off_t)d64->d_off;
  result->d_reclen = d64->d_reclen;
  int j = 0;
  while (d64->d_name[j] && j < 255) {
    result->d_name[j] = d64->d_name[j];
    j++;
  }
  result->d_name[j] = '\0';

  dir->buf_pos += d64->d_reclen;
  return result;
}

int closedir(DIR *dirp) {
  struct __dir *dir = (struct __dir *)dirp;
  if (!dir) {
    errno = EBADF;
    return -1;
  }

  close(dir->dd_fd);
  free(dir->buf);
  free(dir);
  return 0;
}

// ===================== seekdir / telldir / rewinddir / dirfd (S05)
// ===================== The kernel getdents64 cursor lives in the fd's
// f->offset; lseek on a dir fd repositions it and the next getdents resumes
// from there (verified: sys_lseek accepts arbitrary non-negative offsets on
// FD_DIR). d_off is the per-entry seek cookie the kernel emits, so telldir
// returns the next entry's d_off and seekdir restores the kernel cursor to it,
// then invalidates the user buffer.
long telldir(DIR *dirp) {
  struct __dir *dir = (struct __dir *)dirp;
  if (!dir) {
    errno = EBADF;
    return -1;
  }
  /* telldir must return the position of the entry just read by readdir() so
   * that seekdir(loc) + readdir() yields that same entry again. After readdir
   * returns entry X it advances buf_pos to point at entry X+1; the d_off of
   * the NEXT entry (the old buf_pos-based read) is X+1's cookie, not X's.
   * The cookie of the entry we actually handed back is preserved in
   * dir->result.d_off, so return that. */
  return (long)dir->result.d_off;
}

void seekdir(DIR *dirp, long loc) {
  struct __dir *dir = (struct __dir *)dirp;
  if (!dir)
    return;
  // Reposition the kernel getdents cursor to the cookie, then drop the
  // prefetched user buffer so the next readdir refills from the new position.
  lseek(dir->dd_fd, (off_t)loc, SEEK_SET);
  dir->buf_pos = 0;
  dir->buf_len = 0;
}

void rewinddir(DIR *dirp) {
  struct __dir *dir = (struct __dir *)dirp;
  if (!dir)
    return;
  lseek(dir->dd_fd, 0, SEEK_SET);
  dir->buf_pos = 0;
  dir->buf_len = 0;
}

int dirfd(DIR *dirp) {
  struct __dir *dir = (struct __dir *)dirp;
  if (!dir) {
    errno = EBADF;
    return -1;
  }
  return dir->dd_fd;
}

// readdir_r: explicit-result reentrant variant. Deprecated by glibc but still
// provided for compatibility. Copies the next entry into the caller's buffer
// and returns 0 with *result = entry (or NULL at EOF).
int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
  if (!dirp || !entry || !result) {
    if (result)
      *result = NULL;
    return EBADF;
  }
  // Save/restore errno around readdir so a benign EOF (which sets no errno
  // here) does not leak a stale value to the caller.
  int saved = errno;
  struct dirent *e = readdir(dirp);
  if (!e) {
    *result = NULL;
    errno = saved;
    return 0;
  }
  *entry = *e;
  *result = entry;
  return 0;
}

// ===================== scandir =====================
int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  DIR *dir = opendir(dirp);
  if (!dir)
    return -1;

  size_t capacity = 32;
  size_t count = 0;
  struct dirent **list =
      (struct dirent **)malloc(capacity * sizeof(struct dirent *));
  if (!list) {
    closedir(dir);
    errno = ENOMEM;
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (filter && !filter(entry))
      continue;

    struct dirent *copy = (struct dirent *)malloc(sizeof(struct dirent));
    if (!copy) {
      for (size_t i = 0; i < count; i++)
        free(list[i]);
      free(list);
      closedir(dir);
      errno = ENOMEM;
      return -1;
    }
    *copy = *entry;

    if (count >= capacity) {
      capacity *= 2;
      struct dirent **newlist =
          (struct dirent **)realloc(list, capacity * sizeof(struct dirent *));
      if (!newlist) {
        free(copy);
        for (size_t i = 0; i < count; i++)
          free(list[i]);
        free(list);
        closedir(dir);
        errno = ENOMEM;
        return -1;
      }
      list = newlist;
    }
    list[count++] = copy;
  }
  closedir(dir);

  if (compar)
    qsort(list, count, sizeof(struct dirent *),
          (int (*)(const void *, const void *))compar);

  *namelist = list;
  return (int)count;
}
