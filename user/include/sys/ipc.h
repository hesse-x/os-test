#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <sys/types.h>
#include <stdint.h>
#include "common/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

int notify(pid_t pid);
int recv(struct recv_msg *msg, uint32_t timeout_ms);
int rpc(pid_t pid, void *req, void *resp);
int rpc_reply(void *resp);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IPC_H */
