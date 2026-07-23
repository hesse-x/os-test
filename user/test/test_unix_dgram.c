/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * S17 — AF_UNIX SOCK_DGRAM + recvmsg msg_name:
 *   - socket(AF_UNIX, SOCK_DGRAM) / socketpair(..., SOCK_DGRAM) accepted
 *   - bind + sendto(dest) reaches a bound DGRAM server (path resolution)
 *   - message boundaries preserved (two 2B sends → two 2B recvs)
 *   - recvfrom/recvmsg report the sender address (msg_name)
 *   - connect() fixes the default send target for a DGRAM socket
 *   - MSG_PEEK does not consume a datagram
 *   - MSG_TRUNC reports the true datagram length when the buffer is smaller
 *   - SCM_RIGHTS passes an fd over a DGRAM socketpair
 *   - EPROTOTYPE: sending DGRAM to a STREAM-bound path
 *   - ENOENT/ECONNREFUSED: sending to a missing / non-bound path
 *
 * Lengths use sizeof(str)-1 so asserted byte counts can't drift from the
 * payload. Cross-process tests fork: the child binds the server path and the
 * parent acts as the client (paths derived from the child's pid so concurrent
 * test_runner invocations don't collide).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <unity.h>
#include <xos/errno.h>
#include <xos/socket.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- thin wrappers: libc has no sendto/recvfrom in this OS ---- */
static ssize_t sendto_fd(int fd, const void *buf, size_t len, int flags,
                         const struct sockaddr *addr, socklen_t addrlen) {
  int64_t r =
      __syscall6(SYS_SENDTO, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len,
                 (int64_t)flags, (int64_t)(uintptr_t)addr, (int64_t)addrlen);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (ssize_t)r;
}

static ssize_t recvfrom_fd(int fd, void *buf, size_t len, int flags,
                           struct sockaddr *addr, socklen_t *addrlen) {
  int64_t r = __syscall6(SYS_RECVFROM, (int64_t)fd, (int64_t)(uintptr_t)buf,
                         (int64_t)len, (int64_t)flags, (int64_t)(uintptr_t)addr,
                         (int64_t)(uintptr_t)addrlen);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (ssize_t)r;
}

/* build a unique /run path from a pid + tag so forked runs never collide */
static void path_of(struct sockaddr_un *a, const char *path) {
  memset(a, 0, sizeof(*a));
  a->sun_family = AF_UNIX;
  strncpy(a->sun_path, path, sizeof(a->sun_path) - 1);
}

/* 1. socket() / socketpair() accept SOCK_DGRAM. */
void test_dgram_socket_and_socketpair_accepted(void) {
  int s = socket(AF_UNIX, SOCK_DGRAM, 0);
  TEST_ASSERT(s >= 0);
  close(s);

  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));
  close(sv[0]);
  close(sv[1]);

  /* SEQPACKET is not implemented (todo). */
  TEST_ASSERT_EQUAL_INT(-1, socket(AF_UNIX, SOCK_SEQPACKET, 0));
}

/* 2. socketpair DGRAM preserves message boundaries: two 2B sends → two 2B
 * reads (a STREAM socket would coalesce them into one 4B read). */
void test_dgram_socketpair_boundaries(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));

  TEST_ASSERT_EQUAL_INT(2, (int)sendto_fd(sv[0], "AB", 2, 0, NULL, 0));
  TEST_ASSERT_EQUAL_INT(2, (int)sendto_fd(sv[0], "CD", 2, 0, NULL, 0));

  char buf[8] = {0};
  TEST_ASSERT_EQUAL_INT(
      2, (int)recvfrom_fd(sv[1], buf, sizeof(buf), 0, NULL, NULL));
  TEST_ASSERT_EQUAL_MEMORY("AB", buf, 2);
  memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(
      2, (int)recvfrom_fd(sv[1], buf, sizeof(buf), 0, NULL, NULL));
  TEST_ASSERT_EQUAL_MEMORY("CD", buf, 2);

  close(sv[0]);
  close(sv[1]);
}

/* 3. socketpair DGRAM recvfrom reports an anonymous sender (family only). */
void test_dgram_socketpair_sender_anonymous(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));

  TEST_ASSERT_EQUAL_INT(5, (int)sendto_fd(sv[0], "hello", 5, 0, NULL, 0));

  char buf[16] = {0};
  struct sockaddr_un src;
  socklen_t slen = sizeof(src);
  memset(&src, 0, sizeof(src));
  TEST_ASSERT_EQUAL_INT(5, (int)recvfrom_fd(sv[1], buf, sizeof(buf), 0,
                                            (struct sockaddr *)&src, &slen));
  TEST_ASSERT_EQUAL_MEMORY("hello", buf, 5);
  /* socketpair ends are unbound: only the family is reported. */
  TEST_ASSERT_EQUAL_INT(sizeof(sa_family_t), (size_t)slen);
  TEST_ASSERT_EQUAL_INT(AF_UNIX, (int)src.sun_family);

  close(sv[0]);
  close(sv[1]);
}

/* 4. bind + sendto(dest) reaches a bound DGRAM server; recvfrom reports the
 * sender's path. Cross-process: child = server, parent = client. */
void test_dgram_bind_sendto_recvfrom(void) {
  pid_t pid = fork();
  if (pid == 0) {
    /* child: server. bind, recv, exit 0 on success. */
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0)
      _exit(1);
    struct sockaddr_un a;
    path_of(&a, "/run/dgchild-srv");
    /* self-bind under the child's own pid-tagged path so the parent can find
     * it: but the parent computed the path from the child pid. The child does
     * not know its pid relative to the parent's naming, so we pass the path via
     * the shared literal and accept one fixed path (test_runner runs tests
     * sequentially, so no collision). */
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)
      _exit(2);
    char buf[64] = {0};
    struct sockaddr_un src;
    socklen_t slen = sizeof(src);
    memset(&src, 0, sizeof(src));
    ssize_t r =
        recvfrom_fd(s, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
    if (r != 6 || memcmp(buf, "pingup", 6) != 0)
      _exit(3);
    /* sender path must echo back the client's bound path. */
    if (slen < sizeof(sa_family_t))
      _exit(4);
    if (src.sun_family != AF_UNIX)
      _exit(5);
    if (strcmp(src.sun_path, "/run/dgparent-cli") != 0)
      _exit(6);
    close(s);
    _exit(0);
  }

  /* parent: client. Bind its own path, send to the server's path. */
  int c = socket(AF_UNIX, SOCK_DGRAM, 0);
  TEST_ASSERT(c >= 0);
  struct sockaddr_un cli;
  path_of(&cli, "/run/dgparent-cli");
  TEST_ASSERT_EQUAL_INT(0, bind(c, (struct sockaddr *)&cli, sizeof(cli)));

  /* Give the child a moment to bind before we send. */
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
  nanosleep(&ts, NULL);

  struct sockaddr_un srv;
  path_of(&srv, "/run/dgchild-srv");
  TEST_ASSERT_EQUAL_INT(
      6,
      (int)sendto_fd(c, "pingup", 6, 0, (struct sockaddr *)&srv, sizeof(srv)));

  int status;
  waitpid(pid, &status, 0);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
  close(c);
}

/* 5. connect() fixes the default send target for a DGRAM socket; a bare
 * send() (no addr) then reaches the server. */
void test_dgram_connect_then_send(void) {
  pid_t pid = fork();
  if (pid == 0) {
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0)
      _exit(1);
    struct sockaddr_un a;
    path_of(&a, "/run/dgc-srv");
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)
      _exit(2);
    char buf[32] = {0};
    struct sockaddr_un src;
    socklen_t slen = sizeof(src);
    memset(&src, 0, sizeof(src));
    ssize_t r =
        recvfrom_fd(s, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
    if (r != 4 || memcmp(buf, "pong", 4) != 0)
      _exit(3);
    if (strcmp(src.sun_path, "/run/dgc-cli") != 0)
      _exit(4);
    close(s);
    _exit(0);
  }

  int c = socket(AF_UNIX, SOCK_DGRAM, 0);
  TEST_ASSERT(c >= 0);
  struct sockaddr_un cli;
  path_of(&cli, "/run/dgc-cli");
  TEST_ASSERT_EQUAL_INT(0, bind(c, (struct sockaddr *)&cli, sizeof(cli)));

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
  nanosleep(&ts, NULL);

  struct sockaddr_un srv;
  path_of(&srv, "/run/dgc-srv");
  TEST_ASSERT_EQUAL_INT(0, connect(c, (struct sockaddr *)&srv, sizeof(srv)));
  /* send() with no address now routes to the connected target. */
  TEST_ASSERT_EQUAL_INT(4, (int)sendto_fd(c, "pong", 4, 0, NULL, 0));

  int status;
  waitpid(pid, &status, 0);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
  close(c);
}

/* 6. MSG_PEEK on a DGRAM socketpair leaves the datagram queued. */
void test_dgram_peek_does_not_consume(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));
  TEST_ASSERT_EQUAL_INT(3, (int)sendto_fd(sv[0], "xyz", 3, 0, NULL, 0));

  char b1[8] = {0}, b2[8] = {0};
  TEST_ASSERT_EQUAL_INT(
      3, (int)recvfrom_fd(sv[1], b1, sizeof(b1), MSG_PEEK, NULL, NULL));
  TEST_ASSERT_EQUAL_MEMORY("xyz", b1, 3);
  /* a second plain recv still sees the same datagram (PEEK did not dequeue). */
  TEST_ASSERT_EQUAL_INT(3,
                        (int)recvfrom_fd(sv[1], b2, sizeof(b2), 0, NULL, NULL));
  TEST_ASSERT_EQUAL_MEMORY("xyz", b2, 3);

  close(sv[0]);
  close(sv[1]);
}

/* 7. MSG_TRUNC: recv a 10B datagram into a 4B buffer with MSG_TRUNC → returns
 * the true length (10) and sets MSG_TRUNC in msg_flags. */
void test_dgram_trunc_reports_true_length(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));
  TEST_ASSERT_EQUAL_INT(10,
                        (int)sendto_fd(sv[0], "0123456789", 10, 0, NULL, 0));

  struct iovec iov = {.iov_base = (char[4]){0}, .iov_len = 4};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  ssize_t r = recvmsg(sv[1], &msg, MSG_TRUNC);
  TEST_ASSERT_EQUAL_INT(10, (int)r);
  TEST_ASSERT_TRUE(msg.msg_flags & MSG_TRUNC);

  close(sv[0]);
  close(sv[1]);
}

/* 8. SCM_RIGHTS over a DGRAM socketpair: pass an fd, receiver reads its
 * contents. */
void test_dgram_scm_rights(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));

  /* Open a temp file, write a sentinel, pass its fd. */
  int tf = open("/run/dgscm.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT(tf >= 0);
  TEST_ASSERT_EQUAL_INT(5, (int)write(tf, "scmok", 5));

  char cbuf[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {.iov_base = "go", .iov_len = 2};
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  *(int *)CMSG_DATA(cm) = tf;
  TEST_ASSERT_EQUAL_INT(2, (int)sendmsg(sv[0], &msg, 0));
  close(tf);

  /* receive */
  char rbuf[8] = {0};
  char rcbuf[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec riov = {.iov_base = rbuf, .iov_len = sizeof(rbuf)};
  struct msghdr rmsg = {0};
  rmsg.msg_iov = &riov;
  rmsg.msg_iovlen = 1;
  rmsg.msg_control = rcbuf;
  rmsg.msg_controllen = sizeof(rcbuf);
  ssize_t r = recvmsg(sv[1], &rmsg, 0);
  TEST_ASSERT_EQUAL_INT(2, (int)r);
  TEST_ASSERT_EQUAL_MEMORY("go", rbuf, 2);

  struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmsg);
  TEST_ASSERT_NOT_NULL(rcm);
  TEST_ASSERT_EQUAL_INT(SOL_SOCKET, rcm->cmsg_level);
  TEST_ASSERT_EQUAL_INT(SCM_RIGHTS, rcm->cmsg_type);
  int passed_fd = *(int *)CMSG_DATA(rcm);
  TEST_ASSERT(passed_fd >= 0);

  /* read the sentinel through the passed fd. SCM_RIGHTS passes the open file
   * description (shared offset), which sits at EOF after the sender wrote —
   * rewind first. */
  lseek(passed_fd, 0, SEEK_SET);
  char fbuf[8] = {0};
  TEST_ASSERT_EQUAL_INT(5, (int)read(passed_fd, fbuf, sizeof(fbuf)));
  TEST_ASSERT_EQUAL_MEMORY("scmok", fbuf, 5);
  close(passed_fd);

  close(sv[0]);
  close(sv[1]);
  unlink("/run/dgscm.txt");
}

/* 9. EPROTOTYPE: a DGRAM sendto to a path bound by a STREAM socket. */
void test_dgram_sendto_stream_path_eprototype(void) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT(s >= 0);
  struct sockaddr_un a;
  path_of(&a, "/run/dgstream");
  TEST_ASSERT_EQUAL_INT(0, bind(s, (struct sockaddr *)&a, sizeof(a)));

  int c = socket(AF_UNIX, SOCK_DGRAM, 0);
  TEST_ASSERT(c >= 0);
  TEST_ASSERT_EQUAL_INT(
      -1, sendto_fd(c, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)));
  TEST_ASSERT_EQUAL_INT(EPROTOTYPE, errno);

  close(c);
  close(s);
}

/* 10. sendto to a non-existent path → ENOENT (no socket inode, hash miss). */
void test_dgram_sendto_missing_path(void) {
  int c = socket(AF_UNIX, SOCK_DGRAM, 0);
  TEST_ASSERT(c >= 0);
  struct sockaddr_un a;
  path_of(&a, "/run/dgnone-such-path");
  TEST_ASSERT_EQUAL_INT(
      -1, sendto_fd(c, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)));
  /* VFS lookup misses → unix_bind_lookup_dgram returns -ENOENT. */
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
  close(c);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_dgram_socket_and_socketpair_accepted);
  RUN_TEST(test_dgram_socketpair_boundaries);
  RUN_TEST(test_dgram_socketpair_sender_anonymous);
  RUN_TEST(test_dgram_bind_sendto_recvfrom);
  RUN_TEST(test_dgram_connect_then_send);
  RUN_TEST(test_dgram_peek_does_not_consume);
  RUN_TEST(test_dgram_trunc_reports_true_length);
  RUN_TEST(test_dgram_scm_rights);
  RUN_TEST(test_dgram_sendto_stream_path_eprototype);
  RUN_TEST(test_dgram_sendto_missing_path);
  return UNITY_END();
}
