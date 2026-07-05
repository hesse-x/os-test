/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <sys/select.h>
#include <sys/poll.h>
#include <time.h>
#include <errno.h>
#include <stddef.h>

/* select(nfds, readfds, writefds, exceptfds, timeout)
 *
 * Implemented on top of poll(2).  Converts fd_set bitmaps to/from
 * struct pollfd arrays.  Returns immediately (no timeout) if all
 * three bitmap pointers are NULL — this is unspecified but harmless.
 */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
  if (nfds < 0) {
    errno = EINVAL;
    return -1;
  }
  if (nfds > FD_SETSIZE)
    nfds = FD_SETSIZE;

  /* Convert timeout to milliseconds */
  int timeout_ms;
  if (timeout) {
    timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
  } else {
    timeout_ms = -1; /* infinite */
  }

  /* Build pollfd array */
  struct pollfd fds[FD_SETSIZE];
  nfds_t count = 0;
  for (int i = 0; i < nfds; i++) {
    short events = 0;
    if (readfds && FD_ISSET(i, readfds))
      events |= POLLIN;
    if (writefds && FD_ISSET(i, writefds))
      events |= POLLOUT;
    if (exceptfds && FD_ISSET(i, exceptfds))
      events |= POLLERR;
    if (events) {
      fds[count].fd = i;
      fds[count].events = events;
      fds[count].revents = 0;
      count++;
    }
  }

  /* Call poll */
  int ready = poll(fds, count, timeout_ms);
  if (ready < 0)
    return -1; /* errno already set by poll */

  /* Clear all fd_set and re-set based on poll results */
  if (readfds)   FD_ZERO(readfds);
  if (writefds)  FD_ZERO(writefds);
  if (exceptfds) FD_ZERO(exceptfds);

  int result = 0;
  for (nfds_t i = 0; i < count; i++) {
    int fd = fds[i].fd;
    short rev = fds[i].revents;

    if (readfds && (rev & (POLLIN | POLLHUP | POLLERR))) {
      FD_SET(fd, readfds);
      result++;
    }
    if (writefds && (rev & (POLLOUT | POLLHUP | POLLERR))) {
      FD_SET(fd, writefds);
      result++;
    }
    if (exceptfds && (rev & POLLERR)) {
      FD_SET(fd, exceptfds);
      result++;
    }
  }

  return result;
}
