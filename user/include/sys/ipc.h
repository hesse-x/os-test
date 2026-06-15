#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <sys/types.h>
#include <stdint.h>
#include "common/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

int notify(pid_t pid);
int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len, uint32_t timeout_ms);
int req(pid_t pid, void *req, void *resp);
int resp(void *resp);
int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf, size_t resp_len);
int msg_resp(void *resp_buf, size_t resp_len);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IPC_H */
