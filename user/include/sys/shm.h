#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int shm_create(size_t size, void **addr);
int shm_attach(pid_t target, void **addr);
int shm_attach_kernel(int shm_id, void **addr);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SHM_H */
