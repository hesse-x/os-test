#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stdint.h>
#include <xos/types.h>

typedef int64_t off_t;
typedef uint32_t dev_t;
typedef uint32_t ino_t;
typedef uint32_t mode_t;
typedef uint32_t nlink_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t  blksize_t;
typedef int32_t  blkcnt_t;

typedef unsigned long nfds_t;

typedef long ssize_t;
typedef long useconds_t;

#endif /* _SYS_TYPES_H */
