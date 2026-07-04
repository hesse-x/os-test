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
#define F_GETFD     3
#define F_SETFD     4

/* 注意：POSIX 的 FD_CLOEXEC=1 不在此处定义。内核内部在 kernel/bsd/types.h
 * 用 FD_CLOEXEC=0x8000 作为 fd flags 位（与 O_* 分离）。用户态 FD_CLOEXEC
 * 定义在 user/include/fcntl.h（=1，POSIX 约定）。 */

// Linux-compatible sealing constants (for memfd_create + fcntl)
#define F_ADD_SEALS   1033
#define F_GET_SEALS   1034
#define F_SEAL_SEAL   0x0001  // further fcntl(F_ADD_SEALS) fails
#define F_SEAL_SHRINK 0x0002  // ftruncate shrink fails
#define F_SEAL_GROW   0x0004  // ftruncate grow fails
#define F_SEAL_WRITE  0x0008  // mmap(PROT_WRITE) fails

#endif /* _COMMON_FCNTL_H */
