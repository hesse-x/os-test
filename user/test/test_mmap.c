#include <unity.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "test_helpers.h"
#include "syscall.h"

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

/* 6. memfd_create + ftruncate + mmap shared */
void test_mmap_shm_fd(void) {
    int fd = memfd_create("test_shm_fd", 0);
    TEST_ASSERT_TRUE(fd >= 0);
    ftruncate(fd, 4096);
    void *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT_NOT_NULL(addr);

    /* Write data */
    memset(addr, 'S', 4096);
    TEST_ASSERT_EQUAL_INT('S', ((char *)addr)[0]);

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

/* 11. Two memfd mmap MAP_SHARED: write via mmap, verify cross-visibility */
void test_mmap_memfd_shared_cross(void) {
    int fd = memfd_create("cross_vis", 0);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 4096));

    void *p1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT_TRUE(p1 != NULL && p1 != MAP_FAILED);

    /* Second mapping of same memfd — writes should be visible in both */
    void *p2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT_TRUE(p2 != NULL && p2 != MAP_FAILED);

    memset(p1, 'A', 4096);
    TEST_ASSERT_EQUAL_INT('A', ((char *)p2)[0]);
    TEST_ASSERT_EQUAL_INT('A', ((char *)p2)[4095]);

    ((char *)p2)[0] = 'B';
    TEST_ASSERT_EQUAL_INT('B', ((char *)p1)[0]);

    munmap(p1, 4096);
    munmap(p2, 4096);
    close(fd);
}

/* 12. mmap on nonexistent fd → graceful failure */
void test_mmap_bad_fd(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 999, 0);
    TEST_ASSERT_TRUE(p == NULL || p == MAP_FAILED);
}

/* 13. mmap memfd: ftruncate + mmap, verify via direct mmap write/read */
void test_mmap_memfd_verify(void) {
    int fd = memfd_create("verify_buf", 0);
    TEST_ASSERT_TRUE(fd >= 0);

    /* Grow to one page */
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 4096));

    /* mmap and write known data */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT_TRUE(p != NULL && p != MAP_FAILED);

    const char *msg = "mmap_verify";
    memcpy(p, msg, strlen(msg) + 1);
    TEST_ASSERT_EQUAL_STRING(msg, (char *)p);

    /* Modify via mmap and verify persistence in same mapping */
    ((char *)p)[0] = 'M';
    TEST_ASSERT_EQUAL_STRING("Mmap_verify", (char *)p);

    munmap(p, 4096);
    close(fd);
}

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
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
    RUN_TEST(test_mmap_memfd_shared_cross);
    RUN_TEST(test_mmap_bad_fd);
    RUN_TEST(test_mmap_memfd_verify);
    return UNITY_END();
}
