#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define MAP_PHYSICAL 0x80000000
#define MAP_SHARED   0x01

#define MAP_FAILED ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, uint64_t offset);
int munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
