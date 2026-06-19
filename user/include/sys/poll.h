#ifndef _SYS_POLL_H
#define _SYS_POLL_H

#include <sys/types.h>
#include "common/socket.h"  // defines struct pollfd, POLLIN/OUT/ERR/HUP

#ifdef __cplusplus
extern "C" {
#endif

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_POLL_H */
