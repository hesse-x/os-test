/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <stdint.h>
#include <stdlib.h> // IWYU pragma: keep
#include <termios.h>
#include <unistd.h>
#include <xos/syscall_ext.h>

#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/ipc.h>

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
// (fcntl_worklist section 2 aligned all constants). fcntl.c declares
// _GNU_SOURCE itself, so F_GETOWN_EX / struct f_owner_ex are visible at its
// build.

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

// poll/ppoll are provided by musl src/select/{poll,ppoll}.c.

// dup/dup2 are provided by musl src/unistd (musl_unistd_objs).

// getcwd is provided by musl src/unistd/getcwd.c (musl_unistd_objs): musl calls
// SYS_getcwd, which returns bp->cwd (the kernel's single source of truth).

// lseek is provided by musl src/unistd (musl_unistd_objs); seekdir/rewinddir
// below call it, resolved from the same archive.

// stat/lstat/fstat/fstatat and mkdir/mkdirat are provided by musl src/stat
// (musl_stat_objs). Their legacy kstat syscalls share the kernel's statx core.

// ===================== access / faccessat / utimensat =====================
// access is provided by musl src/unistd (musl_unistd_objs); on x86-64 musl
// routes it to syscall(SYS_access), which this kernel resolves via
// vfs_resolve_user (cwd-relative). faccessat is ADOPTED from musl
// src/unistd/faccessat.c (musl_unistd_objs): its AT_EACCESS clone path's deps
// (__block_all_sigs/__restore_sigs from musl_pthread block.c, __clone from
// musl_pthread clone.c, __sys_wait4 = __syscall macro) are all satisfied, and
// the kernel's SYS_WAIT4/SYS_setreuid/SYS_setregid/SYS_faccessat are
// implemented. The repo's old sys_faccessat wrapper here is deleted.

// utimensat(dirfd,path,times,flags): set atime/mtime of path. Each times
// entry's tv_nsec=UTIME_NOW (=now) / UTIME_OMIT (unchanged); times=NULL →
// atime=mtime=now (requires write permission). flags is only
// AT_SYMLINK_NOFOLLOW (this OS has no symlinks; same semantics accepted).
int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
  if (path && !path[0]) {
    errno = EINVAL;
    return -1;
  }
  return sys_utimensat(dirfd, path, times, flags);
}

// ===================== fchmod / fchmodat / fchown / fchownat
// ===================== Persistence is in-memory only (same as utimensat);
// setuid-bit clearing is in kernel/bsd/syscall.c. errno translation lives in
// the syscall.h thin wrappers; here only NULL-path guards (fchmod/fchown take
// an fd, no path).
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
  // Use ioctl TCGETS to detect tty devices. TCGETS writes a struct termios to
  // arg — passing NULL (arg=0) makes the kernel's copy_to_user fail with
  // -EFAULT, so isatty would mis-report real ttys. Supply a real buffer.
  struct termios t;
  long rc = sys_ioctl(fd, TCGETS, (uint64_t)&t);
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
// ADOPTED musl upstream (src/unistd/ttyname.c wraps ttyname_r): procfs M4 now
// provides /proc/self/fd/N, so musl's ttyname_r readlinks it + stat/fstat
// dev+ino cross-check (Linux-canonical). The old repo version (ioctl TIOCGPTN)
// is removed — see build_script/third_party/musl/modules/unistd.cmake.

// ===================== ioctl =====================
// ADOPTED musl upstream (src/misc/ioctl.c, musl_misc_objs): a thin
// __syscall(SYS_ioctl, fd, req, arg) shim that forwards the raw user pointer.
// The kernel's sys_ioctl (kernel/bsd/syscall.c) already performs the
// _IOC_SIZE/_IOC_DIR copy_in/copy_out itself (f_op->ioctl path gets the user
// pointer directly; FD_DEV direct path copy_from_user/copy_to_user into kbuf),
// so the repo's old libc-side second copy layer + reusable heap buffer were
// redundant, and its arg_size==0 legacy-pointer special-case was wrong for the
// f_op->ioctl path (which expects the user pointer). musl's shim is correct
// against the kernel's IPC-based dispatch (libc only forwards fd/cmd/arg);
// kernel-internal routing to user-space drivers is unaffected. musl's ioctl.c
// also gains the SIOCGSTAMP/SIOCGSTAMPNS time64 compat translation.
// — see build_script/third_party/musl/modules/misc.cmake.
