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

int recv(struct recv_msg *msg, uint32_t timeout_ms) {
    int r = sys_recv(msg, timeout_ms);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int rpc(pid_t pid, void *req, void *resp) {
    int r = sys_rpc(pid, req, resp);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int rpc_reply(void *resp) {
    int r = sys_reply(resp);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
