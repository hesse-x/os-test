/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_inotify — inotify Unity tests (inotify.md §7.4, P0 coverage).
 * Operates on the root FAT32 filesystem under /inotify_*. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* FAT32 creation currently stores 8.3 names, so keep these unique within the
 * first eight characters as well as at the POSIX path level. */
static const char *TEST_FILE = "/ino_file";
static const char *TEST_DIR = "/ino_dir";

/* IN-001: inotify_init1 returns a valid fd; bad flags → EINVAL. */
void test_inotify_init(void) {
  int fd = inotify_init1(0);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);

  fd = inotify_init1(IN_CLOEXEC);
  TEST_ASSERT_TRUE(fd >= 0);
  int flags = fcntl(fd, F_GETFD);
  TEST_ASSERT_TRUE(flags & FD_CLOEXEC);
  close(fd);

  fd = inotify_init1(IN_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);

  /* Bogus flag bit → EINVAL. */
  fd = inotify_init1(0x40000000);
  TEST_ASSERT_EQUAL_INT(-1, fd);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* IN-002: add_watch returns wd>=1; dup add returns same wd; rm then rm →
 * EINVAL. */
void test_inotify_add_rm(void) {
  int fd = inotify_init1(0);
  TEST_ASSERT_TRUE(fd >= 0);

  int wd = inotify_add_watch(fd, TEST_FILE, IN_MODIFY);
  TEST_ASSERT_TRUE(wd >= 1);

  /* Re-adding the same inode returns the same wd (default replace semantics).
   */
  int wd2 = inotify_add_watch(fd, TEST_FILE, IN_MODIFY);
  TEST_ASSERT_EQUAL_INT(wd, wd2);

  /* rm_watch succeeds. */
  TEST_ASSERT_EQUAL_INT(0, inotify_rm_watch(fd, wd));
  /* rm again → EINVAL (no such wd). */
  TEST_ASSERT_EQUAL_INT(-1, inotify_rm_watch(fd, wd));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  /* rm on a bogus fd → EBADF. */
  TEST_ASSERT_EQUAL_INT(-1, inotify_rm_watch(999, wd));
  close(fd);
}

/* IN-003: writing a watched file delivers IN_MODIFY + IN_CLOSE_WRITE. */
void test_inotify_basic_event(void) {
  int fd = inotify_init1(0);
  TEST_ASSERT_TRUE(fd >= 0);
  int wd = inotify_add_watch(fd, TEST_FILE, IN_MODIFY | IN_CLOSE_WRITE);
  TEST_ASSERT_TRUE(wd >= 1);

  int f = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_TRUE(f >= 0);
  TEST_ASSERT_EQUAL_INT(4, (int)write(f, "abcd", 4));
  close(f);

  /* Expect IN_MODIFY and IN_CLOSE_WRITE (order: modify first, then close). */
  struct inotify_event ev[8];
  int n = read(fd, ev, sizeof(ev));
  TEST_ASSERT_TRUE(n > 0);

  uint32_t got = 0;
  int off = 0;
  while (off < n) {
    struct inotify_event *e = (struct inotify_event *)((char *)ev + off);
    if (e->wd == wd)
      got |= e->mask;
    off += sizeof(struct inotify_event) + e->len;
  }
  TEST_ASSERT_TRUE(got & IN_MODIFY);
  TEST_ASSERT_TRUE(got & IN_CLOSE_WRITE);

  inotify_rm_watch(fd, wd);
  close(fd);
}

/* IN-004: watching a directory delivers IN_CREATE / IN_DELETE with the child
 * name in event->name. */
void test_inotify_dir_event(void) {
  /* Ensure a clean test dir. */
  rmdir(TEST_DIR);
  TEST_ASSERT_EQUAL_INT(0, mkdir(TEST_DIR, 0755));

  int fd = inotify_init1(0);
  TEST_ASSERT_TRUE(fd >= 0);
  int wd = inotify_add_watch(fd, TEST_DIR, IN_CREATE | IN_DELETE);
  TEST_ASSERT_TRUE(wd >= 1);

  const char *child = "/ino_dir/child";
  int f = open(child, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_TRUE(f >= 0);
  close(f);
  unlink(child);

  struct inotify_event ev[8];
  int n = read(fd, ev, sizeof(ev));
  TEST_ASSERT_TRUE(n > 0);

  int saw_create = 0, saw_delete = 0;
  int off = 0;
  while (off < n) {
    struct inotify_event *e = (struct inotify_event *)((char *)ev + off);
    if (e->wd == wd) {
      if (e->mask & IN_CREATE && e->len > 0 && strcmp(e->name, "child") == 0)
        saw_create = 1;
      if (e->mask & IN_DELETE && e->len > 0 && strcmp(e->name, "child") == 0)
        saw_delete = 1;
    }
    off += sizeof(struct inotify_event) + e->len;
  }
  TEST_ASSERT_TRUE(saw_create);
  TEST_ASSERT_TRUE(saw_delete);

  inotify_rm_watch(fd, wd);
  close(fd);
  rmdir(TEST_DIR);
}

/* IN-005: empty queue + NONBLOCK read → EAGAIN. */
void test_inotify_nonblock(void) {
  int fd = inotify_init1(IN_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  int wd = inotify_add_watch(fd, TEST_FILE, IN_MODIFY);
  TEST_ASSERT_TRUE(wd >= 1);

  char buf[64];
  TEST_ASSERT_EQUAL_INT(-1, (int)read(fd, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

  inotify_rm_watch(fd, wd);
  close(fd);
}

/* IN-006: inotify fd is pollable; write makes it POLLIN-ready. */
void test_inotify_epoll(void) {
  int fd = inotify_init1(0);
  TEST_ASSERT_TRUE(fd >= 0);
  int wd = inotify_add_watch(fd, TEST_FILE, IN_MODIFY);
  TEST_ASSERT_TRUE(wd >= 1);

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 0)); /* empty → not ready */

  int f = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_TRUE(f >= 0);
  write(f, "x", 1);
  close(f);

  pfd.revents = 0;
  TEST_ASSERT_TRUE(poll(&pfd, 1, 100) > 0);
  TEST_ASSERT_TRUE(pfd.revents & POLLIN);

  inotify_rm_watch(fd, wd);
  close(fd);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  /* Ensure the watched file exists for the file-event tests. */
  int f = open(TEST_FILE, O_WRONLY | O_CREAT, 0644);
  if (f >= 0)
    close(f);

  UNITY_BEGIN();
  RUN_TEST(test_inotify_init);
  RUN_TEST(test_inotify_add_rm);
  RUN_TEST(test_inotify_basic_event);
  RUN_TEST(test_inotify_dir_event);
  RUN_TEST(test_inotify_nonblock);
  RUN_TEST(test_inotify_epoll);
  return UNITY_END();
}
