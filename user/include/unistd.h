#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t getpid(void);
void _exit(int status);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int pipe(int fd[2]);
int open(const char *path, int flags, ...);
int dup2(int old_fd, int new_fd);
int sched_yield(void);
int ioperm(unsigned long from, unsigned long num, int turn_on);

#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
