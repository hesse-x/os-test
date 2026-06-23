#ifndef COMMON_ERRNO_H
#define COMMON_ERRNO_H

#define EOK      0
#define EPERM    1
#define ENOENT   2
#define ENOMEM   3
#define EINVAL   4
#define ENOSYS   5
#define ECHILD   6
#define EFAULT   7
#define EEXIST   8
#define EBUSY    9
#define ESRCH    10
#define ETIMEDOUT 11
#define EBADF     12
#define EMFILE    13
#define EISDIR    14
#define ENOTDIR   15
#define EIO       16
#define ENOSPC    17
#define EAGAIN    18
#define EPIPE     19
#define EMSGSIZE  20
#define EAFNOSUPPORT 21
#define EPROTONOSUPPORT 22
#define ECONNREFUSED 23
#define ENOTCONN  24
#define EADDRINUSE 25
#define ENOTSOCK  26
#define ENOTSUP   27
#define ENXIO     28
#define ESPIPE    29
#define EROFS     30
#define EMLINK    31
#define EDOM      32
#define ERANGE    33
#define ENOTEMPTY 34
#define ELOOP     35
#define ENODEV    36
#define EWOULDBLOCK EAGAIN

#endif // COMMON_ERRNO_H
