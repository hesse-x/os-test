#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

LIBC_EXPORT pid_t getpid(void);
LIBC_EXPORT pid_t gettid(void);
LIBC_EXPORT void _exit(int status);
LIBC_EXPORT ssize_t read(int fd, void *buf, size_t count);
LIBC_EXPORT ssize_t write(int fd, const void *buf, size_t count);
LIBC_EXPORT int close(int fd);
LIBC_EXPORT int pipe(int fd[2]);
LIBC_EXPORT int open(const char *path, int flags, ...);
LIBC_EXPORT int dup2(int old_fd, int new_fd);
LIBC_EXPORT int sched_yield(void);
LIBC_EXPORT int ioperm(unsigned long from, unsigned long num, int turn_on);
LIBC_EXPORT char *getcwd(char *buf, size_t size);
LIBC_EXPORT off_t lseek(int fd, off_t offset, int whence);
LIBC_EXPORT int ftruncate(int fd, off_t length);
LIBC_EXPORT int memfd_create(const char *name, unsigned int flags);
LIBC_EXPORT unsigned int sleep(unsigned seconds);
LIBC_EXPORT int usleep(unsigned usec);
LIBC_EXPORT int access(const char *path, int mode);
LIBC_EXPORT int unlink(const char *path);
LIBC_EXPORT int rmdir(const char *path);
LIBC_EXPORT int isatty(int fd);
LIBC_EXPORT char *ttyname(int fd);
LIBC_EXPORT int mkdir(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
