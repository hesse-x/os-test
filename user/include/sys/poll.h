#ifndef _SYS_POLL_H
#define _SYS_POLL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_POLL_H */
