/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_procfs — procfs regression tests (procfs.md §5.4), mirroring
// test_sysfs.c. Assertions grow with the M1-M6 milestones; M6 aligns with
// plan.md Step 43.
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

// ===== M1: /proc mounted + global static nodes readable =====
void test_proc_meminfo_has_MemTotal(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/meminfo"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "MemTotal:"));
}

void test_proc_version_nonempty(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/version"));
  TEST_ASSERT_TRUE(buf[0] != '\0');
}

// ===== M6: cpuinfo (brand string + processor line) =====
void test_proc_cpuinfo_has_processor_and_model(void) {
  TEST_ASSERT_GREATER_THAN(0, read_file("/proc/cpuinfo"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "processor"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "model name"));
}

// ===== M2: /proc lists pid dirs + self magic link =====
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
  // self points to /proc/<own pid>
  TEST_ASSERT_EQUAL_STRING_LEN("/proc/", link, 6);
}

// ===== M3: per-pid read-only fields =====
// Note: pid 0/1 are BSP/AP idle tasks (xtask.proc==NULL, mm==NULL,
// sched.c:315); the procfs liveness check t->proc!=NULL correctly excludes
// them; init is pid 2. So per-pid fields are read via /proc/self (this test
// process, which has a bsd_proc) rather than hardcoded /proc/1 (that is the
// idle task, no proc, would be rejected with -ENOENT).
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
  TEST_ASSERT_GREATER_THAN(20, fields); // ~52 fields; loose assertion
}

void test_proc_self_cwd_readlink(void) {
  char link[256];
  int n = readlink("/proc/self/cwd", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  TEST_ASSERT_EQUAL('/', link[0]); // absolute path
}

// ===== M5: pinfo side table (exe/cmdline) =====
void test_proc_self_exe_readlink(void) {
  char link[256];
  int n = readlink("/proc/self/exe", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  // exe = argv[0] (Linux convention); the test process was execve'd by
  // test_runner as "/test/test_procfs.elf".
  TEST_ASSERT_EQUAL_STRING_LEN("/test/", link, 6);
  TEST_ASSERT_NOT_NULL(strstr(link, "test_procfs"));
}

void test_proc_self_cmdline_has_argv0(void) {
  // /proc/self/cmdline: argv joined with \0. argv[0] is the exe path.
  int n = read_file("/proc/self/cmdline");
  TEST_ASSERT_GREATER_THAN(0, n);
  // buf starts with argv[0] (\0-separated); read_file truncates at the first
  // \0, so assert the prefix; cmdline_show writes the full \0-joined form —
  // here we only verify the first segment.
  TEST_ASSERT_EQUAL_STRING_LEN("/test/", buf, 6);
  TEST_ASSERT_NOT_NULL(strstr(buf, "test_procfs"));
}

// ===== M4: fd magic links (/proc/self/fd/N) =====
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
  TEST_ASSERT_TRUE(saw_fd); // at least stdin (fd 0)
}

void test_proc_self_fd0_is_tty(void) {
  char link[256];
  int n = readlink("/proc/self/fd/0", link, sizeof(link) - 1);
  TEST_ASSERT_GREATER_THAN(0, n);
  link[n] = '\0';
  // ttyname closure: fd 0 points to /dev/ptsN (PTY) or /dev/ttyS0 (serial).
  TEST_ASSERT_EQUAL_STRING_LEN("/dev/", link, 5);
}

// ttyname_r closure (musl upstream ttyname_r.c, procfs.md M4/M6 P0):
// readlink /proc/self/fd/N + stat/fstat dev+ino cross-check pass. ttyname_r
// returning 0 means closure succeeded (not ENODEV).
void test_proc_ttyname_r_closure(void) {
  // Diagnostics: step-wise check which of isatty/readlink/stat/fstat gives
  // EFAULT.
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
