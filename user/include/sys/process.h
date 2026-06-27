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

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCESS_H */
