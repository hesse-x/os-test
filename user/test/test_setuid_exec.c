/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_setuid_exec.c — 验证 execve 的 S_ISUID/S_ISGID 凭证切换(proc.c
 * sys_execve)
 * + setuid 阶梯 gate 自洽化(阶段0:sys_setuid/setgid 经
 * capable(CAP_SETUID/GID))。
 *
 * 机制：helper ELF(/test/setuid_helper.elf,FAT32 打包)先复制到 /run(tmpfs)，
 * 在 tmpfs 上 chmod 04755 + chown(1000,1000)——tmpfs 子项持 inode 引用，path
 * chmod 持久不回收（FAT32 会回收丢位，见 test_chmod.c:18-24）。fork 子
 * setuid(2000) drop root，子 execve(/run/setuid_helper.elf,"euid") →
 * 退出码应反映 euid 切到 1000。 execve 能读 tmpfs 是本任务配套扩的
 * vfs_read_kernel tmpfs 分发（vfs.c）。
 *
 * 退出码约定：uid/gid > 255 装不进 WEXITSTATUS(8-bit)，故 helper 报 (val &
 * 0x7f)， 测试断言 (expected & 0x7f)。1000&0x7f=104、2000&0x7f=80，区分足够。
 *
 * 关键决策（已与用户敲定）：
 *   - 抉择1=A：setuid exec 只切 euid/suid(egid/sgid)，real uid/gid 保持调用者。
 *   - 抉择2=B：NOSUID 用例降级——不在 QEMU 测试里建 NOSUID 挂载；留 todo +
 * 手测。
 *   - 抉择3=B：凭证在 point-of-no-return 后提交，auxv
 * 用预计算新值(内核已实现)。 */
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

#define SRC_HELPER "/test/setuid_helper.elf" /* FAT32 打包的 helper 源 */
#define HELPER "/run/su_exec_helper.elf"     /* tmpfs 副本(chmod 持久) */
#define TARGET_UID 1000
#define TARGET_GID 1000
#define CALLER_UID 2000

void setUp(void) {}
void tearDown(void) {}

/* copy_helper:把 FAT32 上的 helper 源复制到 tmpfs /run（chmod 持久的前提）。
 * tmpfs 文件经 open(O_CREAT)+read/write 建，ELF ~14KB < TMPFS_FILE_CAP 64KB。
 */
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

/* run_helper_as:fork 子 drop root 后 execve helper，返 helper
 * 退出码(WEXITSTATUS)。 mode = "euid"/"egid"/"uid"/"gid"。execve 失败 relay 成
 * 127。 */
static int run_helper_as(const char *mode) {
  pid_t child = fork();
  if (child == 0) {
    setgid(CALLER_UID);
    setuid(CALLER_UID); /* drop root:uid=euid=suid=2000 */
    char *const argv[] = {(char *)"setuid_helper", (char *)mode, NULL};
    char *const envp[] = {NULL};
    execve(HELPER, argv, envp);
    _exit(127); /* execve failed */
  }
  TEST_ASSERT_TRUE(child > 0);
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  return WEXITSTATUS(status);
}

/* ---- 1. S_ISUID exec：euid 切到 inode owner(1000) ----
 * tmpfs 上 chmod 04755(设 S_ISUID,CAP_FSETID 保留)+chown(1000,1000)。子
 * setuid(2000) 后 execve → euid 应切到 1000。退出码 = 1000 & 0x7f = 104。 */
void test_setuid_exec_euid_switch(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 04755));
  TEST_ASSERT_EQUAL_INT(TARGET_UID & 0x7f, run_helper_as("euid"));
}

/* ---- 2. S_ISGID exec：egid 切到 inode group(1000) ----
 * 只设 S_ISGID(02755)。egid 切到 1000，euid 保持调用者 2000(对照 run 5)。 */
void test_setgid_exec_egid_switch(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 02755));
  TEST_ASSERT_EQUAL_INT(TARGET_GID & 0x7f, run_helper_as("egid"));
}

/* ---- 3. real uid 保持调用者(2000)，不被 setuid exec 切走(抉择1=A) ---- */
void test_setuid_exec_real_uid_kept(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 04755));
  TEST_ASSERT_EQUAL_INT(CALLER_UID & 0x7f, run_helper_as("uid"));
}

/* ---- 4. S_ISGID 不影响 real gid：real gid 保持调用者(2000) ---- */
void test_setgid_exec_real_gid_kept(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 02755));
  TEST_ASSERT_EQUAL_INT(CALLER_UID & 0x7f, run_helper_as("gid"));
}

/* ---- 5. 无 setuid 位：euid 不切，保持调用者(2000) ----
 * chmod 0755(清 S_ISUID/S_ISGID)→ execve 不切 euid，应得 2000&0x7f=80。对照组，
 * 排除"helper 退出码恒为某值"的伪通过。 */
void test_no_setuid_bit_no_switch(void) {
  copy_helper();
  TEST_ASSERT_EQUAL_INT(0, chown(HELPER, TARGET_UID, TARGET_GID));
  TEST_ASSERT_EQUAL_INT(0, chmod(HELPER, 0755));
  TEST_ASSERT_EQUAL_INT(CALLER_UID & 0x7f, run_helper_as("euid"));
}

/* ---- 6. setuid 阶梯 gate 自洽化回归：drop root 后 setuid(0) 仍 EPERM ----
 * 验证阶段0把 sys_setuid 的 gate 从裸 euid==0 改 capable(CAP_SETUID) 后行为不变
 * (今天等价 euid==0)。drop 到 2000 后 setuid(0) 须 EPERM(suid 不再持 0)。 */
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
