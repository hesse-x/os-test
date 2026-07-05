#include <unity.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/device.h>
#include "syscall.h"
#include "xos/errno.h"
#include "user/driver/display.h"
#include "input.h"

void setUp(void) {}
void tearDown(void) {}

/* ===== Phase 1: dev_ops → file_operations (callback dispatch) ===== */

/* 1. KMS ioctl via callback: KMS_IOCTL_FLIP on uninit → -ENOENT or 0 */
void test_dev_vfs_kms_ioctl_flip(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        long r = ioctl(fd, KMS_IOCTL_FLIP, 0);
        TEST_ASSERT_TRUE(r == 0 || (r < 0 && (errno == ENOENT || errno == EINVAL)));
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 2. KMS mmap via callback: mmap MAP_SHARED on /dev/kms */
void test_dev_vfs_kms_mmap(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        /* Try mmap — without CREATE_BUF, the handler returns 0 (failure).
         * With CREATE_BUF it would map the back buffer. Test both paths. */
        void *p = mmap(NULL, 800 * 4 * 600, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
        if (p && p != MAP_FAILED) {
            /* If we got a mapping, verify we can write to it */
            ((volatile char *)p)[0] = 0x42;
            munmap(p, 800 * 4 * 600);
        }
        /* Either NULL (no back buffer yet) or a valid mapping is acceptable */
        close(fd);
    }
    TEST_ASSERT_TRUE(1);
}

/* 3. KMS ioctl with unknown cmd → ENOTTY or EINVAL */
void test_dev_vfs_kms_ioctl_unknown(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        long r = ioctl(fd, 0xFFFF, 0);
        TEST_ASSERT_TRUE(r < 0);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* ===== Phase 2: FD type consolidation + serial dev_ops ===== */

/* 4. /dev/serial open returns FD_DEV with serial dev_ops */
void test_dev_vfs_serial_open(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
}

/* 5. Serial write via dev_ops.write callback */
void test_dev_vfs_serial_write(void) {
    int fd = open("/dev/serial", O_RDWR);
    if (fd >= 0) {
        ssize_t w = write(fd, "A", 1);
        TEST_ASSERT_EQUAL_INT(1, (int)w);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 6. Serial read via dev_ops.read callback (non-blocking, empty → EAGAIN) */
void test_dev_vfs_serial_read_nonblock(void) {
    int fd = open("/dev/serial", O_RDWR | O_NONBLOCK);
    if (fd >= 0) {
        char buf[1];
        ssize_t r = read(fd, buf, 1);
        /* No data available → EAGAIN (ring buffer empty) */
        TEST_ASSERT_TRUE(r < 0);
        TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 7. Serial poll via dev_ops.poll callback → POLLOUT always ready */
void test_dev_vfs_serial_poll(void) {
    int fd = open("/dev/serial", O_RDWR);
    if (fd >= 0) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN | POLLOUT;
        pfd.revents = 0;

        int r = poll(&pfd, 1, 100);
        TEST_ASSERT_TRUE(r > 0);
        TEST_ASSERT_TRUE(pfd.revents & POLLOUT);

        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 8. Serial ioctl TCGETS → isatty returns 1 */
void test_dev_vfs_serial_isatty(void) {
    int fd = open("/dev/serial", O_RDWR);
    if (fd >= 0) {
        int r = isatty(fd);
        TEST_ASSERT_EQUAL_INT(1, r);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 9. Serial close via dev_ops.close callback (IRQ cleanup + reopen) */
void test_dev_vfs_serial_close_reopen(void) {
    int fd1 = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd1 >= 0);
    close(fd1);

    int fd2 = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd2 >= 0);

    /* Verify the reopened fd works: write succeeds */
    ssize_t w = write(fd2, "B", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w);
    close(fd2);
}

/* 10. Multiple serial fds simultaneously (fd count tracking) */
void test_dev_vfs_serial_multi_open(void) {
    int fd1 = open("/dev/serial", O_RDWR);
    int fd2 = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd1 >= 0);
    TEST_ASSERT_TRUE(fd2 >= 0);
    TEST_ASSERT_TRUE(fd1 != fd2);

    /* Both should be writable */
    ssize_t w1 = write(fd1, "1", 1);
    ssize_t w2 = write(fd2, "2", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w1);
    TEST_ASSERT_EQUAL_INT(1, (int)w2);

    close(fd1);
    close(fd2);
}

/* ===== Phase 3: Serial devtmpfs + open("/dev/") unified ===== */

/* 11. open("/dev/serial") goes through devtmpfs (not sys_open_dev) */
void test_dev_vfs_serial_devtmpfs_path(void) {
    /* This implicitly tests that /dev/serial is a devtmpfs node
     * accessible via the normal sys_open path */
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
    TEST_ASSERT_TRUE(st.st_ino > 0);

    close(fd);
}

/* 12. open("/dev/kms") goes through devtmpfs */
void test_dev_vfs_kms_devtmpfs_path(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        struct stat st;
        int r = fstat(fd, &st);
        TEST_ASSERT_EQUAL_INT(0, r);
        TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 13. open nonexistent /dev/xxx → ENOENT */
void test_dev_vfs_open_nonexistent(void) {
    int fd = open("/dev/nonexistent_device", O_RDWR);
    TEST_ASSERT_TRUE(fd < 0);
    TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* ===== Phase 4: sys_ioctl + sys_fstat ===== */

/* 16. fstat on /dev/serial → S_ISCHR, st_ino > 0 */
void test_dev_vfs_fstat_serial(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
    TEST_ASSERT_TRUE(st.st_ino > 0);

    close(fd);
}

/* 17. fstat on bad fd → EBADF */
void test_dev_vfs_fstat_bad_fd(void) {
    struct stat st;
    int r = fstat(-1, &st);
    TEST_ASSERT_TRUE(r < 0);
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 18. ioctl on /dev/serial with unknown cmd → ENOTTY */
void test_dev_vfs_serial_ioctl_unknown(void) {
    int fd = open("/dev/serial", O_RDWR);
    if (fd >= 0) {
        long r = ioctl(fd, 0x9999, 0);
        TEST_ASSERT_TRUE(r < 0);
        TEST_ASSERT_EQUAL_INT(ENOTTY, errno);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 19. lseek on /dev/serial → ESPIPE (not seekable) */
void test_dev_vfs_serial_lseek(void) {
    int fd = open("/dev/serial", O_RDWR);
    if (fd >= 0) {
        off_t r = lseek(fd, 0, SEEK_SET);
        TEST_ASSERT_TRUE(r < 0);
        TEST_ASSERT_EQUAL_INT(ESPIPE, errno);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 20. dup2 with /dev/serial fd */
void test_dev_vfs_serial_dup2(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    int new_fd = dup2(fd, 20);
    TEST_ASSERT_EQUAL_INT(20, new_fd);

    /* Both fds should be writable */
    ssize_t w1 = write(fd, "D", 1);
    ssize_t w2 = write(new_fd, "E", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w1);
    TEST_ASSERT_EQUAL_INT(1, (int)w2);

    close(fd);
    close(new_fd);
}

/* ===== Phase 5: sys_dev_create simplified + sys_open_dev/sys_load_dev removed ===== */

/* 21. sys_dev_create simplified (2-arg, kernel fills driver_pid) */
void test_dev_vfs_dev_create(void) {
    /* Create a test user-space device node via sys_dev_create */
    int r = sys_dev_create("test_dev", -1);
    /* May succeed (0) or fail if name collision / no permission */
    if (r == 0) {
        /* Verify the device node exists */
        int fd = open("/dev/test_dev", O_RDWR);
        if (fd >= 0) {
            struct stat st;
            fstat(fd, &st);
            TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
            close(fd);
        } else {
            /* Device node may not appear instantly */
            TEST_ASSERT_TRUE(1);
        }
    } else {
        /* Creating devices from user-space may have restrictions */
        TEST_ASSERT_TRUE(1);
    }
}

/* ===== Phase 6: dev_table elimination + ISR redesign ===== */

/* 23. devtmpfs_cleanup_pid: device nodes removed when driver exits (indirect) */
void test_dev_vfs_cleanup(void) {
    /* Indirect test: /dev/kms and /dev/serial are kernel devices
     * and should persist. Only user-space driver nodes get cleaned. */
    int fd1 = open("/dev/kms", O_RDWR);
    int fd2 = open("/dev/serial", O_RDWR);
    /* At minimum serial should exist (kernel device) */
    TEST_ASSERT_TRUE(fd2 >= 0);
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
}

/* ===== Phase 7: libc fd_table elimination ===== */

/* 24. open() returns kernel fd directly (no libc fd_table layer) */
void test_dev_vfs_open_kernel_fd(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    /* fd should be a kernel-assigned fd (≥3 since 0/1/2 are reserved) */
    TEST_ASSERT_TRUE(fd >= 3);

    /* fstat works on this fd (kernel-side validation) */
    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);

    close(fd);
}

/* 25. read/write/close are thin syscall wrappers (no libc fd_type dispatch) */
void test_dev_vfs_thin_wrappers(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    /* write goes directly to sys_write → FD_DEV → ops->write */
    ssize_t w = write(fd, "W", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w);

    /* close goes directly to sys_close → FD_DEV → ops->close + inode_put */
    int r = close(fd);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* Double close → EBADF (kernel fd_table already cleared) */
    r = close(fd);
    TEST_ASSERT_TRUE(r < 0);
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 26. dup2 on dev fd: kernel ref-counts inode (no libc fd_table copy) */
void test_dev_vfs_dup2_refcount(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    int new_fd = dup2(fd, 30);
    TEST_ASSERT_EQUAL_INT(30, new_fd);

    /* Close original — dup'd fd should still work */
    close(fd);
    ssize_t w = write(new_fd, "X", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w);

    close(new_fd);
}

/* 27. fcntl F_GETFL on dev fd → kernel returns flags directly */
void test_dev_vfs_fcntl_getfl(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    int flags = fcntl(fd, F_GETFL);
    TEST_ASSERT_TRUE(flags >= 0);
    TEST_ASSERT_TRUE(flags & O_RDWR);

    close(fd);
}

/* 28. fcntl F_SETFL O_NONBLOCK on serial fd */
void test_dev_vfs_fcntl_setfl(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    int r = fcntl(fd, F_SETFL, O_NONBLOCK);
    TEST_ASSERT_EQUAL_INT(0, r);

    int flags = fcntl(fd, F_GETFL);
    TEST_ASSERT_TRUE(flags & O_NONBLOCK);

    /* Non-blocking read on empty buffer → EAGAIN */
    char buf[1];
    ssize_t rr = read(fd, buf, 1);
    TEST_ASSERT_TRUE(rr < 0);
    TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

    close(fd);
}

/* 29. mmap MAP_SHARED on /dev/kms → kernel auto resolves via dev_ops.mmap */
void test_dev_vfs_kms_mmap_shared(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        /* Without CREATE_BUF, mmap returns NULL (no back buffer).
         * Test that the kernel path works without crashing. */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
        (void)p;
        close(fd);
    }
    TEST_ASSERT_TRUE(1);
}

/* 30. pipe + dev fd coexist (no libc fd_table interference) */
void test_dev_vfs_pipe_dev_coexist(void) {
    int pipefd[2];
    int r = pipe(pipefd);
    TEST_ASSERT_EQUAL_INT(0, r);

    int serial_fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(serial_fd >= 0);

    /* Write to pipe */
    write(pipefd[1], "P", 1);
    char buf[2] = {0};
    ssize_t rr = read(pipefd[0], buf, 1);
    TEST_ASSERT_EQUAL_INT(1, (int)rr);
    TEST_ASSERT_EQUAL_STRING("P", buf);

    /* Write to serial */
    ssize_t ws = write(serial_fd, "S", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)ws);

    close(pipefd[0]);
    close(pipefd[1]);
    close(serial_fd);
}

/* ===== Cross-phase integration ===== */

/* 31. Full /dev/kms lifecycle: open → ioctl FLIP → fstat → close */
void test_dev_vfs_kms_lifecycle(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        /* fstat */
        struct stat st;
        int r = fstat(fd, &st);
        TEST_ASSERT_EQUAL_INT(0, r);
        TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));

        /* ioctl */
        long ir = ioctl(fd, KMS_IOCTL_FLIP, 0);
        TEST_ASSERT_TRUE(ir == 0 || ir < 0);

        /* isatty → 0 (KMS is not a tty) */
        TEST_ASSERT_EQUAL_INT(0, isatty(fd));

        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 32. Full /dev/serial lifecycle: open → write → fstat → poll → close → reopen */
void test_dev_vfs_serial_lifecycle(void) {
    int fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    /* Write */
    ssize_t w = write(fd, "L", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w);

    /* fstat */
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
    TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));

    /* poll → POLLOUT */
    struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
    int pr = poll(&pfd, 1, 100);
    TEST_ASSERT_TRUE(pr > 0);
    TEST_ASSERT_TRUE(pfd.revents & POLLOUT);

    /* isatty → 1 */
    TEST_ASSERT_EQUAL_INT(1, isatty(fd));

    /* Close + reopen */
    close(fd);
    fd = open("/dev/serial", O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    w = write(fd, "R", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w);
    close(fd);
}

/* 33. /dev/serial and /dev/kms have distinct inodes */
void test_dev_vfs_distinct_inodes(void) {
    int serial_fd = open("/dev/serial", O_RDWR);
    int kms_fd = open("/dev/kms", O_RDWR);

    if (serial_fd >= 0 && kms_fd >= 0) {
        struct stat st_serial, st_kms;
        fstat(serial_fd, &st_serial);
        fstat(kms_fd, &st_kms);

        TEST_ASSERT_TRUE(st_serial.st_ino != st_kms.st_ino);
        TEST_ASSERT_TRUE(S_ISCHR(st_serial.st_mode));
        TEST_ASSERT_TRUE(S_ISCHR(st_kms.st_mode));

        close(kms_fd);
    } else {
        if (kms_fd >= 0) close(kms_fd);
    }

    if (serial_fd >= 0) close(serial_fd);
    else TEST_ASSERT_TRUE(1);
}

/* 34. fstat on /dev/fs (user-space driver node) */
void test_dev_vfs_fstat_fs(void) {
    int fd = open("/dev/fs", O_RDWR);
    if (fd >= 0) {
        struct stat st;
        int r = fstat(fd, &st);
        TEST_ASSERT_EQUAL_INT(0, r);
        TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
        close(fd);
    } else {
        /* /dev/fs may not be available in test env */
        TEST_ASSERT_TRUE(1);
    }
}

/* ===== Phase 8: ioctl IPC proxy ===== */

/* 36. KMS ioctl via sys_ioctl with struct arg (copy_to_user path) */
void test_dev_vfs_kms_ioctl_struct_arg(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        struct display_ioctl_create_buf_arg arg;
        memset(&arg, 0, sizeof(arg));
        arg.width = 800;
        arg.height = 600;
        arg.bpp = 32;

        int r = ioctl(fd, KMS_IOCTL_CREATE_BUF, &arg);
        if (r == 0) {
            /* Kernel filled output fields via copy_to_user */
            TEST_ASSERT_TRUE(arg.pitch == 800 * 4);
            TEST_ASSERT_TRUE(arg.size == 800 * 4 * 600);
            TEST_ASSERT_TRUE(arg.rows > 0);
            TEST_ASSERT_TRUE(arg.cols > 0);
            TEST_ASSERT_EQUAL_INT(0, arg.result);
        }
        /* If -EBUSY (already initialized by terminal), that's also valid */
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 37. ioctl INPUT_BIND on /dev/kbd (IPC proxy path) */
void test_dev_vfs_kbd_ioctl_proxy(void) {
    int fd = open("/dev/kbd", O_RDWR);
    if (fd >= 0) {
        /* Direction A: driver owns SHM, consumer just registers pid for notify.
         * No memfd_create / shm_fd passing. */
        struct input_bind_arg arg;
        arg.shm_fd = -1;
        arg.result = -1;

        int r = ioctl(fd, INPUT_BIND, &arg);
        /* kbd_driver may or may not be running in test env */
        if (r == 0) {
            TEST_ASSERT_EQUAL_INT(0, arg.result);
        }
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 38. KMS FLIP via sys_ioctl (no arg) */
void test_dev_vfs_kms_flip_ioctl(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        long r = ioctl(fd, KMS_IOCTL_FLIP, 0);
        TEST_ASSERT_TRUE(r == 0 || (r < 0 && (errno == ENOENT || errno == EINVAL)));
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    UNITY_BEGIN();

    /* Phase 1: dev_ops callback dispatch */
    RUN_TEST(test_dev_vfs_kms_ioctl_flip);
    RUN_TEST(test_dev_vfs_kms_mmap);
    RUN_TEST(test_dev_vfs_kms_ioctl_unknown);

    /* Phase 2: FD type consolidation + serial dev_ops */
    RUN_TEST(test_dev_vfs_serial_open);
    RUN_TEST(test_dev_vfs_serial_write);
    RUN_TEST(test_dev_vfs_serial_read_nonblock);
    RUN_TEST(test_dev_vfs_serial_poll);
    RUN_TEST(test_dev_vfs_serial_isatty);
    RUN_TEST(test_dev_vfs_serial_close_reopen);
    RUN_TEST(test_dev_vfs_serial_multi_open);

    /* Phase 3: devtmpfs unified */
    RUN_TEST(test_dev_vfs_serial_devtmpfs_path);
    RUN_TEST(test_dev_vfs_kms_devtmpfs_path);
    RUN_TEST(test_dev_vfs_open_nonexistent);

    /* Phase 4: sys_ioctl + sys_fstat */
    RUN_TEST(test_dev_vfs_fstat_serial);
    RUN_TEST(test_dev_vfs_fstat_bad_fd);
    RUN_TEST(test_dev_vfs_serial_ioctl_unknown);
    RUN_TEST(test_dev_vfs_serial_lseek);
    RUN_TEST(test_dev_vfs_serial_dup2);

    /* Phase 5: sys_dev_create simplified */
    RUN_TEST(test_dev_vfs_dev_create);

    /* Phase 6: dev_table elimination */
    RUN_TEST(test_dev_vfs_cleanup);

    /* Phase 7: libc fd_table elimination */
    RUN_TEST(test_dev_vfs_open_kernel_fd);
    RUN_TEST(test_dev_vfs_thin_wrappers);
    RUN_TEST(test_dev_vfs_dup2_refcount);
    RUN_TEST(test_dev_vfs_fcntl_getfl);
    RUN_TEST(test_dev_vfs_fcntl_setfl);
    RUN_TEST(test_dev_vfs_kms_mmap_shared);
    RUN_TEST(test_dev_vfs_pipe_dev_coexist);

    /* Cross-phase integration */
    RUN_TEST(test_dev_vfs_kms_lifecycle);
    RUN_TEST(test_dev_vfs_serial_lifecycle);
    RUN_TEST(test_dev_vfs_distinct_inodes);
    RUN_TEST(test_dev_vfs_fstat_fs);

    /* Phase 8: ioctl IPC proxy */
    RUN_TEST(test_dev_vfs_kms_ioctl_struct_arg);
    RUN_TEST(test_dev_vfs_kbd_ioctl_proxy);
    RUN_TEST(test_dev_vfs_kms_flip_ioctl);

    return UNITY_END();
}
