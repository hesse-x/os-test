/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm/drm.h>

_Static_assert(sizeof(struct udmabuf_create) == 24, "udmabuf create ABI");
_Static_assert(sizeof(struct udmabuf_create_item) == 24, "udmabuf item ABI");
_Static_assert(offsetof(struct udmabuf_create_list, list) == 8,
               "udmabuf list ABI");
_Static_assert(UDMABUF_CREATE == 0x40187542UL, "UDMABUF_CREATE value");
_Static_assert(UDMABUF_CREATE_LIST == 0x40087543UL,
               "UDMABUF_CREATE_LIST value");
_Static_assert(DMA_BUF_IOCTL_SYNC == 0x40086200UL, "DMA_BUF_IOCTL_SYNC value");

static int create_memfd(size_t size, unsigned seals) {
  int fd = memfd_create("udmabuf-test", MFD_ALLOW_SEALING);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, (off_t)size));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_ADD_SEALS, seals));
  return fd;
}

static int create_udmabuf(int dev, int memfd, uint32_t flags, uint64_t offset,
                          uint64_t size) {
  struct udmabuf_create create = {
      .memfd = (uint32_t)memfd,
      .flags = flags,
      .offset = offset,
      .size = size,
  };
  return ioctl(dev, UDMABUF_CREATE, &create);
}

void setUp(void) {}
void tearDown(void) {}

static void test_memfd_seal_contract(void) {
  int fd = memfd_create("no-sealing", 0);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_BITS_HIGH(F_SEAL_SEAL, fcntl(fd, F_GET_SEALS));
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
  close(fd);

  fd = create_memfd(4096, F_SEAL_SHRINK);
  void *writable = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, writable);
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
  TEST_ASSERT_EQUAL_INT(EBUSY, errno);
  TEST_ASSERT_EQUAL_INT(0, munmap(writable, 4096));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
  close(fd);

  fd = create_memfd(4096, F_SEAL_SHRINK);
  TEST_ASSERT_BITS_HIGH(F_SEAL_SHRINK, fcntl(fd, F_GET_SEALS));
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, ftruncate(fd, 0));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
  close(fd);
}

static void test_create_fd_mmap_sync_lifetime(void) {
  int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dev);
  int memfd = create_memfd(8192, F_SEAL_SHRINK);
  uint32_t *mem =
      mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, mem);
  mem[0] = 0x11223344;
  mem[1024] = 0xaabbccdd;

  int dmabuf = create_udmabuf(dev, memfd, UDMABUF_FLAGS_CLOEXEC, 0, 8192);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dmabuf);
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, fcntl(memfd, F_ADD_SEALS, F_SEAL_WRITE));
  TEST_ASSERT_EQUAL_INT(EBUSY, errno);
  TEST_ASSERT_BITS_HIGH(FD_CLOEXEC, fcntl(dmabuf, F_GETFD));
  close(memfd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(dmabuf, &st));
  TEST_ASSERT_EQUAL_INT64(8192, st.st_size);
  TEST_ASSERT_EQUAL_INT64(8192, lseek(dmabuf, 0, SEEK_END));
  TEST_ASSERT_EQUAL_INT64(0, lseek(dmabuf, 0, SEEK_SET));
  errno = 0;
  TEST_ASSERT_EQUAL_INT64(-1, lseek(dmabuf, 1, SEEK_SET));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, read(dmabuf, &memfd, sizeof(memfd)));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, write(dmabuf, &memfd, sizeof(memfd)));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, ftruncate(dmabuf, 4096));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, fsync(dmabuf));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  uint32_t *map =
      mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, map);
  TEST_ASSERT_EQUAL_HEX32(0x11223344, map[0]);
  TEST_ASSERT_EQUAL_HEX32(0xaabbccdd, map[1024]);
  struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW};
  TEST_ASSERT_EQUAL_INT(0, ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &sync));
  map[1] = 0x55667788;
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
  TEST_ASSERT_EQUAL_INT(0, ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &sync));
  TEST_ASSERT_EQUAL_HEX32(0x55667788, mem[1]);
  TEST_ASSERT_EQUAL_INT(0, ioctl(dmabuf, DMA_BUF_SET_NAME, "udmabuf-test"));

  struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
  TEST_ASSERT_EQUAL_INT(0,
                        ioctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, export.fd);
  struct pollfd pfd = {.fd = export.fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, 0));
  close(export.fd);

  close(dmabuf);
  map[2] = 0xdeadbeef;
  TEST_ASSERT_EQUAL_HEX32(0xdeadbeef, mem[2]);
  TEST_ASSERT_EQUAL_INT(0, munmap(map, 8192));
  TEST_ASSERT_EQUAL_INT(0, munmap(mem, 8192));
  close(dev);
}

static void test_create_list_alias_and_negative(void) {
  int dev = open("/dev/udmabuf", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dev);
  int memfd = create_memfd(8192, F_SEAL_SHRINK);
  size_t request_size = sizeof(struct udmabuf_create_list) +
                        2 * sizeof(struct udmabuf_create_item);
  struct udmabuf_create_list *request = calloc(1, request_size);
  TEST_ASSERT_NOT_NULL(request);
  request->count = 2;
  request->list[0] = (struct udmabuf_create_item){
      .memfd = (uint32_t)memfd, .offset = 0, .size = 4096};
  request->list[1] = request->list[0];
  int dmabuf = ioctl(dev, UDMABUF_CREATE_LIST, request);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dmabuf);
  uint32_t *map =
      mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, map);
  map[0] = 0xabcdef01;
  TEST_ASSERT_EQUAL_HEX32(0xabcdef01, map[1024]);
  munmap(map, 8192);
  close(dmabuf);

  request->list[1].__pad = 1;
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, ioctl(dev, UDMABUF_CREATE_LIST, request));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  free(request);

  int unsealed = memfd_create("unsealed", MFD_ALLOW_SEALING);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, unsealed);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(unsealed, 4096));
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, create_udmabuf(dev, unsealed, 0, 0, 4096));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(unsealed);

  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, create_udmabuf(dev, memfd, 2, 0, 4096));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(memfd);
  close(dev);
}

static void test_drm_import_deduplicates(void) {
  int dev = open("/dev/udmabuf", O_RDWR);
  int memfd = create_memfd(4096, F_SEAL_SHRINK);
  int dmabuf = create_udmabuf(dev, memfd, 0, 0, 4096);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dmabuf);
  int drm = open("/dev/dri/card0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, drm);
  struct drm_prime_handle first = {.fd = dmabuf};
  struct drm_prime_handle second = {.fd = dmabuf};
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &first));
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &second));
  TEST_ASSERT_EQUAL_UINT32(first.handle, second.handle);
  struct drm_gem_close close_handle = {.handle = first.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm, DRM_IOCTL_GEM_CLOSE, &close_handle));
  close(drm);
  close(dmabuf);
  close(memfd);
  close(dev);
}

static void test_scm_rights_lifetime(void) {
  int dev = open("/dev/udmabuf", O_RDWR);
  int memfd = create_memfd(4096, F_SEAL_SHRINK);
  int dmabuf = create_udmabuf(dev, memfd, 0, 0, 4096);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dmabuf);
  uint32_t *source =
      mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, source);
  source[0] = 0x51c0ffee;

  int sockets[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  char byte = 'd';
  char control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {.iov_base = &byte, .iov_len = 1};
  struct msghdr message = {.msg_iov = &iov,
                           .msg_iovlen = 1,
                           .msg_control = control,
                           .msg_controllen = sizeof(control)};
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &dmabuf, sizeof(dmabuf));
  TEST_ASSERT_EQUAL_INT(1, sendmsg(sockets[0], &message, 0));
  close(dmabuf);
  close(memfd);

  memset(control, 0, sizeof(control));
  byte = 0;
  message.msg_controllen = sizeof(control);
  TEST_ASSERT_EQUAL_INT(1, recvmsg(sockets[1], &message, 0));
  cmsg = CMSG_FIRSTHDR(&message);
  TEST_ASSERT_NOT_NULL(cmsg);
  TEST_ASSERT_EQUAL_INT(SCM_RIGHTS, cmsg->cmsg_type);
  int received = -1;
  memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, received);
  uint32_t *map =
      mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, received, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, map);
  TEST_ASSERT_EQUAL_HEX32(0x51c0ffee, map[0]);

  munmap(map, 4096);
  munmap(source, 4096);
  close(received);
  close(sockets[0]);
  close(sockets[1]);
  close(dev);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_memfd_seal_contract);
  RUN_TEST(test_create_fd_mmap_sync_lifetime);
  RUN_TEST(test_create_list_alias_and_negative);
  RUN_TEST(test_drm_import_deduplicates);
  RUN_TEST(test_scm_rights_lifetime);
  return UNITY_END();
}
