#include <sys/ipc.h>
#include <errno.h>
#include "common/syscall.h"

int notify(pid_t pid) {
    int r = sys_notify(pid);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len, uint32_t timeout_ms) {
    int r = sys_recv(msg, data_buf, data_buf_len, timeout_ms);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int req(pid_t pid, void *req_ptr, void *resp) {
    int r = sys_req(pid, req_ptr, resp);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int resp(void *resp) {
    int r = sys_resp(resp);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf, size_t resp_len) {
    int r = sys_msg(pid, req_buf, req_len, resp_buf, resp_len);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int msg_resp(void *resp_buf, size_t resp_len) {
    int r = sys_msg_resp(resp_buf, resp_len);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
