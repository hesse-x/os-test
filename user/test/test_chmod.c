/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_chmod.c — 验证 chmod/fchmod/fchmodat/chown/fchown/fchownat(2) 的真实
 * 实现(阶段1)与 capable() 收口(阶段0)。对齐 test_link_utimensat.c 风格:Unity
 * freestanding,FAT32(/ 前缀)+ tmpfs(/run 前缀)双夹具。
 *
 * 落盘仅内存(与 utimensat Q5 一致)。setuid 位清除规则(apply_chmod/apply_chown):
 *   - 非特权(CAP_FSETID 缺)chmod/chown 清 S_ISUID/S_ISGID —— 防伪造 setuid-root
 * 提权
 *   - root(有 CAP_FSETID)chmod/chown 保留 S_ISUID —— sudo 使能前提(位需能设上)
 * open(O_CREAT) 仅落地 0777 位(& 0777),S_ISUID 须经 chmod 设上,故 setuid 用例均
 * 先 chmod 04755 设位再操作。非 root 用例经 fork 子 setuid(1000)+_exit 码 +
 * 父 waitpid 断退出码(照 test_kill_perm.c:106)。
 *
 * FAT32 vs tmpfs 差异(同 test_link_utimensat):fat32 inode 无 fs
 * 内部强引用,chmod/ chown 释放 inode 后可被回收,再 stat 经 lookup 建新 inode →
 * mode/uid/gid 回 inode_create 默认值(0100644 / uid=0);故 path 设置+path
 * 验证(set→stat 都经 path 重解)的用例用 tmpfs(子项持 inode
 * 引用不回收,精确往返)。fd 路径用例(open 持 inode 引用,fchmod/fstat 同 fd)用
 * FAT32 作对照,验证 fd 与 path 两路殊途同归。 */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FAT "/chmod_fat"
#define TFS "/run/chmod_tfs"

void setUp(void) {
  /* 夹具父目录(幂等:create 仅建末段,父目录须先在)。 */
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

/* cleanup:删除各测试用的具体文件,保留夹具父目录。 */
static void cleanup(void) {
  unlink(FAT "/f");
  unlink(TFS "/f");
}

/* 创建一个空文件(0644)用于 chmod/chown 操作。返 fd(>=0)或 -1。 */
static int make_file(const char *path) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0)
    close(fd);
  return fd;
}

/* ---- 1. root chmod → stat 断 mode(验证真写 inode 非桩) ----
 * tmpfs:子项持 inode 引用,chmod 释放后 stat 重解命中缓存 → mode 精确往返。 */
void test_chmod_root_basic(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
}

/* ---- 2. chmod 保留文件类型位:S_ISREG/S_ISDIR 仍真 ---- */
void test_chmod_preserves_type(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  /* 目录类型位保留:tmpfs 夹具父目录 chmod 后仍 S_ISDIR。 */
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS, 0700));
  TEST_ASSERT_EQUAL_INT(0, stat(TFS, &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

/* ---- 3. owner(非 root)chmod 自己的 04755 文件 → S_ISUID 被清 ----
 * 非 root owner 有 chmod 权(euid==ip->uid)但无 CAP_FSETID,apply_chmod 清
 * setuid 位。设位须 root chmod 04755(有 CAP_FSETID 保留),owner 先 chown 给
 * 1000。 */
void test_chmod_owner_clears_setuid(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  /* root chown 给 1000(本轮 chown root-only,见 test_chown_root_basic)。 */
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  /* root chmod 04755 设 S_ISUID 位(CAP_FSETID 保留)。 */
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 04755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID);

  pid_t child = fork();
  if (child == 0) {
    setuid(1000); /* drop root:uid==euid==1000,owner of /run/chmod_tfs/f */
    /* owner chmod 0644(非特权):apply_chmod 清 S_ISUID。 */
    int r = chmod(TFS "/f", 0644);
    if (r != 0)
      _exit(2);
    struct stat s2;
    if (stat(TFS "/f", &s2) != 0)
      _exit(3);
    _exit((s2.st_mode & S_ISUID) ? 1 : 0); /* 0 = cleared as expected */
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 4. 非 root chmod root 文件得 EPERM ----
 * root 拥有的 tmpfs 文件:非 root euid != ip->uid(0) 且无 CAP_FOWNER → EPERM
 * (对齐 Linux/POSIX chmod(2):非 owner 无 CAP_FOWNER 返 EPERM,非 EACCES)。 */
void test_chmod_non_root_eperm(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  pid_t child = fork();
  if (child == 0) {
    setuid(1000);
    int r = chmod(TFS "/f", 0600);
    _exit(r == -1 && errno == EPERM ? 0 : 1); /* 0 = got EPERM as expected */
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 5. root chmod 04755 → S_ISUID 保留(CAP_FSETID 豁免) ---- */
void test_chmod_root_setuid_kept(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 04755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID);
}

/* ---- 6. fchmod(fd) → fstat(fd) 断 0600 ----
 * FAT32 对照:open 持 inode 引用,fchmod 与 fstat 同 fd 共享 inode(不回收)。 */
void test_fchmod_fd_path(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(FAT "/f"));
  int fd = open(FAT "/f", O_WRONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(0, fchmod(fd, 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
  close(fd);
}

/* ---- 7. fchmodat(fd,"",0600,AT_EMPTY_PATH) → fstat(fd) 断 0600 ---- */
void test_fchmodat_at_empty_path(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(FAT "/f"));
  int fd = open(FAT "/f", O_WRONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(0, fchmodat(fd, "", 0600, AT_EMPTY_PATH));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
  close(fd);
}

/* ---- 8. fchmodat dirfd 相对路径解析 ----
 * tmpfs:fchmodat(dirfd,"f") 释放后 stat(path) 重解命中缓存(子项持引用)。 */
void test_fchmodat_dirfd_relative(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  int dfd = open(TFS, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(0, fchmodat(dfd, "f", 0600, 0));
  close(dfd);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
}

/* ---- 9. root chown(1000,1000) → stat 断 uid/gid ---- */
void test_chown_root_basic(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_gid);
}

/* ---- 10. chown(-1,-1) 不改 uid/gid(防回归) ---- */
void test_chown_minus_one_unchanged(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  /* (uid_t)-1/(gid_t)-1 = 该字段不变(POSIX)。 */
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", (uid_t)-1, (gid_t)-1));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_gid);
}

/* ---- 11. 非 root chown 得 EPERM(本轮简化为 root-only) ----
 * EPERM 来自 CAP_CHOWN 门控(任何 path/fs 前),故 FAT32 亦可。 */
void test_chown_non_root_eperm(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(FAT "/f"));
  pid_t child = fork();
  if (child == 0) {
    setuid(1000);
    int r = chown(FAT "/f", 1000, 1000);
    _exit(r == -1 && errno == EPERM ? 0 : 1); /* 0 = got EPERM as expected */
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 12. root chown 保留 S_ISUID(有 CAP_FSETID) ----
 * 本轮 chown root-only,非 root chown 得 EPERM 不会执行清位;root chown 因
 * CAP_FSETID 豁免保留 S_ISUID(对齐 apply_chown:!capable(CAP_FSETID) 才清)。
 * 非 root chown 清 setuid 位需"属主改 group 到自己所在 group"复杂规则放开
 * 后才有路径,记入 todo。 */
void test_chown_root_keeps_setuid(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 04755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID);
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID); /* root chown 保留 S_ISUID */
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_chmod_root_basic);
  RUN_TEST(test_chmod_preserves_type);
  RUN_TEST(test_chmod_owner_clears_setuid);
  RUN_TEST(test_chmod_non_root_eperm);
  RUN_TEST(test_chmod_root_setuid_kept);
  RUN_TEST(test_fchmod_fd_path);
  RUN_TEST(test_fchmodat_at_empty_path);
  RUN_TEST(test_fchmodat_dirfd_relative);
  RUN_TEST(test_chown_root_basic);
  RUN_TEST(test_chown_minus_one_unchanged);
  RUN_TEST(test_chown_non_root_eperm);
  RUN_TEST(test_chown_root_keeps_setuid);
  return UNITY_END();
}
