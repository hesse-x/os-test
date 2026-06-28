#ifndef _SYS_PROCESS_H
#define _SYS_PROCESS_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t fork(void);
int   execve(const char *pathname, char *const argv[], char *const envp[]);
pid_t spawn(const char *path);
pid_t setsid(void);
int   setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getsid(pid_t pid);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCESS_H */
