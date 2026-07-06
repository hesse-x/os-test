/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <syscall.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/types.h>

int notify(pid_t pid) { return sys_notify(pid); }

int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len,
         uint32_t timeout_ms) {
  return sys_recv(msg, data_buf, data_buf_len, timeout_ms);
}

int req(pid_t pid, void *req_ptr, void *resp) {
  return sys_req(pid, req_ptr, resp);
}

int resp(void *resp) { return sys_resp(resp); }

int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf,
        size_t resp_len) {
  return sys_msg(pid, req_buf, req_len, resp_buf, resp_len);
}

int msg_resp(void *resp_buf, size_t resp_len) {
  return sys_msg_resp(resp_buf, resp_len);
}
