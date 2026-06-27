#ifndef COMMON_MMAN_H
#define COMMON_MMAN_H

// Memory protection flags
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

// Mapping types
#define MAP_SHARED   0x01
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x04
#define MAP_PHYSICAL  0x80000000
#define MAP_UC  0x08  // Map as uncacheable (for device MMIO)

// mmap return value on failure
#define MAP_FAILED ((void *)-1)

// memfd_create flags
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

#endif /* COMMON_MMAN_H */
