#ifndef USER_SYS_H
#define USER_SYS_H

#include <stdint.h>
#include <stddef.h>
#include "common/syscall.h"

// rpc_call: 极简 RPC wrapper，委托 sys_rpc（56 字节载荷限制）
extern "C" int rpc_call(int32_t server_pid, void *req, size_t req_len,
                        void *resp, size_t resp_len);

#endif // USER_SYS_H
