/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * IPC and mount syscall wrappers.
 *
 * Merged from sys_ipc.cc + sys_mount.cc
 */

#include <stdint.h>
#include <sys/ipc.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <syscall.h>

// ===================== IPC =====================

extern "C" int ipc_notify(pid_t pid) { return sys_notify(pid); }

extern "C" int ipc_recv(struct recv_msg *msg, void *data_buf,
                        size_t data_buf_len, uint32_t timeout_ms) {
  return sys_recv(msg, data_buf, data_buf_len, timeout_ms);
}

extern "C" int ipc_req(pid_t pid, void *req_ptr, void *resp) {
  return sys_req(pid, req_ptr, resp);
}

extern "C" int ipc_resp(void *resp, size_t len, int32_t result) {
  return sys_resp(resp, len, result);
}

extern "C" int ipc_msg(int32_t pid, void *req_buf, size_t req_len,
                       void *resp_buf, size_t resp_len) {
  return sys_msg(pid, req_buf, req_len, resp_buf, resp_len);
}

extern "C" int ipc_msg_resp(void *resp_buf, size_t resp_len) {
  return sys_msg_resp(resp_buf, resp_len);
}

// ===================== mount =====================

extern "C" int mount(const char *source, const char *target, const char *fstype,
                     unsigned long flags, const void *data) {
  return sys_mount(source, target, fstype, flags, data);
}
