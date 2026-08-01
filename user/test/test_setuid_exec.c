/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_setuid_exec.c — verify execve S_ISUID/S_ISGID credential switching
// (proc.c sys_execve) + setuid ladder gate self-consistency (stage 0:
// sys_setuid/setgid via capable(CAP_SETUID/GID)).
//
// Mechanism: the helper ELF (/test/setuid_helper.elf, packed on FAT32) is
// first copied to /run (tmpfs), where chmod 04755 + chown(1000,1000) are
// done on the tmpfs child — the child holds an inode reference, so the path
// chmod persists (FAT32 would discard it; see test_chmod.c:18-24). A forked
// child setuid(2000) drops root, then execve(/run/setuid_helper.elf,"euid")
// -> exit code should reflect euid switched to 1000. execve being able to
// read tmpfs is via this task's vfs_read_kernel tmpfs dispatch (vfs.c).
//
// Exit-code convention: uid/gid > 255 don't fit in WEXITSTATUS (8-bit), so
// the helper reports (val & 0x7f) and the test asserts (expected & 0x7f).
// 1000&0x7f=104, 2000&0x7f=80 — distinguishable enough.
//
// Key decisions (settled with the user):
//   - Choice1=A: setuid exec switches only euid/suid (egid/sgid); real
//     uid/gid stay as the caller's.
//   - Choice2=B: NOSUID case deferred — no NOSUID mount built in the QEMU
//     test; left to todo + manual testing.
//   - Choice3=B: credentials committed past point-of-no-return; auxv uses
//     the precomputed new values (already implemented in-kernel).
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h> // fork, setuid, setgid, execve
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

#define SRC_HELPER "/test/setuid_helper.elf" // FAT32-packed helper source
#define HELPER "/run/su_exec_helper.elf"     // tmpfs copy (chmod persists)
#define TARGET_UID 1000
#define TARGET_GID 1000
#define CALLER_UID 2000

void setUp(void) {}
void tearDown(void) {}

// copy_helper: copy the FAT32 helper source to tmpfs /run (so chmod persists).
// The tmpfs file is created via open(O_CREAT)+read/write; ELF ~14KB <
// TMPFS_FILE_CAP 64KB.
static void copy_helper(void) {
  int in = open(SRC_HELPER, O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, in);
  int out = open(HELPER, O_CREAT | O_WRONLY | O_TRUNC, 0755);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, out);
  char buf[512];
  ssize_t n;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    ssize_t w = write(out, buf, (size_t)n);
    TEST_ASSERT_EQUAL_INT(n, w);
  }
  TEST_ASSERT_EQUAL_INT(0, n);
  close(in);
  close(out);
}

// run_helper_as: fork a child that drops root then execve's the helper;
// returns the helper's exit code (WEXITSTATUS). mode = "euid"/"egid"/
// "uid"/"gid". execve failure is relayed as 127.
static int run_helper_as(const char *mode) {
  pid_t child = fork();
  if (child == 0) {
    setgid(CALLER_UID);
    setuid(CALLER_UID); // drop root: uid=euid=suid=2000
    char *const argv[] = {(char *)"setuid_helper", (char *)mode, NULL};
    char *const envp[] = {NULL};
    execve(HELPER, argv, envp);
    _exit(127); // execve failed
  }
  TEST_ASSERT_TRUE(child > 0);
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  return WEXITSTATUS(status);
}

// ---- 1. S_ISUID exec: euid switches to inode owner (1000) ----
// On tmpfs chmod 04755 (sets S_ISUID; CAP_FSETID kept) + chown(1000,1000).
// Child setuid(2000) then execve -> euid should switch to 1000. Exit code =
// 1000 & 0x7f = 104.
void test_setuid_exec_euid_switch(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 04755));
  TEST_ASSERT_EQUAL_INT(TARGET_UID & 0x7f, run_helper_as("euid"));
}

// ---- 2. S_ISGID exec: egid switches to inode group (1000) ----
// Only S_ISGID set (02755). egid switches to 1000, euid stays as caller 2000
// (contrast with run 5).
void test_setgid_exec_egid_switch(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 02755));
  TEST_ASSERT_EQUAL_INT(TARGET_GID & 0x7f, run_helper_as("egid"));
}

// ---- 3. real uid stays as caller (2000), not switched by setuid exec
// (Choice1=A) ----
void test_setuid_exec_real_uid_kept(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 04755));
  TEST_ASSERT_EQUAL_INT(CALLER_UID & 0x7f, run_helper_as("uid"));
}

// ---- 4. S_ISGID does not affect real gid: real gid stays as caller (2000)
// ----
void test_setgid_exec_real_gid_kept(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 02755));
  TEST_ASSERT_EQUAL_INT(CALLER_UID & 0x7f, run_helper_as("gid"));
}

// ---- 5. No setuid bit: euid not switched, stays as caller (2000) ----
// chmod 0755 (clears S_ISUID/S_ISGID) -> execve does not switch euid, should
// get 2000&0x7f=80. Control group, rules out a "helper exit code is always
// some fixed value" false pass.
void test_no_setuid_bit_no_switch(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 0755));
  TEST_ASSERT_EQUAL_INT(CALLER_UID & 0x7f, run_helper_as("euid"));
}

// ---- 6. setuid ladder gate self-consistency regression: after dropping root,
// setuid(0) still EPERM ----
// Verifies stage 0 changing sys_setuid's gate from bare euid==0 to
// capable(CAP_SETUID) leaves behavior unchanged (today equivalent to
// euid==0). After dropping to 2000, setuid(0) must EPERM (suid no longer
// holds 0).
void test_setuid_ladder_drop_then_eperm(void) {
  pid_t child = fork();
  if (child == 0) {
    if (setuid(CALLER_UID) != 0)
      _exit(1);
    if (setuid(0) != -1 || errno != EPERM)
      _exit(2);
    _exit(0);
  }
  TEST_ASSERT_TRUE(child > 0);
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_setuid_exec_euid_switch);
  RUN_TEST(test_setgid_exec_egid_switch);
  RUN_TEST(test_setuid_exec_real_uid_kept);
  RUN_TEST(test_setgid_exec_real_gid_kept);
  RUN_TEST(test_no_setuid_bit_no_switch);
  RUN_TEST(test_setuid_ladder_drop_then_eperm);
  return UNITY_END();
}
