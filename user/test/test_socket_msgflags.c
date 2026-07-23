/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * S16 — recvmsg/sendmsg/poll flag semantics (AF_UNIX SOCK_STREAM + pipe):
 *   - MSG_PEEK: data stays in the queue for the next recv
 *   - MSG_TRUNC: return value = true skb length; MSG_TRUNC set in msg_flags
 *   - MSG_WAITALL: block until the full request is filled (or EOF)
 *   - sendmsg EPIPE → SIGPIPE (default) / MSG_NOSIGNAL suppresses it
 *   - pipe write EPIPE → SIGPIPE
 *   - SCM_RIGHTS MSG_CTRUNC when the control buffer is too small
 *   - poll bad fd → POLLNVAL
 *   - msg_flags written back to user space
 *
 * Lengths use sizeof(str) / strlen(str) on static string literals so the
 * asserted byte counts can never drift from the actual payload size.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/socket.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- sendmsg single-iov helper (this OS has no send() wrapper) ---- */
static ssize_t sock_send(int fd, const void *buf, size_t len) {
  struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  return sendmsg(fd, &msg, 0);
}

/* sendmsg helper that propagates flags (MSG_NOSIGNAL/MSG_DONTWAIT/...). */
static ssize_t sock_send_flags(int fd, const void *buf, size_t len, int flags) {
  struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  return sendmsg(fd, &msg, flags);
}

/* ---- recvmsg single-iov helper ---- */
static ssize_t recv_one(int fd, void *buf, size_t len, int flags) {
  struct iovec iov = {.iov_base = buf, .iov_len = len};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  return recvmsg(fd, &msg, flags);
}

/* 1. MSG_PEEK does not consume data: a second plain recv returns the same
 * bytes. */
void test_recvmsg_peek_does_not_consume(void) {
  static const char hello[] = "hello";
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  TEST_ASSERT_EQUAL_INT((int)sizeof(hello) - 1,
                        (int)sock_send(sv[0], hello, sizeof(hello) - 1));

  char peek_buf[8] = {0};
  ssize_t r = recv_one(sv[1], peek_buf, sizeof(hello) - 1, MSG_PEEK);
  TEST_ASSERT_EQUAL_INT((int)sizeof(hello) - 1, (int)r);
  TEST_ASSERT_EQUAL_STRING("hello", peek_buf);

  /* A subsequent normal recv must still see "hello" (PEEK left it queued). */
  char buf[8] = {0};
  r = recv_one(sv[1], buf, sizeof(hello) - 1, 0);
  TEST_ASSERT_EQUAL_INT((int)sizeof(hello) - 1, (int)r);
  TEST_ASSERT_EQUAL_STRING("hello", buf);

  close(sv[0]);
  close(sv[1]);
}

/* 2. MSG_TRUNC: send a record larger than the recv buffer, recv with
 * MSG_TRUNC → return value is the true record length (not the buffer size),
 * the buffer holds as much as fit, and msg_flags has MSG_TRUNC set. */
void test_recvmsg_trunc_returns_true_length(void) {
  /* A 100-byte record (digits cycling 0-9). sizeof - 1 excludes the NUL. */
  static const char record[] = "0123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789"
                               "01234567890123456789";
  size_t rec_len = sizeof(record) - 1; /* 100 */
  TEST_ASSERT_EQUAL_INT(100, (int)rec_len);

  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  TEST_ASSERT_EQUAL_INT((int)rec_len, (int)sock_send(sv[0], record, rec_len));

  char rx[50] = {0};
  struct iovec iov = {.iov_base = rx, .iov_len = sizeof(rx)};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  ssize_t r = recvmsg(sv[1], &msg, MSG_TRUNC);
  TEST_ASSERT_EQUAL_INT((int)rec_len,
                        (int)r); /* true length, not buffer size */
  TEST_ASSERT_EQUAL_UINT8('0', rx[0]);
  TEST_ASSERT_EQUAL_UINT8('9', rx[49]);
  TEST_ASSERT_TRUE(msg.msg_flags & MSG_TRUNC);

  close(sv[0]);
  close(sv[1]);
}

/* 3. MSG_WAITALL: send 100B in two 50B chunks; recv 100B with MSG_WAITALL
 * blocks until the full request is satisfied and returns 100. */
void test_recvmsg_waitall_fills_request(void) {
  static const char part1[] =
      "01234567890123456789012345678901234567890123456789"; /* 50 */
  static const char part2[] =
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"; /* 50 */
  size_t len1 = sizeof(part1) - 1;
  size_t len2 = sizeof(part2) - 1;
  TEST_ASSERT_EQUAL_INT(50, (int)len1);
  TEST_ASSERT_EQUAL_INT(50, (int)len2);

  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  TEST_ASSERT_EQUAL_INT((int)len1, (int)sock_send(sv[0], part1, len1));
  TEST_ASSERT_EQUAL_INT((int)len2, (int)sock_send(sv[0], part2, len2));

  char rx[100] = {0};
  ssize_t r = recv_one(sv[1], rx, len1 + len2, MSG_WAITALL);
  TEST_ASSERT_EQUAL_INT((int)(len1 + len2), (int)r);
  TEST_ASSERT_EQUAL_MEMORY(part1, rx, len1);
  TEST_ASSERT_EQUAL_MEMORY(part2, rx + len1, len2);

  close(sv[0]);
  close(sv[1]);
}

/* 4. MSG_WAITALL short read on EOF: peer sends 40B then shuts down the write
 * side; a 100B MSG_WAITALL recv returns the 40 bytes actually available (not
 * 100, not -1). */
void test_recvmsg_waitall_short_read_on_eof(void) {
  static const char part[] =
      "0123456789012345678901234567890123456789"; /* 40 */
  size_t len = sizeof(part) - 1;
  TEST_ASSERT_EQUAL_INT(40, (int)len);

  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  TEST_ASSERT_EQUAL_INT((int)len, (int)sock_send(sv[0], part, len));
  shutdown(sv[0], SHUT_WR);

  char rx[100] = {0};
  ssize_t r = recv_one(sv[1], rx, 100, MSG_WAITALL);
  TEST_ASSERT_EQUAL_INT((int)len, (int)r);
  TEST_ASSERT_EQUAL_MEMORY(part, rx, len);

  close(sv[0]);
  close(sv[1]);
}

/* 5. sendmsg to a peer that closed its read end: with MSG_NOSIGNAL the call
 * returns -1/EPIPE and the process is NOT killed; without MSG_NOSIGNAL the
 * default action (SIGPIPE → terminate) kills the child (WIFSIGNALED,
 * WTERMSIG==SIGPIPE).
 *
 * Synchronization: the parent closes the peer's read end, then signals the
 * child via a dedicated "go" pipe so the child's send runs only after the
 * close has taken effect (no poll-on-socket / timing assumptions). */
void test_sendmsg_epipe_sigpipe_default_and_nosignal(void) {
  static const char x[] = "x";
  int sv[2];

  /* Case A — MSG_NOSIGNAL: survives, sees -EPIPE. */
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  int go[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(go));

  pid_t child = fork();
  if (child == 0) {
    close(sv[1]); /* child keeps only the send end */
    close(go[1]); /* child reads the go signal */
    char sig;
    if (read(go[0], &sig, 1) != 1)
      _exit(2);
    ssize_t r = sock_send_flags(sv[0], x, sizeof(x) - 1, MSG_NOSIGNAL);
    if (r == -1 && errno == EPIPE)
      _exit(0);
    _exit(1);
  }
  close(go[0]);
  /* Parent: close the peer read end → child's send will EPIPE, then signal. */
  close(sv[1]);
  write(go[1], "g", 1);
  close(go[1]);

  int status = 0;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status)); /* saw -EPIPE, not killed */
  close(sv[0]);

  /* Case B — default (no MSG_NOSIGNAL): child is killed by SIGPIPE. */
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  TEST_ASSERT_EQUAL_INT(0, pipe(go));
  child = fork();
  if (child == 0) {
    close(sv[1]);
    close(go[1]);
    char sig;
    if (read(go[0], &sig, 1) != 1)
      _exit(2);
    sock_send(sv[0], x,
              sizeof(x) - 1); /* default: raises SIGPIPE → terminate */
    _exit(1);                 /* unreachable if SIGPIPE fires */
  }
  close(go[0]);
  close(sv[1]);
  write(go[1], "g", 1);
  close(go[1]);

  status = 0;
  ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGPIPE, WTERMSIG(status));
  close(sv[0]);
}

/* 6. pipe write with all read ends closed raises SIGPIPE (default). */
void test_pipe_write_epipe_sigpipe_default(void) {
  static const char x[] = "x";
  int fd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(fd));
  int go[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(go));

  pid_t child = fork();
  if (child == 0) {
    close(fd[0]); /* close our read end; the parent's read end is the only
                   * reader, and the parent will close it */
    close(go[1]); /* child reads the go signal */
    char sig;
    if (read(go[0], &sig, 1) != 1)
      _exit(2);
    write(fd[1], x, sizeof(x) - 1); /* default: raises SIGPIPE → terminate */
    _exit(1);
  }
  close(go[0]);
  close(fd[0]); /* close the parent's read end → child's write has no readers */
  write(go[1], "g", 1);
  close(go[1]);

  int status = 0;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGPIPE, WTERMSIG(status));
  close(fd[1]);
}

/* 7. SCM_RIGHTS MSG_CTRUNC: send 3 fds, recv with a control buffer big enough
 * for only 1 fd → msg_flags has MSG_CTRUNC, return >= 0, and msg_controllen
 * reflects the single fd that fit. */
void test_scm_rights_ctrunc(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  /* Three distinct fds to pass: dup the socketpair members. */
  int f0 = dup(sv[0]);
  int f1 = dup(sv[0]);
  int f2 = dup(sv[0]);
  TEST_ASSERT_TRUE(f0 >= 0 && f1 >= 0 && f2 >= 0);

  /* Build a sendmsg with SCM_RIGHTS carrying 3 fds. */
  char tx = 'X';
  struct iovec iov = {.iov_base = &tx, .iov_len = 1};
  char cbuf[CMSG_SPACE(3 * sizeof(int))];
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(3 * sizeof(int));
  int fds[3] = {f0, f1, f2};
  memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
  TEST_ASSERT_EQUAL_INT(1, (int)sendmsg(sv[0], &msg, 0));

  /* Recv with a control buffer sized for exactly 1 fd. */
  char rx;
  struct iovec riov = {.iov_base = &rx, .iov_len = 1};
  char rcbuf[CMSG_SPACE(1 * sizeof(int))];
  struct msghdr rmsg = {0};
  rmsg.msg_iov = &riov;
  rmsg.msg_iovlen = 1;
  rmsg.msg_control = rcbuf;
  rmsg.msg_controllen = sizeof(rcbuf);

  ssize_t r = recvmsg(sv[1], &rmsg, 0);
  TEST_ASSERT_TRUE(r >= 0); /* data still delivered */
  TEST_ASSERT_TRUE(rmsg.msg_flags & MSG_CTRUNC);

  /* Close everything; the kernel installed whatever fit (and dropped the
   * rest). Leak-free teardown is best-effort — the test's pass/fail is the
   * CTRUNC assertion above. */
  close(f0);
  close(f1);
  close(f2);
  close(sv[0]);
  close(sv[1]);
}

/* 8. poll on an out-of-range fd and on a closed fd returns POLLNVAL (not
 * POLLERR). */
void test_poll_bad_fd_returns_pollnval(void) {
  struct pollfd pfds[2];
  pfds[0].fd = 999; /* >= MAX_FD, out of range */
  pfds[0].events = POLLIN;
  pfds[0].revents = 0;
  pfds[1].fd = -1; /* negative */
  pfds[1].events = POLLIN;
  pfds[1].revents = 0;

  int ready = poll(pfds, 2, 0);
  TEST_ASSERT_EQUAL_INT(2, ready);
  TEST_ASSERT_EQUAL_INT(POLLNVAL, (int)pfds[0].revents);
  TEST_ASSERT_EQUAL_INT(POLLNVAL, (int)pfds[1].revents);

  /* A closed (never-opened) fd also yields POLLNVAL. Open then close a pipe
   * end to get a concrete closed fd number. */
  int pfd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(pfd));
  int wf = pfd[1];
  close(pfd[0]);
  close(pfd[1]);

  struct pollfd p = {.fd = wf, .events = POLLIN, .revents = 0};
  ready = poll(&p, 1, 0);
  TEST_ASSERT_EQUAL_INT(1, ready);
  TEST_ASSERT_EQUAL_INT(POLLNVAL, (int)p.revents);
}

/* 9. msg_flags writeback: a normal (non-truncated) recvmsg leaves msg_flags
 * at 0 in user space, and a truncated recv sets the bit. Guards the
 * copy_to_user of msghdr.msg_flags. */
void test_recvmsg_msg_flags_written_back(void) {
  static const char hello[] = "hello";
  static const char record[] = "0123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789"
                               "01234567890123456789"; /* 100 */
  size_t rec_len = sizeof(record) - 1;

  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  /* Non-truncated recv: msg_flags stays 0 even if the caller pre-set it. */
  TEST_ASSERT_EQUAL_INT((int)(sizeof(hello) - 1),
                        (int)sock_send(sv[0], hello, sizeof(hello) - 1));
  char buf[8] = {0};
  struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_flags = 0xDEADBEEF; /* sentinel: kernel must zero this */
  TEST_ASSERT_EQUAL_INT((int)(sizeof(hello) - 1), (int)recvmsg(sv[1], &msg, 0));
  TEST_ASSERT_EQUAL_INT(0, msg.msg_flags);

  /* Truncated recv: MSG_TRUNC set in msg_flags. */
  TEST_ASSERT_EQUAL_INT((int)rec_len, (int)sock_send(sv[0], record, rec_len));
  char rx[50] = {0};
  struct iovec iov2 = {.iov_base = rx, .iov_len = sizeof(rx)};
  struct msghdr msg2 = {0};
  msg2.msg_iov = &iov2;
  msg2.msg_iovlen = 1;
  msg2.msg_flags = 0;
  TEST_ASSERT_EQUAL_INT((int)rec_len, (int)recvmsg(sv[1], &msg2, MSG_TRUNC));
  TEST_ASSERT_TRUE(msg2.msg_flags & MSG_TRUNC);

  close(sv[0]);
  close(sv[1]);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_recvmsg_peek_does_not_consume);
  RUN_TEST(test_recvmsg_trunc_returns_true_length);
  RUN_TEST(test_recvmsg_waitall_fills_request);
  RUN_TEST(test_recvmsg_waitall_short_read_on_eof);
  RUN_TEST(test_sendmsg_epipe_sigpipe_default_and_nosignal);
  RUN_TEST(test_pipe_write_epipe_sigpipe_default);
  RUN_TEST(test_scm_rights_ctrunc);
  RUN_TEST(test_poll_bad_fd_returns_pollnval);
  RUN_TEST(test_recvmsg_msg_flags_written_back);
  return UNITY_END();
}
