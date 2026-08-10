/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// DRM ioctl regression test: verify new ioctls (GET_MAGIC, AUTH_MAGIC, ADDFB2,
// GEM_CLOSE, GETFB, DRM_CAP_ADDFB2_MODIFIERS) work correctly.
#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/ioctl.h>

void setUp(void) {}
void tearDown(void) {}

// 1. GET_MAGIC: returns non-zero magic
void test_drm_get_magic(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_auth auth;
  memset(&auth, 0, sizeof(auth));
  int rc = ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(auth.magic != 0);

  close(fd);
}

// 2. AUTH_MAGIC: get magic then auth it
void test_drm_auth_magic(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  // Need to be master first — skip if display holds master
  int rc = ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
  if (rc < 0 && errno == EBUSY) {
    close(fd);
    TEST_IGNORE_MESSAGE("master held by display");
    return;
  }
  TEST_ASSERT_EQUAL_INT(0, rc);

  struct drm_auth auth;
  memset(&auth, 0, sizeof(auth));
  rc = ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth);
  TEST_ASSERT_EQUAL_INT(0, rc);

  // Auth with the same magic
  rc = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

// 3. DRM_CAP_ADDFB2_MODIFIERS: capability query
void test_drm_cap_addfb2_modifiers(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_get_cap cap;
  memset(&cap, 0, sizeof(cap));
  cap.capability = 0x10; // DRM_CAP_ADDFB2_MODIFIERS
  int rc = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(0, cap.value); // no modifiers support

  close(fd);
}

// 4. ADDFB2 + GETFB roundtrip
void test_drm_addfb2_getfb(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  // Create a dumb buffer
  struct drm_mode_create_dumb dumb;
  memset(&dumb, 0, sizeof(dumb));
  dumb.width = 800;
  dumb.height = 600;
  dumb.bpp = 32;
  int rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(dumb.handle != 0);

  // ADDFB2
  struct drm_mode_fb_cmd2 fb2;
  memset(&fb2, 0, sizeof(fb2));
  fb2.width = 800;
  fb2.height = 600;
  fb2.pixel_format = DRM_FORMAT_XRGB8888;
  fb2.handles[0] = dumb.handle;
  fb2.pitches[0] = dumb.pitch;
  rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(fb2.fb_id != 0);

  // GETFB — read back
  struct drm_mode_fb_cmd getfb;
  memset(&getfb, 0, sizeof(getfb));
  getfb.fb_id = fb2.fb_id;
  rc = ioctl(fd, DRM_IOCTL_MODE_GETFB, &getfb);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(dumb.handle, getfb.handle);
  TEST_ASSERT_EQUAL_INT(800, getfb.width);
  TEST_ASSERT_EQUAL_INT(600, getfb.height);
  TEST_ASSERT_EQUAL_INT(dumb.pitch, getfb.pitch);
  TEST_ASSERT_EQUAL_INT(24, getfb.depth);

  // Cleanup
  rc = ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb2.fb_id);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

// 5. GEM_CLOSE: create dumb + close handle
void test_drm_gem_close(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  // Create a dumb buffer
  struct drm_mode_create_dumb dumb;
  memset(&dumb, 0, sizeof(dumb));
  dumb.width = 800;
  dumb.height = 600;
  dumb.bpp = 32;
  int rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(dumb.handle != 0);

  // Create a framebuffer via ADDFB2 (bumps refcount)
  struct drm_mode_fb_cmd2 fb2;
  memset(&fb2, 0, sizeof(fb2));
  fb2.width = 800;
  fb2.height = 600;
  fb2.pixel_format = DRM_FORMAT_XRGB8888;
  fb2.handles[0] = dumb.handle;
  fb2.pitches[0] = dumb.pitch;
  rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2);
  TEST_ASSERT_EQUAL_INT(0, rc);

  // GEM_CLOSE — Mesa calls this after ADDFB2 to release handle reference
  struct drm_gem_close gc;
  memset(&gc, 0, sizeof(gc));
  gc.handle = dumb.handle;
  rc = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
  TEST_ASSERT_EQUAL_INT(0, rc);

  // Cleanup framebuffer
  rc = ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb2.fb_id);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

// 6. renderD128's fstat.st_rdev must be makedev(226,128), else libdrm won't
// recognize it as a render node.
void test_drm_render_rdev(void) {
  int fd = open("/dev/dri/renderD128", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/renderD128 not available");
    return;
  }

  struct stat st;
  memset(&st, 0, sizeof(st));
  int rc = fstat(fd, &st);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(226, major(st.st_rdev));
  TEST_ASSERT_EQUAL_INT(128, minor(st.st_rdev));

  close(fd);
}

// 7. The M1.3 mock is a distinct DRM device with its own minor and ioctl path.
void test_drm_mock_device(void) {
  int fd = open("/dev/dri/card1", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card1 not available");
    return;
  }

  struct stat st;
  memset(&st, 0, sizeof(st));
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(226, major(st.st_rdev));
  TEST_ASSERT_EQUAL_INT(1, minor(st.st_rdev));

  char name[32] = {0};
  struct drm_version version;
  memset(&version, 0, sizeof(version));
  version.name = name;
  version.name_len = sizeof(name);
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_VERSION, &version));
  TEST_ASSERT_TRUE(strncmp(name, "xos_drm_mock", 12) == 0);

  struct drm_get_cap cap;
  memset(&cap, 0, sizeof(cap));
  cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_GET_CAP, &cap));
  TEST_ASSERT_EQUAL_UINT64(1, cap.value);
  close(fd);

  int render_fd = open("/dev/dri/renderD129", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, render_fd);
  memset(&st, 0, sizeof(st));
  TEST_ASSERT_EQUAL_INT(0, fstat(render_fd, &st));
  TEST_ASSERT_EQUAL_INT(226, major(st.st_rdev));
  TEST_ASSERT_EQUAL_INT(129, minor(st.st_rdev));
  close(render_fd);
}

// 8. M1.4 common file state must be isolated by device and node type.
void test_drm_core_device_isolation(void) {
  int mock_master = open("/dev/dri/card1", O_RDWR);
  int mock_peer = open("/dev/dri/card1", O_RDWR);
  int virtio_peer = open("/dev/dri/card0", O_RDWR);
  int mock_render = open("/dev/dri/renderD129", O_RDWR);
  if (mock_master < 0 || mock_peer < 0 || virtio_peer < 0 || mock_render < 0) {
    if (mock_master >= 0)
      close(mock_master);
    if (mock_peer >= 0)
      close(mock_peer);
    if (virtio_peer >= 0)
      close(virtio_peer);
    if (mock_render >= 0)
      close(mock_render);
    TEST_IGNORE_MESSAGE("virtio + mock DRM devices not available");
    return;
  }

  TEST_ASSERT_EQUAL_INT(0, ioctl(mock_master, DRM_IOCTL_SET_MASTER, 0));
  TEST_ASSERT_EQUAL_INT(-1, ioctl(mock_peer, DRM_IOCTL_SET_MASTER, 0));
  TEST_ASSERT_EQUAL_INT(EBUSY, errno);

  struct drm_auth foreign_magic;
  memset(&foreign_magic, 0, sizeof(foreign_magic));
  TEST_ASSERT_EQUAL_INT(
      0, ioctl(virtio_peer, DRM_IOCTL_GET_MAGIC, &foreign_magic));
  TEST_ASSERT_EQUAL_INT(
      -1, ioctl(mock_master, DRM_IOCTL_AUTH_MAGIC, &foreign_magic));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);

  struct drm_auth local_magic;
  memset(&local_magic, 0, sizeof(local_magic));
  TEST_ASSERT_EQUAL_INT(0, ioctl(mock_peer, DRM_IOCTL_GET_MAGIC, &local_magic));
  TEST_ASSERT_EQUAL_INT(0,
                        ioctl(mock_master, DRM_IOCTL_AUTH_MAGIC, &local_magic));

  TEST_ASSERT_EQUAL_INT(-1, ioctl(mock_render, DRM_IOCTL_SET_MASTER, 0));
  TEST_ASSERT_EQUAL_INT(EACCES, errno);

  struct drm_mode_obj_get_properties object = {
      .obj_id = 3,
      .obj_type = DRM_MODE_OBJECT_CONNECTOR,
  };
  TEST_ASSERT_EQUAL_INT(
      0, ioctl(mock_master, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object));
  TEST_ASSERT_EQUAL_UINT32(2, object.count_props);
  uint32_t properties[2] = {0};
  uint64_t values[2] = {0};
  object.props_ptr = (uint64_t)(uintptr_t)properties;
  object.prop_values_ptr = (uint64_t)(uintptr_t)values;
  TEST_ASSERT_EQUAL_INT(
      0, ioctl(mock_master, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object));

  struct drm_mode_get_property mock_property = {.prop_id = properties[0]};
  TEST_ASSERT_EQUAL_INT(
      0, ioctl(mock_master, DRM_IOCTL_MODE_GETPROPERTY, &mock_property));
  TEST_ASSERT_EQUAL_STRING("MOCK_VALUE", mock_property.name);
  TEST_ASSERT_EQUAL_UINT64(2, values[0]);

  /* The same numeric property ID resolves in card0's own namespace. */
  struct drm_mode_get_property virtio_property = {.prop_id = properties[0]};
  TEST_ASSERT_EQUAL_INT(
      0, ioctl(virtio_peer, DRM_IOCTL_MODE_GETPROPERTY, &virtio_property));
  TEST_ASSERT_EQUAL_STRING("SRC_X", virtio_property.name);

  struct drm_mode_get_blob blob = {
      .blob_id = (uint32_t)values[1],
      .length = sizeof(uint64_t),
  };
  uint64_t blob_generation = 0;
  blob.data = (uint64_t)(uintptr_t)&blob_generation;
  TEST_ASSERT_EQUAL_INT(0,
                        ioctl(mock_master, DRM_IOCTL_MODE_GETPROPBLOB, &blob));
  TEST_ASSERT_EQUAL_UINT64(2, blob_generation);

  TEST_ASSERT_EQUAL_INT(
      -1, ioctl(mock_render, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object));
  TEST_ASSERT_EQUAL_INT(EACCES, errno);
  TEST_ASSERT_EQUAL_INT(0, ioctl(mock_master, DRM_IOCTL_DROP_MASTER, 0));

  close(mock_render);
  close(virtio_peer);
  close(mock_peer);
  close(mock_master);
}

// 9. GEM mmap authorization is per file; the VMA, not the handle, owns the
// backing after GEM_CLOSE and fd close.
void test_drm_gem_vma_lifetime_and_authorization(void) {
  const size_t page_size = 4096;
  int owner = open("/dev/dri/card0", O_RDWR);
  int peer = open("/dev/dri/card0", O_RDWR);
  if (owner < 0 || peer < 0) {
    if (owner >= 0)
      close(owner);
    if (peer >= 0)
      close(peer);
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_create_dumb dumb = {.width = 800, .height = 600, .bpp = 32};
  TEST_ASSERT_EQUAL_INT(0, ioctl(owner, DRM_IOCTL_MODE_CREATE_DUMB, &dumb));
  struct drm_mode_map_dumb map = {.handle = dumb.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(owner, DRM_IOCTL_MODE_MAP_DUMB, &map));
  TEST_ASSERT_NOT_EQUAL((uint64_t)dumb.handle << 12, map.offset);

  void *guessed = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       peer, (off_t)map.offset);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, guessed);
  TEST_ASSERT_EQUAL_INT(EACCES, errno);

  uint32_t *view = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        owner, (off_t)map.offset);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, view);
  view[page_size / sizeof(*view)] = 0x15a15a15u;

  struct drm_gem_close gem_close = {.handle = dumb.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(owner, DRM_IOCTL_GEM_CLOSE, &gem_close));
  close(owner);
  TEST_ASSERT_EQUAL_HEX32(0x15a15a15u, view[page_size / sizeof(*view)]);

  pid_t child = fork();
  TEST_ASSERT_NOT_EQUAL(-1, child);
  if (child == 0) {
    if (view[page_size / sizeof(*view)] != 0x15a15a15u)
      _exit(2);
    view[2 * page_size / sizeof(*view)] = 0x25a25a25u;
    _exit(0);
  }
  TEST_ASSERT_GREATER_THAN_INT(0, child);
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
  TEST_ASSERT_EQUAL_HEX32(0x25a25a25u, view[2 * page_size / sizeof(*view)]);
  TEST_ASSERT_EQUAL_INT(0, munmap(view, page_size));
  TEST_ASSERT_EQUAL_INT(
      0, munmap((char *)view + page_size, dumb.size - page_size));
  close(peer);
}

// 10. PRIME fd owns the GEM object independently and rejects cross-device
// imports even when numeric handles overlap.
void test_drm_prime_object_lifetime_and_cross_device(void) {
  int exporter = open("/dev/dri/card0", O_RDWR);
  int importer = open("/dev/dri/card0", O_RDWR);
  int foreign = open("/dev/dri/card1", O_RDWR);
  if (exporter < 0 || importer < 0 || foreign < 0) {
    if (exporter >= 0)
      close(exporter);
    if (importer >= 0)
      close(importer);
    if (foreign >= 0)
      close(foreign);
    TEST_IGNORE_MESSAGE("virtio + mock DRM devices not available");
    return;
  }

  struct drm_mode_create_dumb dumb = {.width = 800, .height = 600, .bpp = 32};
  TEST_ASSERT_EQUAL_INT(0, ioctl(exporter, DRM_IOCTL_MODE_CREATE_DUMB, &dumb));
  struct drm_prime_handle prime = {.handle = dumb.handle,
                                   .flags = DRM_CLOEXEC | DRM_RDWR};
  TEST_ASSERT_EQUAL_INT(0,
                        ioctl(exporter, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, prime.fd);

  struct drm_gem_close gem_close = {.handle = dumb.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(exporter, DRM_IOCTL_GEM_CLOSE, &gem_close));
  close(exporter);

  struct drm_prime_handle cross = {.fd = prime.fd};
  TEST_ASSERT_EQUAL_INT(-1,
                        ioctl(foreign, DRM_IOCTL_PRIME_FD_TO_HANDLE, &cross));
  TEST_ASSERT_EQUAL_INT(EXDEV, errno);

  struct drm_prime_handle imported = {.fd = prime.fd};
  TEST_ASSERT_EQUAL_INT(
      0, ioctl(importer, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imported));
  close(prime.fd);

  struct drm_mode_map_dumb map = {.handle = imported.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(importer, DRM_IOCTL_MODE_MAP_DUMB, &map));
  uint32_t *view = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        importer, (off_t)map.offset);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, view);
  view[0] = 0x51515151u;
  gem_close.handle = imported.handle;
  TEST_ASSERT_EQUAL_INT(0, ioctl(importer, DRM_IOCTL_GEM_CLOSE, &gem_close));
  close(importer);
  TEST_ASSERT_EQUAL_HEX32(0x51515151u, view[0]);
  TEST_ASSERT_EQUAL_INT(0, munmap(view, dumb.size));
  close(foreign);
}

// PRIME descriptors provide the minimal dma-buf-style ABI consumed by
// llvmpipe, while preserving export access mode and VMA ownership.
void test_drm_prime_fd_file_semantics(void) {
  int drm_fd = open("/dev/dri/card0", O_RDWR);
  if (drm_fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_create_dumb dumb = {.width = 800, .height = 600, .bpp = 32};
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb));

  struct drm_prime_handle ro = {.handle = dumb.handle, .flags = DRM_CLOEXEC};
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ro));
  void *bad =
      mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, ro.fd, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, bad);
  TEST_ASSERT_EQUAL_INT(EACCES, errno);
  close(ro.fd);

  struct drm_prime_handle prime = {.handle = dumb.handle,
                                   .flags = DRM_CLOEXEC | DRM_RDWR};
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(prime.fd, &st));
  TEST_ASSERT_EQUAL_UINT64(dumb.size, (uint64_t)st.st_size);
  TEST_ASSERT_EQUAL_INT(S_IFREG, st.st_mode & S_IFMT);
  TEST_ASSERT_EQUAL_INT(4096, st.st_blksize);
  TEST_ASSERT_TRUE(st.st_ino != 0);
  TEST_ASSERT_EQUAL_INT64((off_t)dumb.size, lseek(prime.fd, 0, SEEK_END));
  TEST_ASSERT_EQUAL_INT64(0, lseek(prime.fd, 0, SEEK_SET));
  TEST_ASSERT_EQUAL_INT64(-1, lseek(prime.fd, -1, SEEK_SET));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  struct pollfd pfd = {.fd = prime.fd, .events = POLLIN | POLLOUT};
  TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, 0));
  TEST_ASSERT_BITS(POLLIN | POLLOUT, POLLIN | POLLOUT, pfd.revents);

  int duplicate = fcntl(prime.fd, F_DUPFD_CLOEXEC, 0);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, duplicate);
  TEST_ASSERT_BITS(FD_CLOEXEC, FD_CLOEXEC, fcntl(duplicate, F_GETFD));

  uint32_t *view =
      mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, duplicate, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, view);
  bad = mmap(NULL, dumb.size, PROT_READ, MAP_PRIVATE, duplicate, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, bad);
  bad = mmap(NULL, dumb.size, PROT_EXEC, MAP_SHARED, duplicate, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, bad);
  bad = mmap(NULL, dumb.size + 1, PROT_READ, MAP_SHARED, duplicate, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, bad);

  view[0] = 0x119535ffu;
  close(prime.fd);
  close(duplicate);
  struct drm_gem_close gem_close = {.handle = dumb.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close));
  close(drm_fd);
  TEST_ASSERT_EQUAL_HEX32(0x119535ffu, view[0]);
  TEST_ASSERT_EQUAL_INT(0, munmap(view, dumb.size));
}

// 11. Binary syncobjs are file-scoped, waitable, and keep fences alive through
// sync_file descriptors independently of their DRM fd.
void test_drm_binary_syncobj_and_sync_file(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  int foreign = open("/dev/dri/card1", O_RDWR);
  if (fd < 0 || foreign < 0) {
    if (fd >= 0)
      close(fd);
    if (foreign >= 0)
      close(foreign);
    TEST_IGNORE_MESSAGE("virtio + mock DRM devices not available");
    return;
  }

  struct drm_get_cap cap = {.capability = DRM_CAP_SYNCOBJ};
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_GET_CAP, &cap));
  TEST_ASSERT_EQUAL_UINT64(1, cap.value);
  cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_GET_CAP, &cap));
  TEST_ASSERT_EQUAL_UINT64(0, cap.value);

  struct drm_syncobj_create local = {0};
  struct drm_syncobj_create other = {0};
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &local));
  TEST_ASSERT_EQUAL_INT(0, ioctl(foreign, DRM_IOCTL_SYNCOBJ_CREATE, &other));
  TEST_ASSERT_EQUAL_UINT32(local.handle, other.handle);

  uint32_t local_handle = local.handle;
  struct drm_syncobj_wait wait = {.handles = (uintptr_t)&local_handle,
                                  .timeout_nsec = 0,
                                  .count_handles = 1};
  TEST_ASSERT_EQUAL_INT(-1, ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait));
  TEST_ASSERT_EQUAL_INT(ETIME, errno);

  struct drm_syncobj_array array = {.handles = (uintptr_t)&local_handle,
                                    .count_handles = 1};
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array));
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait));

  uint32_t foreign_handle = other.handle;
  struct drm_syncobj_wait foreign_wait = {.handles = (uintptr_t)&foreign_handle,
                                          .timeout_nsec = 0,
                                          .count_handles = 1};
  TEST_ASSERT_EQUAL_INT(-1,
                        ioctl(foreign, DRM_IOCTL_SYNCOBJ_WAIT, &foreign_wait));
  TEST_ASSERT_EQUAL_INT(ETIME, errno);

  struct drm_syncobj_handle export = {
      .handle = local.handle,
      .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE};
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, export.fd);

  struct drm_syncobj_handle import = {
      .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
      .fd = export.fd};
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import));
  uint32_t imported_handle = import.handle;
  wait.handles = (uintptr_t)&imported_handle;
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait));

  struct drm_syncobj_destroy destroy = {.handle = local.handle};
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
  destroy.handle = import.handle;
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
  close(fd);

  struct pollfd pfd = {.fd = export.fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, 0));
  TEST_ASSERT_BITS(POLLIN, POLLIN, pfd.revents);
  close(export.fd);

  destroy.handle = other.handle;
  TEST_ASSERT_EQUAL_INT(0, ioctl(foreign, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
  close(foreign);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_drm_get_magic);
  RUN_TEST(test_drm_auth_magic);
  RUN_TEST(test_drm_cap_addfb2_modifiers);
  RUN_TEST(test_drm_addfb2_getfb);
  RUN_TEST(test_drm_gem_close);
  RUN_TEST(test_drm_render_rdev);
  RUN_TEST(test_drm_mock_device);
  RUN_TEST(test_drm_core_device_isolation);
  RUN_TEST(test_drm_gem_vma_lifetime_and_authorization);
  RUN_TEST(test_drm_prime_object_lifetime_and_cross_device);
  RUN_TEST(test_drm_prime_fd_file_semantics);
  RUN_TEST(test_drm_binary_syncobj_and_sync_file);
  return UNITY_END();
}
