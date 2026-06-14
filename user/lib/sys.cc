#include "sys.h"

int rpc_call(int32_t server_pid, void *req, size_t req_len,
             void *resp, size_t resp_len) {
    (void)req_len;
    (void)resp_len;
    return sys_rpc(server_pid, req, resp);
}
