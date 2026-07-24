/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* S19 §7 — execve uses the VFS-generic kernel read (vfs_read_kernel) instead
 * of hardcoding fat32_read. Verification test (degraded per the test-design
 * D4/D5 corrections):
 *   - fat32 exec regression: execve("/local/hello.elf") runs the static hello
 *     binary cleanly — proves the fat32 path through vfs_read_kernel did not
 *     regress.
 *   - non-regular exec rejection: execve on a character device (/dev/urandom,
 *     an FD_DEV) is rejected at the fd-type check (proc.c) with -EIO, not
 *     -ENOEXEC — the device never reaches vfs_read_kernel.
 *
 * tmpfs/memfd exec is NOT reachable today (memfd is FD_SHM, rejected at the
 * same fd-type check), so it is not exercised here — recorded in todo.md. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h> // fork, execve
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- 1. fat32 exec regression: hello.elf runs cleanly ---- */
void test_execve_fat32_regression(void) {
  pid_t child = fork();
  if (child == 0) {
    execve("/local/hello.elf", NULL, NULL);
    _exit(127); /* execve failed */
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  /* hello.elf is the static hello-world; it exits 0. */
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 2. execve on a char device is rejected with -EIO (fd-type check) ----
 * /dev/urandom is a real registered char device (FD_DEV). execve opens it,
 * then the fd-type guard (must be FD_REGULAR) rejects it with -EIO before any
 * inode read. The child relays the errno back through its exit status. */
void test_execve_device_rejected(void) {
  pid_t child = fork();
  if (child == 0) {
    int r = execve("/dev/urandom", NULL, NULL);
    /* execve returns -1 + errno on failure; relay errno to the parent. The
     * expected errno is EIO (5). Cap at 0x7f so WEXITSTATUS is unambiguous. */
    int e = (r == -1) ? errno : 0xff;
    _exit(e & 0x7f);
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(EIO, WEXITSTATUS(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_execve_fat32_regression);
  RUN_TEST(test_execve_device_rejected);
  return UNITY_END();
}
