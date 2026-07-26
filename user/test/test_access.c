/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_access.c — 验证 §3.2 access(2)/faccessat(2)(内核 inode_permission 按
 * euid 判定,Q4)。对齐 test_rename.c/test_stat_real.c 风格:Unity freestanding,
 * FAT32(/ 前缀)+ tmpfs(/run 前缀)双夹具。
 *
 * 本 OS 默认 euid=0=root,inode_permission 对 root 放行(CAP_DAC_OVERRIDE 等价),
 * 故 R_OK/W_OK/X_OK 对 root 恒返 0;主要验证存在性(F_OK=0)/不存在(ENOENT)与
 * faccessat 的 AT_FDCWD/dirfd 路径。非 root 权限位判定见 test_setuid_saved。 */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAT "/access_fat"
#define TFS "/run/access_tfs"

void setUp(void) {
  /* 夹具父目录(幂等:create 仅建末段,父目录须先在)。 */
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

/* cleanup:删除各测试用的具体文件/链接,保留夹具父目录。 */
static void cleanup(void) {
  unlink(FAT "/exists");
  unlink(FAT "/ro");
  unlink(TFS "/exists");
  unlink(TFS "/ro");
}

/* access 对存在的文件 F_OK(存在性)=0;对不存在 ENOENT。 */
void test_access_existing_fok(void) {
  cleanup();
  int fd = open(FAT "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, access(FAT "/exists", F_OK));
  TEST_ASSERT_EQUAL_INT(0, access(FAT "/exists", R_OK | W_OK | X_OK));

  TEST_ASSERT_EQUAL_INT(-1, access(FAT "/nope", F_OK));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* access 对只读文件(0644):root 下 R_OK=0,W_OK/X_OK 亦放行(root CAP 覆盖)。
 * 验证 mode 参数透传到内核(非旧 stat 兜底忽略 mode)。 */
void test_access_ro_mode_checked(void) {
  cleanup();
  int fd = open(FAT "/ro", O_CREAT | O_WRONLY | O_TRUNC, 0444);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, access(FAT "/ro", R_OK));
  /* root 对 W_OK/X_OK 放行(CAP_DAC_OVERRIDE);断言 syscall 真按 mode 调用
   * (旧实现忽略 mode,R_OK 与 W_OK 不可区分)。 */
  TEST_ASSERT_EQUAL_INT(0, access(FAT "/ro", W_OK));
  TEST_ASSERT_EQUAL_INT(0, access(FAT "/ro", X_OK));
}

/* tmpfs(/run)同样路径:确认 access 对 tmpfs inode 走同一 inode_permission。 */
void test_access_tmpfs(void) {
  cleanup();
  int fd = open(TFS "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, access(TFS "/exists", F_OK));
  TEST_ASSERT_EQUAL_INT(-1, access(TFS "/nope", F_OK));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* faccessat(AT_FDCWD) ≡ access;AT_FDCWD 是无 per-process CWD 内核的根等价。 */
void test_faccessat_atfdcwd(void) {
  cleanup();
  int fd = open(FAT "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, faccessat(AT_FDCWD, FAT "/exists", F_OK, 0));
  TEST_ASSERT_EQUAL_INT(0, faccessat(AT_FDCWD, FAT "/exists", R_OK | W_OK, 0));
  TEST_ASSERT_EQUAL_INT(-1, faccessat(AT_FDCWD, FAT "/nope", F_OK, 0));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* faccessat 对打开的目录 dirfd 自身(AT_EMPTY_PATH):dirfd 指向夹具父目录,
 * 对其 F_OK/R_OK/X_OK 应为 0。 */
void test_faccessat_dirfd_empty_path(void) {
  cleanup();
  int dfd = open(FAT, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(
      0, faccessat(dfd, "", F_OK | R_OK | W_OK | X_OK, AT_EMPTY_PATH));
  close(dfd);
}

/* 非法 mode 位(如 0x100)→ EINVAL;非法 flags 位 → EINVAL(Q6 严格校验)。 */
void test_faccessat_invalid_args(void) {
  cleanup();
  int fd = open(FAT "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(-1, faccessat(AT_FDCWD, FAT "/exists", 0x100, 0));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  TEST_ASSERT_EQUAL_INT(-1, faccessat(AT_FDCWD, FAT "/exists", F_OK, 0x400000));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_access_existing_fok);
  RUN_TEST(test_access_ro_mode_checked);
  RUN_TEST(test_access_tmpfs);
  RUN_TEST(test_faccessat_atfdcwd);
  RUN_TEST(test_faccessat_dirfd_empty_path);
  RUN_TEST(test_faccessat_invalid_args);
  return UNITY_END();
}
