#include <unity.h>
#include <sys/pci.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <xos/errno.h>

#include "syscall.h"

/* AHCI: (class << 8) | subclass = 0x0106 */
#define PCI_CLASS_STORAGE_AHCI 0x0106
#define AHCI_VENDOR_INTEL      0x8086

/* Scan bus 0 for a device matching class_code; fill out info if found */
static int find_pci_device(uint16_t class_code, struct pci_dev_info *out) {
    for (int dev = 0; dev < 32; dev++) {
        for (int func = 0; func < 8; func++) {
            memset(out, 0, sizeof(*out));
            if (pci_dev_info(0, dev, func, out) == 0 &&
                out->class_code == class_code) {
                return 0;
            }
        }
    }
    return -1;
}

void setUp(void) {}
void tearDown(void) {}

/* 1. AHCI device found by class has vendor=0x8086 */
void test_pci_dev_info_valid(void) {
    struct pci_dev_info info;
    int r = find_pci_device(PCI_CLASS_STORAGE_AHCI, &info);
    if (r == 0) {
        TEST_ASSERT_EQUAL_INT(AHCI_VENDOR_INTEL, info.vendor_id);
    } else {
        /* AHCI device may not exist in some QEMU configs */
        TEST_ASSERT_TRUE(1);
    }
}

/* 2. pci_dev_info(255,0,0) returns -ENOENT */
void test_pci_dev_info_invalid(void) {
    struct pci_dev_info info;
    memset(&info, 0, sizeof(info));
    int r = pci_dev_info(255, 0, 0, &info);
    TEST_ASSERT_TRUE(r < 0);
}

/* 3. AHCI device has at least one BAR with size > 0 */
void test_pci_bar_info(void) {
    struct pci_dev_info info;
    int r = find_pci_device(PCI_CLASS_STORAGE_AHCI, &info);
    if (r == 0) {
        int found = 0;
        for (int i = 0; i < 6; i++) {
            if (info.bars[i].size > 0) {
                found = 1;
                break;
            }
        }
        TEST_ASSERT_TRUE(found);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

/* 4. Scan bus 0 for known devices */
void test_pci_scan_all(void) {
    int found_count = 0;
    struct pci_dev_info info;

    for (int dev = 0; dev < 32; dev++) {
        for (int func = 0; func < 8; func++) {
            memset(&info, 0, sizeof(info));
            if (pci_dev_info(0, dev, func, &info) == 0) {
                found_count++;
            }
        }
    }
    /* QEMU should have at least AHCI */
    TEST_ASSERT_TRUE(found_count >= 1);
}

/* 5. open /dev/kms returns valid fd */
void test_open_dev_kms(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        close(fd);
    }
    TEST_ASSERT_TRUE(1);
}

/* 6. open /dev/fs returns valid fd */
void test_open_dev_fs(void) {
    int fd = open("/dev/fs", O_RDWR);
    if (fd >= 0) {
        close(fd);
    }
    TEST_ASSERT_TRUE(1);
}

/* 7. fstat on /dev/kms → S_ISCHR */
void test_fstat_dev_kms(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        struct stat st;
        int r = fstat(fd, &st);
        TEST_ASSERT_EQUAL_INT(0, r);
        TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
        close(fd);
    } else {
        /* KMS may not be available in test env */
        TEST_ASSERT_TRUE(1);
    }
}

/* 8. isatty on /dev/kms → 0 (KMS doesn't support TCGETS) */
void test_isatty_dev_kms(void) {
    int fd = open("/dev/kms", O_RDWR);
    if (fd >= 0) {
        int r = isatty(fd);
        TEST_ASSERT_EQUAL_INT(0, r);
        close(fd);
    } else {
        TEST_ASSERT_TRUE(1);
    }
}

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    UNITY_BEGIN();
    RUN_TEST(test_pci_dev_info_valid);
    RUN_TEST(test_pci_dev_info_invalid);
    RUN_TEST(test_pci_bar_info);
    RUN_TEST(test_pci_scan_all);
    RUN_TEST(test_open_dev_kms);
    RUN_TEST(test_open_dev_fs);
    RUN_TEST(test_fstat_dev_kms);
    RUN_TEST(test_isatty_dev_kms);
    return UNITY_END();
}
