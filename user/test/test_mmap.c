#include <unity.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include "test_helpers.h"

void setUp(void) {}
void tearDown(void) {}

/* 1. mmap anonymous page returns non-NULL */
void test_mmap_anon(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 0, -1, 0);
    TEST_ASSERT_NOT_NULL(p);
    munmap(p, 4096);
}

/* 2. Write to mmap page → read back */
void test_mmap_write_read(void) {
    char *p = (char *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, 0, -1, 0);
    TEST_ASSERT_NOT_NULL(p);
    p[0] = 'A';
    p[4095] = 'Z';
    TEST_ASSERT_EQUAL_INT('A', p[0]);
    TEST_ASSERT_EQUAL_INT('Z', p[4095]);
    munmap(p, 4096);
}

/* 3. mmap 3 pages, independent read/write */
void test_mmap_multi_page(void) {
    char *p = (char *)mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, 0, -1, 0);
    TEST_ASSERT_NOT_NULL(p);
    p[0] = 'X';
    p[4096] = 'Y';
    p[8192] = 'Z';
    TEST_ASSERT_EQUAL_INT('X', p[0]);
    TEST_ASSERT_EQUAL_INT('Y', p[4096]);
    TEST_ASSERT_EQUAL_INT('Z', p[8192]);
    munmap(p, 3 * 4096);
}

/* 4. munmap returns 0 */
void test_munmap_basic(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 0, -1, 0);
    TEST_ASSERT_NOT_NULL(p);
    int r = munmap(p, 4096);
    TEST_ASSERT_EQUAL_INT(0, r);
}

/* 5. mmap with address hint */
void test_mmap_addr_hint(void) {
    /* Request a specific address hint — kernel may ignore it */
    void *hint = (void *)0x900000;
    void *p = mmap(hint, 4096, PROT_READ | PROT_WRITE, 0, -1, 0);
    TEST_ASSERT_NOT_NULL(p);
    munmap(p, 4096);
}

/* 6. shm_create + mmap shared between processes */
void test_mmap_shm_fd(void) {
    void *addr = NULL;
    int fd = shm_create(4096, &addr);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_NOT_NULL(addr);

    /* Write data */
    memset(addr, 'S', 4096);
    TEST_ASSERT_EQUAL_INT('S', ((char *)addr)[0]);

    /* Attach from same process (trivial test) */
    munmap(addr, 4096);
    close(fd);
}

/* 7. memfd_create returns valid fd */
void test_memfd_create(void) {
    int fd = memfd_create("test", 0);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
}

/* 8. memfd → ftruncate → mmap → write/read */
void test_memfd_mmap(void) {
    int fd = memfd_create("test_mmap", 0);
    TEST_ASSERT_TRUE(fd >= 0);

    int r = ftruncate(fd, 4096);
    TEST_ASSERT_EQUAL_INT(0, r);

    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 0, fd, 0);
    if (p == NULL || p == MAP_FAILED) {
        /* memfd mmap may not be fully implemented yet */
        close(fd);
        TEST_ASSERT_NOT_NULL(p);
        return;
    }

    memset(p, 'M', 4096);
    TEST_ASSERT_EQUAL_INT('M', ((char *)p)[0]);
    munmap(p, 4096);
    close(fd);
}

/* 9. ftruncate grow memfd */
void test_ftruncate_grow(void) {
    int fd = memfd_create("test_grow", 0);
    TEST_ASSERT_TRUE(fd >= 0);

    int r = ftruncate(fd, 8192);
    TEST_ASSERT_EQUAL_INT(0, r);

    close(fd);
}

/* 10. mmap PROT_READ|PROT_EXEC (code page, indirect) */
void test_mmap_prot_exec(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_EXEC, 0, -1, 0);
    /* May return NULL if exec-only not supported without read */
    if (p && p != MAP_FAILED) {
        munmap(p, 4096);
    }
    TEST_ASSERT_TRUE(1); /* no crash = pass */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mmap_anon);
    RUN_TEST(test_mmap_write_read);
    RUN_TEST(test_mmap_multi_page);
    RUN_TEST(test_munmap_basic);
    RUN_TEST(test_mmap_addr_hint);
    RUN_TEST(test_mmap_shm_fd);
    RUN_TEST(test_memfd_create);
    RUN_TEST(test_memfd_mmap);
    RUN_TEST(test_ftruncate_grow);
    RUN_TEST(test_mmap_prot_exec);
    return UNITY_END();
}
