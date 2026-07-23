/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_mmap_file_private.c — S12: MAP_PRIVATE+fd file-backed mmap + COW.
 *
 * Validates the demand-fault page-in path (file_fault_handler) for regular
 * files on the FAT32 root: mmap(MAP_PRIVATE, fd) reads the file's contents
 * (not zero pages), writes are COW-private and do not mutate the backing file,
 * multi-page and offset mappings fault independently, the mapping survives
 * close(fd) via the region's inode reference, memfd MAP_PRIVATE COWs from the
 * shm page list, and fork gives parent/child independent private copies.
 *
 * Scratch files live in /local (FAT32 root, writable at runtime — same idiom as
 * test_fcntl). Unity freestanding: setUp/tearDown are empty. */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

#define PAGE 4096

/* Create /local/<name> with the given bytes and return an O_RDONLY fd, or -1.
 * The file is written via the FAT32 FD_REGULAR write path (fat32_write through
 * the page cache), so page_cache_fill will read the same bytes back on fault.
 */
static int make_file(const char *name, const void *buf, size_t len) {
  char path[64];
  strcpy(path, "/local/");
  strcat(path, name);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t w = write(fd, buf, len);
  close(fd);
  if (w != (ssize_t)len)
    return -1;
  fd = open(path, O_RDONLY);
  return fd;
}

static void unlink_file(const char *name) {
  char path[64];
  strcpy(path, "/local/");
  strcat(path, name);
  unlink(path);
}

/* TC1: basic read — mmap'd memory reflects file contents (the S12 bug this
 * fixes: MAP_PRIVATE+fd previously returned a zero page). */
void test_mmap_file_private_basic_read(void) {
  int fd = make_file("mfp_basic", "hello", 5);
  TEST_ASSERT_TRUE(fd >= 0);

  char *p = (char *)mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
  TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);
  TEST_ASSERT_EQUAL_INT(0, memcmp(p, "hello", 5));
  munmap(p, PAGE);
  close(fd);
  unlink_file("mfp_basic");
}

/* TC2: write COW does not mutate the backing file — write in-memory, munmap,
 * remap, and confirm the file still holds the original bytes. */
void test_mmap_file_private_write_cow(void) {
  int fd = make_file("mfp_cow", "hello", 5);
  TEST_ASSERT_TRUE(fd >= 0);

  char *p =
      (char *)mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);
  p[0] = 'X';
  TEST_ASSERT_EQUAL_INT('X', p[0]); // write took effect in the private copy
  munmap(p, PAGE);
  close(fd);

  fd = open("/local/mfp_cow", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  char buf[8] = {0};
  ssize_t r = read(fd, buf, sizeof(buf));
  close(fd);
  TEST_ASSERT_EQUAL_INT(5, (int)r);
  TEST_ASSERT_EQUAL_STRING("hello", buf); // file unchanged
  unlink_file("mfp_cow");
}

/* TC3: multi-page read — an 8KB file faults in two pages independently. */
void test_mmap_file_private_multi_page(void) {
  char data[8192];
  memcpy(data, "page0", 5);
  memcpy(data + PAGE, "page1", 5);

  int fd = make_file("mfp_multi", data, sizeof(data));
  TEST_ASSERT_TRUE(fd >= 0);

  char *p = (char *)mmap(NULL, 2 * PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
  TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);
  TEST_ASSERT_EQUAL_INT(0, memcmp(p, "page0", 5));
  TEST_ASSERT_EQUAL_INT(0, memcmp(p + PAGE, "page1", 5));
  munmap(p, 2 * PAGE);
  close(fd);
  unlink_file("mfp_multi");
}

/* TC4: offset mapping — mmap at offset=PAGE maps the second page's contents. */
void test_mmap_file_private_offset(void) {
  char data[8192];
  memcpy(data, "page0", 5);
  memcpy(data + PAGE, "page1", 5);

  int fd = make_file("mfp_off", data, sizeof(data));
  TEST_ASSERT_TRUE(fd >= 0);

  char *p = (char *)mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
  TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);
  TEST_ASSERT_EQUAL_INT(0, memcmp(p, "page1", 5));
  munmap(p, PAGE);
  close(fd);
  unlink_file("mfp_off");
}

/* TC5: the mapping survives close(fd) — the region holds an inode reference, so
 * page-in works after the fd is closed (Linux semantics). */
void test_mmap_file_private_survives_close(void) {
  int fd = make_file("mfp_close", "hello", 5);
  TEST_ASSERT_TRUE(fd >= 0);

  char *p = (char *)mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
  TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);
  close(fd); // drop the fd; the region's inode ref keeps the mapping valid
  TEST_ASSERT_EQUAL_INT(0, memcmp(p, "hello", 5));
  munmap(p, PAGE);
  unlink_file("mfp_close");
}

/* TC6: memfd MAP_PRIVATE — ftruncate sizes the memfd, data is written through a
 * MAP_SHARED mapping, then a MAP_PRIVATE mapping reads a private COW copy. */
void test_mmap_memfd_private_cow(void) {
  int fd = memfd_create("mfp_memfd", 0);
  TEST_ASSERT_TRUE(fd >= 0);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, PAGE));

  char *shared =
      (char *)mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  TEST_ASSERT_TRUE(shared != NULL && shared != MAP_FAILED);
  memcpy(shared, "abc", 3);
  munmap(shared, PAGE);

  char *priv =
      (char *)mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  TEST_ASSERT_TRUE(priv != NULL && priv != MAP_FAILED);
  TEST_ASSERT_EQUAL_INT('a', priv[0]); // read sees the memfd contents
  priv[0] = 'X';
  TEST_ASSERT_EQUAL_INT('X', priv[0]); // private write
  munmap(priv, PAGE);
  close(fd);
}

/* TC7: fork independence. fork copies the parent's address space, so the child
 * inherits the parent's in-flight MAP_PRIVATE write ('P') — that is NOT a write
 * to the backing file, it is private memory, and fork snapshots it. The
 * invariant that MAP_PRIVATE guarantees is post-fork isolation: the child's
 * write is invisible to the parent, and the parent's later write is invisible
 * to the child. (Verified against host Linux: child sees the parent's private
 * 'P', not the original 'h'.) */
void test_mmap_file_private_fork_independent(void) {
  int fd = make_file("mfp_fork", "hello", 5);
  TEST_ASSERT_TRUE(fd >= 0);

  char *p =
      (char *)mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);
  p[0] = 'P'; // parent's private write — inherited by the child's snapshot

  pid_t pid = fork();
  if (pid == 0) {
    /* Child inherits the parent's private copy (p[0]=='P'), not the file. */
    int child_ok = (p[0] == 'P');
    p[0] = 'C'; // child's private write — must not reach the parent
    _exit(child_ok ? 0 : 1);
  } else if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0,
                          WEXITSTATUS(status)); // child saw the inherited 'P'
    TEST_ASSERT_EQUAL_INT('P', p[0]); // child's 'C' did not reach the parent
    p[0] = 'Q';                       // parent's later private write
    munmap(p, PAGE);
    close(fd);
  } else {
    munmap(p, PAGE);
    close(fd);
    TEST_ASSERT_TRUE(1); /* fork unavailable — skip */
  }
  unlink_file("mfp_fork");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mmap_file_private_basic_read);
  RUN_TEST(test_mmap_file_private_write_cow);
  RUN_TEST(test_mmap_file_private_multi_page);
  RUN_TEST(test_mmap_file_private_offset);
  RUN_TEST(test_mmap_file_private_survives_close);
  RUN_TEST(test_mmap_memfd_private_cow);
  RUN_TEST(test_mmap_file_private_fork_independent);
  return UNITY_END();
}
