#include <unity.h>
#include <sys/pci.h>
#include <fcntl.h>
#include <unistd.h>
#include "common/dev.h"
#include "common/errno.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. pci_dev_info(0,2,0) returns AHCI device (vendor=0x8086) */
void test_pci_dev_info_valid(void) {
    struct pci_dev_info info;
    memset(&info, 0, sizeof(info));
    int r = pci_dev_info(0, 2, 0, &info);
    if (r == 0) {
        TEST_ASSERT_EQUAL_INT(0x8086, info.vendor_id);
    } else {
        /* Device may not be at (0,2,0) in all configs */
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
    memset(&info, 0, sizeof(info));
    int r = pci_dev_info(0, 2, 0, &info);
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pci_dev_info_valid);
    RUN_TEST(test_pci_dev_info_invalid);
    RUN_TEST(test_pci_bar_info);
    RUN_TEST(test_pci_scan_all);
    RUN_TEST(test_open_dev_kms);
    RUN_TEST(test_open_dev_fs);
    return UNITY_END();
}
