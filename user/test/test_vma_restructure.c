/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_vma_restructure — S10 VMA refactor acceptance (behavior unchanged).
 *
 * The VMA list moved from head-insert (unordered) to a vaddr-sorted list with
 * vma_find/vma_insert_sorted, and mmap_region gained fd/offset/flags. This
 * suite pins the externally visible behavior that must NOT change:
 *
 *   TC1: munmap exact-start match succeeds; munmap of a non-start address
 *        (addr + 0x1000) inside a region returns -EINVAL (size still ignored,
 *        no partial unmap — that is S13).
 *   TC2: two adjacent anonymous mappings (bump-allocated) are both readable;
 *        fork's copy_mmap_regions shallow-copies the new fd/offset/flags and
 *        the child can read both.
 *   TC3: mprotect RW→R syncs mmap_region->prot (fork COW honors it): a child
 *        writing the now-read-only page is killed by SIGSEGV.
 *   TC4: memfd_create + MAP_SHARED mmap is visible to a forked child (shm_obj
 *        shallow-copied + shm_get on fork).
 *   TC5: a second munmap of an already-unmapped address returns -EINVAL
 *        (idempotent failure, no double-free).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* TC1: munmap exact-start match; non-start address rejected. */
void test_munmap_exact_start_and_non_start_rejected(void) {
  void *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_TRUE(p != MAP_FAILED);

  /* Non-start address inside the region must NOT unmap (size ignored, exact
   * match only). */
  int r = munmap((void *)((uintptr_t)p + 0x1000), 0x1000);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  /* Exact start unmaps the whole region. */
  r = munmap(p, 8192);
  TEST_ASSERT_EQUAL_INT(0, r);
}

/* TC2: two mappings + fork shallow-copy. */
void test_two_mappings_fork_child_reads_both(void) {
  char *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  char *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  a[0] = 'A';
  b[0] = 'B';

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    if (a[0] != 'A' || b[0] != 'B')
      _exit(1);
    _exit(0);
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  munmap(a, 4096);
  munmap(b, 4096);
}

/* TC3: mprotect RW→R syncs prot; child write faults. */
void test_mprotect_readonly_fork_child_faults(void) {
  char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'X';

  TEST_ASSERT_EQUAL_INT(0, mprotect(p, 4096, PROT_READ));

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    /* Writing a read-only page should SIGSEGV the child. */
    p[0] = 'Y';
    _exit(0);
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGSEGV, WTERMSIG(status));

  munmap(p, 4096);
}

/* TC4: memfd + MAP_SHARED visible to forked child. */
void test_memfd_shared_mapping_visible_to_child(void) {
  int fd = memfd_create("vma_shm_test", 0);
  TEST_ASSERT_TRUE(fd >= 0);

  /* Size the memfd before mapping + writing (Linux memfd semantics: the file
   * size is set by ftruncate; writing a page beyond EOF would SIGBUS). */
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 4096));

  char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_TRUE(p != MAP_FAILED);
  p[0] = 'S';

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    if (p[0] != 'S')
      _exit(1);
    /* Child writes back; parent should see it (shared mapping). */
    p[1] = 'C';
    _exit(0);
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
  TEST_ASSERT_EQUAL_INT('C', p[1]);

  munmap(p, 4096);
  close(fd);
}

/* TC5: double munmap of same address returns -EINVAL (no double-free). */
void test_double_munmap_returns_einval(void) {
  void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);

  TEST_ASSERT_EQUAL_INT(0, munmap(p, 4096));

  int r = munmap(p, 4096);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_munmap_exact_start_and_non_start_rejected);
  RUN_TEST(test_two_mappings_fork_child_reads_both);
  RUN_TEST(test_mprotect_readonly_fork_child_faults);
  RUN_TEST(test_memfd_shared_mapping_visible_to_child);
  RUN_TEST(test_double_munmap_returns_einval);
  return UNITY_END();
}
