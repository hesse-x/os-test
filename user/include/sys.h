#ifndef USER_SYS_H
#define USER_SYS_H

#include <stdint.h>
#include <stddef.h>
#include "common/syscall.h"

// req_call: 极简 REQ wrapper，委托 sys_req（56 字节载荷限制）
extern "C" int req_call(int32_t server_pid, void *req, size_t req_len,
                        void *resp, size_t resp_len);

#endif // USER_SYS_H
