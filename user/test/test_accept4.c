/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// accept4(2): accept() + flags.
//   - SOCK_CLOEXEC sets FD_CLOEXEC on the new fd.
//   - SOCK_NONBLOCK sets O_NONBLOCK on the new socket.
//   - Invalid flags → EINVAL (no connection consumed).

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unity.h>

#include <xos/socket.h>

void setUp(void) {}
void tearDown(void) {}

static const char *g_path = "/tmp/accept4_test";

static int make_listener(void) {
  int lst = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT_TRUE(lst >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, g_path, sizeof(addr.sun_path) - 1);
  unlink(g_path);
  TEST_ASSERT_EQUAL_INT(0, bind(lst, (struct sockaddr *)&addr, sizeof(addr)));
  TEST_ASSERT_EQUAL_INT(0, listen(lst, 4));
  return lst;
}

// accept4 with bogus flags → EINVAL, no child needed.
void test_accept4_bad_flags(void) {
  int lst = make_listener();

  TEST_ASSERT_EQUAL_INT(-1, accept4(lst, NULL, NULL, 0x40000000));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  close(lst);
}

// SOCK_CLOEXEC + SOCK_NONBLOCK: the accepted fd carries both attributes.
void test_accept4_cloexec_nonblock(void) {
  int lst = make_listener();

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0)
      _exit(100);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_path, sizeof(addr.sun_path) - 1);
    if (connect(c, (struct sockaddr *)&addr, sizeof(addr)) != 0)
      _exit(101);
    // The connection is now in the listener backlog; let the parent accept it
    // before this end closes.
    sleep(1);
    close(c);
    _exit(0);
  }

  int a = accept4(lst, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
  TEST_ASSERT_TRUE(a >= 0);
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(a, F_GETFD) & FD_CLOEXEC);
  TEST_ASSERT_EQUAL_INT(O_NONBLOCK, fcntl(a, F_GETFL) & O_NONBLOCK);
  close(a);

  int status = 0;
  waitpid(pid, &status, 0);
  close(lst);
}

void test_so_peercred_snapshot(void) {
  int lst = make_listener();
  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_path, sizeof(addr.sun_path) - 1);
    if (c < 0 || connect(c, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      _exit(100);
    sleep(1);
    _exit(0);
  }

  int accepted = accept(lst, NULL, NULL);
  TEST_ASSERT_TRUE(accepted >= 0);
  struct ucred cred = {0};
  socklen_t len = sizeof(cred);
  TEST_ASSERT_EQUAL_INT(
      0, getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &cred, &len));
  TEST_ASSERT_EQUAL_INT(sizeof(cred), len);
  TEST_ASSERT_EQUAL_INT(pid, cred.pid);
  TEST_ASSERT_EQUAL_INT(geteuid(), cred.uid);
  TEST_ASSERT_EQUAL_INT(getegid(), cred.gid);
  close(accepted);
  close(lst);
  waitpid(pid, NULL, 0);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_accept4_bad_flags);
  RUN_TEST(test_accept4_cloexec_nonblock);
  RUN_TEST(test_so_peercred_snapshot);
  return UNITY_END();
}
