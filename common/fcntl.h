#ifndef _COMMON_FCNTL_H
#define _COMMON_FCNTL_H

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_NONBLOCK  4
#define O_APPEND    8
#define O_CREAT    16
#define O_TRUNC    32

#define O_SETFL_MASK (O_NONBLOCK | O_APPEND)

#define F_GETFL     1
#define F_SETFL     2

// Linux-compatible sealing constants (for memfd_create + fcntl)
#define F_ADD_SEALS   1033
#define F_GET_SEALS   1034
#define F_SEAL_SEAL   0x0001  // further fcntl(F_ADD_SEALS) fails
#define F_SEAL_SHRINK 0x0002  // ftruncate shrink fails
#define F_SEAL_GROW   0x0004  // ftruncate grow fails
#define F_SEAL_WRITE  0x0008  // mmap(PROT_WRITE) fails

#endif /* _COMMON_FCNTL_H */
