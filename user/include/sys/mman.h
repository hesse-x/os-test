#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include "common/mman.h"

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset);
int munmap(void *addr, size_t length);
int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
