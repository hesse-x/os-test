/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>      /* struct timespec — utimensat(2) signature */
#include <xos/fcntl.h> /* F_OK/R_OK/W_OK/X_OK + AT_* (shared kernel+user uapi) */

#ifdef __cplusplus
extern "C" {
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_DATA 3
#define SEEK_HOLE 4

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

LIBC_EXPORT pid_t getpid(void);
LIBC_EXPORT pid_t gettid(void);
LIBC_EXPORT void _exit(int status);
LIBC_EXPORT ssize_t read(int fd, void *buf, size_t count);
LIBC_EXPORT ssize_t write(int fd, const void *buf, size_t count);
LIBC_EXPORT int close(int fd);
LIBC_EXPORT int pipe(int fd[2]);
LIBC_EXPORT int pipe2(int fd[2], int flags);
LIBC_EXPORT int open(const char *path, int flags, ...);
LIBC_EXPORT int dup2(int old_fd, int new_fd);
LIBC_EXPORT int dup(int old_fd);
LIBC_EXPORT int sched_yield(void);
LIBC_EXPORT int ioperm(unsigned long from, unsigned long num, int turn_on);
LIBC_EXPORT char *getcwd(char *buf, size_t size);
LIBC_EXPORT off_t lseek(int fd, off_t offset, int whence);
LIBC_EXPORT int ftruncate(int fd, off_t length);
LIBC_EXPORT int memfd_create(const char *name, unsigned int flags);
LIBC_EXPORT unsigned int sleep(unsigned seconds);
LIBC_EXPORT int usleep(unsigned usec);
LIBC_EXPORT int access(const char *path, int mode);
LIBC_EXPORT int faccessat(int dirfd, const char *path, int mode, int flags);
LIBC_EXPORT int utimensat(int dirfd, const char *path,
                          const struct timespec times[2], int flags);
/* §3.3 symlink/readlink — path-based 链接 + 读软链 target。 */
LIBC_EXPORT int symlink(const char *target, const char *linkpath);
LIBC_EXPORT int symlinkat(const char *target, int newdirfd,
                          const char *linkpath);
LIBC_EXPORT ssize_t readlink(const char *path, char *buf, size_t bufsiz);
LIBC_EXPORT ssize_t readlinkat(int dirfd, const char *path, char *buf,
                               size_t bufsiz);
/* §3.4 link/linkat — path-based 硬链接(nlink 全链路)。 */
LIBC_EXPORT int link(const char *oldpath, const char *newpath);
LIBC_EXPORT int linkat(int olddirfd, const char *oldpath, int newdirfd,
                       const char *newpath, int flags);
LIBC_EXPORT int unlink(const char *path);
LIBC_EXPORT int unlinkat(int dirfd, const char *path, int flags);
LIBC_EXPORT int rmdir(const char *path);
LIBC_EXPORT int rename(const char *oldpath, const char *newpath);
LIBC_EXPORT int renameat(int olddirfd, const char *oldpath, int newdirfd,
                         const char *newpath);
LIBC_EXPORT int isatty(int fd);
LIBC_EXPORT char *ttyname(int fd);
LIBC_EXPORT int mkdir(const char *path, mode_t mode);

// POSIX identity & permissions (group 1)
LIBC_EXPORT uid_t getuid(void);
LIBC_EXPORT uid_t geteuid(void);
LIBC_EXPORT gid_t getgid(void);
LIBC_EXPORT gid_t getegid(void);
LIBC_EXPORT pid_t getppid(void);
LIBC_EXPORT pid_t getpgrp(void);
LIBC_EXPORT mode_t umask(mode_t mask);
LIBC_EXPORT int gethostname(char *name, size_t len);
LIBC_EXPORT int sethostname(const char *name, size_t len);
LIBC_EXPORT unsigned int alarm(unsigned int seconds);
LIBC_EXPORT int pause(void);
LIBC_EXPORT int truncate(const char *path, off_t length);
LIBC_EXPORT int fsync(int fd);
LIBC_EXPORT void sync(void);
LIBC_EXPORT int getpagesize(void);
LIBC_EXPORT void wait_dev_ready(const char *dev_path);

/* file ownership / symlink — used by libdrm device-node & sysfs paths */
LIBC_EXPORT int chown(const char *path, uid_t owner, gid_t group);
LIBC_EXPORT int fchown(int fd, uid_t owner, gid_t group);
LIBC_EXPORT int fchownat(int dirfd, const char *path, uid_t owner, gid_t group,
                         int flags);
LIBC_EXPORT ssize_t readlink(const char *path, char *buf, size_t bufsiz);

/* sysconf() — POSIX runtime configuration query. _SC_* constants live in the
 * shared uapi header <xos/confname.h> so the kernel-side sys_sysconf and the
 * user-side sysconf compile against identical literals. Unknown names return
 * -1 without setting errno (POSIX). */
#include <xos/confname.h>

LIBC_EXPORT long sysconf(int name);

#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
