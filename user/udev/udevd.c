/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Minimal udevd — subscribes to NETLINK_KOBJECT_UEVENT via epoll,
// receives uevent messages and prints them to stdout.
//
// socket activation (dep0 1.1)：init 可经 fd 3 传入一个已 listen 的
// AF_UNIX socket（/run/udev/socket）。本进程开头探测 fd 3：
//   - 是 listen socket → 纳入 epoll，accept 客户端连接（udevd
//   主体方案接管处理）
//   - 非 socket / 无效 → 跳过，仅跑 netlink（降级，udevd 仍能起）
// 探测用 try-accept（本 OS 无 getsockname）。

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define UDEV_LISTEN_FD 3

/* 探测 fd 是否为已 listen 的 AF_UNIX socket。
 * 返 >=0（== fd）表示是 listen socket；-1 表示非 socket / 无效。 */
static int probe_listen_fd(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1; /* fd 无效 */
  /* 置非阻塞后 try-accept：成功/ EAGAIN 都说明是 listen socket */
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int probe = accept(fd, NULL, NULL);
  int saved = errno;
  fcntl(fd, F_SETFL, flags); /* 还原 */
  if (probe >= 0) {
    close(probe); /* 排队的 client 立即关，真实 client 重连 */
    return fd;
  }
  if (saved == EAGAIN)
    return fd; /* 无排队，socket activation 成立 */
  /* ENOTSOCK / EINVAL 等 → 非 listen socket */
  return -1;
}

/* 降级自 bind+listen：init 未传 fd 3 或探测失败时，udevd 自建 listen socket。
 * 保证 udevd 永远能 accept 客户端连接，不硬依赖 init socket activation。 */
static int fallback_self_bind_listen(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/udev/socket", sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 8) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int main(void) {
  int listen_fd = probe_listen_fd(UDEV_LISTEN_FD);
  if (listen_fd < 0)
    listen_fd = fallback_self_bind_listen();

  // Create netlink socket
  int nl_fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  if (nl_fd < 0) {
    printf("udevd: socket(AF_NETLINK) failed: errno=%d\n", errno);
    return 1;
  }

  // Bind: subscribe to uevent group (bit 0 = group 1)
  struct sockaddr_nl addr;
  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  addr.nl_pid = 0;    // auto-assign PID
  addr.nl_groups = 1; // subscribe to group 1 (UEVENT)
  if (bind(nl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    printf("udevd: bind failed: errno=%d\n", errno);
    close(nl_fd);
    return 1;
  }

  // Create epoll fd
  int epfd = epoll_create1(0);
  if (epfd < 0) {
    printf("udevd: epoll_create1 failed: errno=%d\n", errno);
    close(nl_fd);
    return 1;
  }

  // Register netlink fd with epoll
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = nl_fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, nl_fd, &ev) < 0) {
    printf("udevd: epoll_ctl ADD failed: errno=%d\n", errno);
    close(epfd);
    close(nl_fd);
    return 1;
  }

  /* socket activation：将 init 传入的 listen fd 纳入 epoll（非阻塞）。
   * udevd 主体方案在此 accept 客户端并处理 udev 协议；本轮只保活 + accept。
   * listen_fd < 0（无 socket activation）时跳过，udevd 仍跑 netlink。 */
  if (listen_fd >= 0) {
    struct epoll_event lev;
    lev.events = EPOLLIN;
    lev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &lev);
    printf("udevd: socket activation on fd=%d\n", listen_fd);
  } else {
    printf("udevd: no socket activation fd, netlink-only\n");
  }

  printf("udevd: listening for uevents on fd=%d\n", nl_fd);

  // Event loop
  while (1) {
    struct epoll_event events[4];
    int n = epoll_wait(epfd, events, 4, -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      printf("udevd: epoll_wait error: errno=%d\n", errno);
      break;
    }

    for (int i = 0; i < n; i++) {
      if (events[i].data.fd == nl_fd) {
        // Read uevent from netlink socket
        char buf[4096];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t len = recvmsg(nl_fd, &msg, 0);
        if (len < 0) {
          printf("udevd: recvmsg error: errno=%d\n", errno);
          continue;
        }

        // Parse nlmsghdr
        if (len < (ssize_t)sizeof(struct nlmsghdr))
          continue;

        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        if (!NLMSG_OK(nh, len))
          continue;

        // Extract payload
        char *payload = (char *)NLMSG_DATA(nh);
        int payload_len = (int)(nh->nlmsg_len) - NLMSG_HDRLEN;

        // Print uevent info
        printf("udevd: uevent type=%d pid=%u len=%d: ", nh->nlmsg_type,
               nh->nlmsg_pid, payload_len);

        // Payload is \0-separated key-value pairs; print first segment
        // (the "action@devpath" line) then remaining pairs
        char *p = payload;
        int remaining = payload_len;
        while (remaining > 0 && *p) {
          printf("[%s] ", p);
          int seg_len = (int)strlen(p) + 1;
          p += seg_len;
          remaining -= seg_len;
        }
        printf("\n");
      } else if (events[i].data.fd == listen_fd) {
        /* socket activation accept：udevd 主体方案接管 udev 协议处理；
         * 本轮保活——accept 出连接并立即关闭占位（真实处理见 udevd 主体）。 */
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0)
          close(cfd);
      }
    }
  }

  close(epfd);
  close(nl_fd);
  return 0;
}
