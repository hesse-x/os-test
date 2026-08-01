/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>

#include <sys/wait.h>

extern char **environ;

void setUp(void) {}
void tearDown(void) {}

static int spawn_shell(const char *command) {
  pid_t pid = -1;
  char *argv[] = {(char *)"sh", (char *)"-c", (char *)command, NULL};
  int rc = posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ);
  TEST_ASSERT_EQUAL_INT(0, rc);
  int status = 0;
  TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
  return status;
}

void test_spawn_exec_and_wait_status(void) {
  int status = spawn_shell("exit 37");
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(37, WEXITSTATUS(status));
}

void test_spawn_reports_exec_enoent(void) {
  pid_t pid = -1;
  char *argv[] = {(char *)"missing", NULL};
  TEST_ASSERT_EQUAL_INT(ENOENT, posix_spawn(&pid, "/definitely/missing", NULL,
                                            NULL, argv, environ));
}

void test_spawn_rejects_unsupported_scheduling_flags(void) {
  posix_spawnattr_t attr;
  short flags = -1;

  TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_init(&attr));
  TEST_ASSERT_EQUAL_INT(
      ENOTSUP, posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDPARAM));
  TEST_ASSERT_EQUAL_INT(
      ENOTSUP, posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDULER));
  TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_getflags(&attr, &flags));
  TEST_ASSERT_EQUAL_INT(0, flags);
  TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_destroy(&attr));
}

void test_spawn_file_actions_and_shell_subset(void) {
  const char *path = "/run/spawn-output";
  posix_spawn_file_actions_t actions;
  TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&actions));
  TEST_ASSERT_EQUAL_INT(
      0, posix_spawn_file_actions_addopen(&actions, 1, path,
                                          O_WRONLY | O_CREAT | O_TRUNC, 0666));
  pid_t pid = -1;
  char *argv[] = {(char *)"sh", (char *)"-c",
                  (char *)"X=hello echo \"$X world\"; false || echo ok", NULL};
  TEST_ASSERT_EQUAL_INT(
      0, posix_spawn(&pid, "/bin/sh", &actions, NULL, argv, environ));
  TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&actions));
  int status = 0;
  TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
  TEST_ASSERT_EQUAL_INT(0, status);
  FILE *f = fopen(path, "r");
  TEST_ASSERT_NOT_NULL(f);
  char buf[64] = {0};
  TEST_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
  TEST_ASSERT_EQUAL_STRING(" world\n", buf);
  TEST_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
  TEST_ASSERT_EQUAL_STRING("ok\n", buf);
  fclose(f);
  unlink(path);
}

void test_popen_read_and_raw_status(void) {
  FILE *f = popen("echo hello; exit 7", "r");
  TEST_ASSERT_NOT_NULL(f);
  char buf[32] = {0};
  TEST_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
  TEST_ASSERT_EQUAL_STRING("hello\n", buf);
  int status = pclose(f);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(7, WEXITSTATUS(status));
}

void test_popen_mode_and_cloexec(void) {
  errno = 0;
  TEST_ASSERT_NULL(popen("true", "x"));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  FILE *f = popen("true", "re");
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fileno(f), F_GETFD));
  TEST_ASSERT_EQUAL_INT(0, pclose(f));
}

void test_spawn_stress_no_lost_wakeup(void) {
  for (int i = 0; i < 50; i++)
    TEST_ASSERT_EQUAL_INT(0, spawn_shell("true"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_spawn_exec_and_wait_status);
  RUN_TEST(test_spawn_reports_exec_enoent);
  RUN_TEST(test_spawn_rejects_unsupported_scheduling_flags);
  RUN_TEST(test_spawn_file_actions_and_shell_subset);
  RUN_TEST(test_popen_read_and_raw_status);
  RUN_TEST(test_popen_mode_and_cloexec);
  RUN_TEST(test_spawn_stress_no_lost_wakeup);
  return UNITY_END();
}
