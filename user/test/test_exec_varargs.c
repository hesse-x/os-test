/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* exec family varargs wrappers (musl src/process): execl/execle/execlp/execv
 * forward their va_list argv into execv/execve/execvp. These were missing from
 * libc entirely (process.cmake only globbed posix_spawn+execvp), leaving
 * libc.so with an undefined `execl` referenced by wordexp — which broke the
 * libc++ (--cxx) compiler probe link. This test exercises the varargs→argv
 * conversion path that the bug fix restores:
 *   - execl:  absolute-path varargs list, runs hello.elf (exit 0).
 *   - execv:  prebuilt argv[], defaults envp to __environ via execv→execve.
 *   - execlp: PATH-resolved lookup via execvp→__execvpe (setenv PATH=/local).
 * hello.elf is the static hello-world that exits 0, shipped to /local/. */

#include <errno.h>
#include <stdlib.h>
#include <sys/process.h> // fork, execl/execv/execlp/execvp
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* Fork a child that runs `body` (which must exec and not return on success),
 * then reap it and assert a clean exit-0. */
static void run_and_expect_exit0(void (*body)(void)) {
  pid_t child = fork();
  if (child == 0) {
    body();
    _exit(127); /* exec failed */
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 1. execl: varargs list → execv → execve ---- */
static void body_execl(void) {
  execl("/local/hello.elf", "hello", (char *)NULL);
}
void test_execl_runs_hello(void) { run_and_expect_exit0(body_execl); }

/* ---- 2. execv: prebuilt argv[], envp defaults to __environ ---- */
static void body_execv(void) {
  char *argv[] = {"hello", NULL};
  execv("/local/hello.elf", argv);
}
void test_execv_runs_hello(void) { run_and_expect_exit0(body_execv); }

/* ---- 3. execlp: PATH-resolved lookup via execvp→__execvpe ----
 * execlp forwards to execvp, which scans $PATH (default
 * /usr/local/bin:/bin:/usr/bin). hello.elf lives in /local/, so put /local on
 * PATH and invoke by basename. Exercises the __execvpe search loop. */
static void body_execlp(void) {
  setenv("PATH", "/local", 1);
  execlp("hello.elf", "hello", (char *)NULL);
}
void test_execlp_path_lookup_runs_hello(void) {
  run_and_expect_exit0(body_execlp);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_execl_runs_hello);
  RUN_TEST(test_execv_runs_hello);
  RUN_TEST(test_execlp_path_lookup_runs_hello);
  return UNITY_END();
}
