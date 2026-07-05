#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stdint.h>
#include <xos/types.h>

typedef int64_t off_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t mode_t;
typedef uint64_t nlink_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;

typedef unsigned long nfds_t;

typedef long ssize_t;
typedef long useconds_t;

#endif /* _SYS_TYPES_H */
