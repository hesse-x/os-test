#ifndef _SYS_PROCESS_H
#define _SYS_PROCESS_H

#include <sys/types.h>
#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT pid_t fork(void);
LIBC_EXPORT int   execve(const char *pathname, char *const argv[], char *const envp[]);
LIBC_EXPORT pid_t spawn(const char *path);
LIBC_EXPORT pid_t setsid(void);
LIBC_EXPORT int   setpgid(pid_t pid, pid_t pgid);
LIBC_EXPORT pid_t getpgid(pid_t pid);
LIBC_EXPORT pid_t getsid(pid_t pid);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCESS_H */
