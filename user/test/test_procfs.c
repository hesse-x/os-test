/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_procfs — procfs 回归测试(procfs.md §5.4),仿 test_sysfs.c。
 * 随 procfs M1-M6 里程碑逐步追加断言;M6 完成时对齐 plan.md Step 43 全集。 */
#include "unity.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static char buf[4096];
static int read_file(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n > 0)
    buf[n] = '\0';
  return n;
}

/* ===== M1: /proc 挂载 + 全局静态节点可读 ===== */
void test_proc_meminfo_has_MemTotal(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/meminfo"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "MemTotal:"));
}

void test_proc_version_nonempty(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/version"));
  TEST_ASSERT_TRUE(buf[0] != '\0');
}

/* ===== M6: cpuinfo(brand string + processor 行) ===== */
void test_proc_cpuinfo_has_processor_and_model(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/cpuinfo"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "processor"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "model name"));
}

/* ===== M2: /proc 列 pid 目录 + self 魔幻链接 ===== */
void test_proc_lists_pids(void) {
  DIR *d = opendir("/proc");
  TEST_ASSERT_NOT_NULL(d);
  int saw_pid = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9') {
      saw_pid = 1;
      break;
    }
  }
  closedir(d);
  TEST_ASSERT_TRUE(saw_pid);
}

void test_proc_self_readlink(void) {
  char link[256];
  int n = readlink("/proc/self", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  /* self 指向 /proc/<自身 pid> */
  TEST_ASSERT_EQUAL_STRING_LEN("/proc/", link, 6);
}

/* ===== M3: per-pid 只读字段 =====
 * 注意:本 OS 中 pid 0/1 是 BSP/AP idle 任务(xtask.proc==NULL、mm==NULL,
 * sched.c:315),procfs 活跃判据 t->proc!=NULL 正确排除之;init 是 pid 2。
 * 故 per-pid 字段读 /proc/self(测试进程自身,有 bsd_proc),而非硬编码 /proc/1
 * (那是 idle,无 proc,会被 -ENOENT 拒绝)。 */
void test_proc_self_status_has_state(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/self/status"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "State:"));
}

void test_proc_self_status_reaches_eof(void) {
  int fd = open("/proc/self/status", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  char chunk[16];
  int n = -1;
  int reads = 0;
  while (reads++ < 64 && (n = read(fd, chunk, sizeof(chunk))) > 0)
    ;
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, n);
  TEST_ASSERT_LESS_THAN(64, reads);
}

void test_proc_self_maps_is_regular(void) {
  struct stat st;
  TEST_ASSERT_EQUAL(0, stat("/proc/self/maps", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
}

void test_proc_self_stat_fields(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/self/stat"));
  int fields = 0;
  int in_paren = 0;
  for (int i = 0; buf[i]; i++) {
    if (buf[i] == '(')
      in_paren = 1;
    else if (buf[i] == ')')
      in_paren = 0;
    else if (!in_paren && buf[i] == ' ')
      fields++;
  }
  TEST_ASSERT_GREATER_THAN(20, fields); /* ~52 字段,宽松断言 */
}

void test_proc_self_cwd_readlink(void) {
  char link[256];
  int n = readlink("/proc/self/cwd", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  TEST_ASSERT_EQUAL('/', link[0]); /* 绝对路径 */
}

/* ===== M5: pinfo 侧表(exe/cmdline) ===== */
void test_proc_self_exe_readlink(void) {
  char link[256];
  int n = readlink("/proc/self/exe", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  /* exe = argv[0](Linux 约定),测试进程由 test_runner execve
   * "/test/test_procfs.elf"。 */
  TEST_ASSERT_EQUAL_STRING_LEN("/test/", link, 6);
  TEST_ASSERT_NOT_NULL(strstr(link, "test_procfs"));
}

void test_proc_self_cmdline_has_argv0(void) {
  /* /proc/self/cmdline:argv \0 拼接。argv[0] 即 exe 路径。 */
  int n = read_file("/proc/self/cmdline");
  TEST_ASSERT_GREATER_THAN(0, n);
  /* buf 以 argv[0] 起(\0 分隔),首段含 "test_procfs"。read_file 在首个 \0 截断,
   * 故断言前缀;cmdline_show 已写完整 \0 拼接,这里只验首段。 */
  TEST_ASSERT_EQUAL_STRING_LEN("/test/", buf, 6);
  TEST_ASSERT_NOT_NULL(strstr(buf, "test_procfs"));
}

/* ===== M4: fd 魔幻链接(/proc/self/fd/N) ===== */
void test_proc_self_fd_lists_entries(void) {
  DIR *d = opendir("/proc/self/fd");
  TEST_ASSERT_NOT_NULL(d);
  int saw_fd = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9') {
      saw_fd = 1;
      break;
    }
  }
  closedir(d);
  TEST_ASSERT_TRUE(saw_fd); /* 至少有 stdin(fd 0) */
}

void test_proc_self_fd0_is_tty(void) {
  char link[256];
  int n = readlink("/proc/self/fd/0", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  /* ttyname 闭环:fd 0 指向 /dev/ptsN(PTY)或 /dev/ttyS0(串口)。 */
  TEST_ASSERT_EQUAL_STRING_LEN("/dev/", link, 5);
}

/* ttyname_r 闭环(musl 上游 ttyname_r.c,procfs.md M4/M6 P0):readlink
 * /proc/self/fd/N
 * + stat/fstat dev+ino 交叉校验通过。ttyname_r 返回 0 即闭环成功(非 ENODEV)。
 */
void test_proc_ttyname_r_closure(void) {
  /* 诊断:逐步排查 isatty/readlink/stat/fstat 哪步给 EFAULT。 */
  char name[64];
  int is_tty = isatty(0);
  int rl = readlink("/proc/self/fd/0", name, sizeof(name) - 1);
  if (rl > 0)
    name[rl] = '\0';
  struct stat st1, st2;
  int s1 = (rl > 0) ? stat(name, &st1) : -999;
  int s2 = fstat(0, &st2);
  printf("[ttyname_r-diag] isatty=%d readlink=%d stat=%d fstat=%d name='%s' "
         "st1.dev=%lu ino=%lu st2.dev=%lu ino=%lu\n",
         is_tty, rl, s1, s2, rl > 0 ? name : "(none)",
         (unsigned long)(rl > 0 ? st1.st_dev : 0),
         (unsigned long)(rl > 0 ? st1.st_ino : 0), (unsigned long)st2.st_dev,
         (unsigned long)st2.st_ino);
  int rc = ttyname_r(0, name, sizeof(name));
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL_STRING_LEN("/dev/", name, 5);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_proc_meminfo_has_MemTotal);
  RUN_TEST(test_proc_version_nonempty);
  RUN_TEST(test_proc_cpuinfo_has_processor_and_model);
  RUN_TEST(test_proc_lists_pids);
  RUN_TEST(test_proc_self_readlink);
  RUN_TEST(test_proc_self_status_has_state);
  RUN_TEST(test_proc_self_status_reaches_eof);
  RUN_TEST(test_proc_self_maps_is_regular);
  RUN_TEST(test_proc_self_stat_fields);
  RUN_TEST(test_proc_self_cwd_readlink);
  RUN_TEST(test_proc_self_fd_lists_entries);
  RUN_TEST(test_proc_self_fd0_is_tty);
  RUN_TEST(test_proc_ttyname_r_closure);
  RUN_TEST(test_proc_self_exe_readlink);
  RUN_TEST(test_proc_self_cmdline_has_argv0);
  return UNITY_END();
}
