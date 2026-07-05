#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include "syscall.h"
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/poll.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int notify(pid_t pid);
LIBC_EXPORT int notify_fd(int fd);
LIBC_EXPORT int msg_fd(int fd, const void *msg_buf, size_t msg_len,
                       void *reply_buf, size_t reply_len);
LIBC_EXPORT int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len,
                     uint32_t timeout_ms);
LIBC_EXPORT int req(pid_t pid, void *req, void *resp);
LIBC_EXPORT int resp(void *resp);
LIBC_EXPORT int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf,
                    size_t resp_len);
LIBC_EXPORT int msg_resp(void *resp_buf, size_t resp_len);
LIBC_EXPORT int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IPC_H */
