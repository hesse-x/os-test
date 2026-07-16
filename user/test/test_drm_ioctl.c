/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* DRM ioctl regression test: verify new ioctls (GET_MAGIC, AUTH_MAGIC, ADDFB2,
 * GEM_CLOSE, GETFB, DRM_CAP_ADDFB2_MODIFIERS) work correctly. */
#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <unity.h>
#include <xos/ioctl.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. GET_MAGIC: returns non-zero magic */
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

/* 2. AUTH_MAGIC: get magic then auth it */
void test_drm_auth_magic(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Need to be master first — skip if display holds master */
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

  /* Auth with the same magic */
  rc = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

/* 3. DRM_CAP_ADDFB2_MODIFIERS: capability query */
void test_drm_cap_addfb2_modifiers(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_get_cap cap;
  memset(&cap, 0, sizeof(cap));
  cap.capability = 0x10; /* DRM_CAP_ADDFB2_MODIFIERS */
  int rc = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(0, cap.value); /* no modifiers support */

  close(fd);
}

/* 4. ADDFB2 + GETFB roundtrip */
void test_drm_addfb2_getfb(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Create a dumb buffer */
  struct drm_mode_create_dumb dumb;
  memset(&dumb, 0, sizeof(dumb));
  dumb.width = 800;
  dumb.height = 600;
  dumb.bpp = 32;
  int rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(dumb.handle != 0);

  /* ADDFB2 */
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

  /* GETFB — read back */
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

  /* Cleanup */
  rc = ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb2.fb_id);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

/* 5. GEM_CLOSE: create dumb + close handle */
void test_drm_gem_close(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Create a dumb buffer */
  struct drm_mode_create_dumb dumb;
  memset(&dumb, 0, sizeof(dumb));
  dumb.width = 800;
  dumb.height = 600;
  dumb.bpp = 32;
  int rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(dumb.handle != 0);

  /* Create a framebuffer via ADDFB2 (bumps refcount) */
  struct drm_mode_fb_cmd2 fb2;
  memset(&fb2, 0, sizeof(fb2));
  fb2.width = 800;
  fb2.height = 600;
  fb2.pixel_format = DRM_FORMAT_XRGB8888;
  fb2.handles[0] = dumb.handle;
  fb2.pitches[0] = dumb.pitch;
  rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* GEM_CLOSE — Mesa calls this after ADDFB2 to release handle reference */
  struct drm_gem_close gc;
  memset(&gc, 0, sizeof(gc));
  gc.handle = dumb.handle;
  rc = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Cleanup framebuffer */
  rc = ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb2.fb_id);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

/* 6. renderD128 的 fstat.st_rdev 必须是 makedev(226,128)，否则 libdrm 判不出
 * render */
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
  return UNITY_END();
}
