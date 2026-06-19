#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

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
char *getcwd(char *buf, size_t size);
off_t lseek(int fd, off_t offset, int whence);
unsigned int sleep(unsigned seconds);
int usleep(unsigned usec);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int isatty(int fd);
int mkdir(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
