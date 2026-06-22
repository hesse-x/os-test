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
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x04

#define MAP_FAILED ((void *)-1)

// memfd_create flags
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

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
