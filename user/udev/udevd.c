/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Minimal udevd — subscribes to NETLINK_KOBJECT_UEVENT via epoll,
// receives uevent messages and prints them to stdout.

#include <errno.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
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
      }
    }
  }

  close(epfd);
  close(nl_fd);
  return 0;
}
