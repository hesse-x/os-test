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

#endif /* _COMMON_FCNTL_H */
