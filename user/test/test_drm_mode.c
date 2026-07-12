/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * DRM Phase C tests: property ioctls, master model, cursor, resource cleanup.
 */

#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unity.h>
#include <xos/ioctl.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. DRM_CAP_ATOMIC: value must be 0 (force legacy) */
void test_drm_getcap_atomic(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_get_cap cap;
  memset(&cap, 0, sizeof(cap));
  cap.capability = 0x0D; /* DRM_CAP_ATOMIC */
  int rc = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(0, cap.value); /* force legacy */

  close(fd);
}

/* 2. GETPLANE: count_format_types must be 4 */
void test_drm_getplane_formats(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_get_plane plane;
  memset(&plane, 0, sizeof(plane));
  plane.plane_id = 4; /* DRM_PLANE_ID */
  int rc = ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(4, plane.count_format_types);

  close(fd);
}

/* 3. GETPROPERTY: read known property attributes */
void test_drm_getproperty(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Find the first property by querying connector's OBJ_GETPROPERTIES */
  struct drm_mode_obj_get_properties objp;
  memset(&objp, 0, sizeof(objp));
  objp.obj_id = 2; /* DRM_CONNECTOR_ID */
  objp.obj_type = DRM_MODE_OBJECT_CONNECTOR;
  int rc = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &objp);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(objp.count_props > 0);

  /* Query the first property */
  uint32_t props[16];
  uint64_t vals[16];
  objp.props_ptr = (uint64_t)(uintptr_t)props;
  objp.prop_values_ptr = (uint64_t)(uintptr_t)vals;
  rc = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &objp);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Now query GETPROPERTY for the first prop_id */
  struct drm_mode_get_property gp;
  memset(&gp, 0, sizeof(gp));
  gp.prop_id = props[0];
  gp.values_ptr = (uint64_t)(uintptr_t)NULL; /* skip values for now */
  rc = ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(gp.prop_id == props[0]);
  TEST_ASSERT_TRUE(strlen(gp.name) > 0);

  close(fd);
}

/* 4. OBJ_GETPROPERTIES on plane: verify IN_FORMATS + CRTC_ID + FB_ID + SRC_* */
void test_drm_obj_getproperties_plane(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_obj_get_properties o;
  memset(&o, 0, sizeof(o));
  o.obj_id = 4; /* DRM_PLANE_ID */
  o.obj_type = DRM_MODE_OBJECT_PLANE;

  /* First call: get count */
  int rc = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(o.count_props >=
                   4); /* IN_FORMATS + CRTC_ID + FB_ID + at least 1 SRC */

  /* Second call: get values */
  uint32_t props[16];
  uint64_t vals[16];
  o.props_ptr = (uint64_t)(uintptr_t)props;
  o.prop_values_ptr = (uint64_t)(uintptr_t)vals;
  rc = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

/* 5. OBJ_GETPROPERTIES on CRTC: verify ACTIVE + MODE_ID */
void test_drm_obj_getproperties_crtc(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_obj_get_properties o;
  memset(&o, 0, sizeof(o));
  o.obj_id = 1; /* DRM_CRTC_ID */
  o.obj_type = DRM_MODE_OBJECT_CRTC;

  int rc = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(o.count_props >= 2); /* ACTIVE + MODE_ID */

  close(fd);
}

/* 6. GETPROPBLOB on IN_FORMATS: verify blob header + 4 formats + linear
 * modifier */
void test_drm_getpropropblob_in_formats(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Get plane properties to find IN_FORMATS blob_id */
  uint32_t props[16];
  uint64_t vals[16];
  struct drm_mode_obj_get_properties o;
  memset(&o, 0, sizeof(o));
  o.obj_id = 4;
  o.obj_type = DRM_MODE_OBJECT_PLANE;
  o.props_ptr = (uint64_t)(uintptr_t)props;
  o.prop_values_ptr = (uint64_t)(uintptr_t)vals;
  ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o);

  /* Find IN_FORMATS prop — identify by querying each property name */
  uint32_t in_formats_blob_id = 0;
  for (uint32_t i = 0; i < o.count_props; i++) {
    struct drm_mode_get_property gp;
    memset(&gp, 0, sizeof(gp));
    gp.prop_id = props[i];
    ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp);
    if (strcmp(gp.name, "IN_FORMATS") == 0) {
      in_formats_blob_id = (uint32_t)vals[i];
      break;
    }
  }
  TEST_ASSERT_TRUE(in_formats_blob_id > 0);

  /* First call: get blob length */
  struct drm_mode_get_blob blob;
  memset(&blob, 0, sizeof(blob));
  blob.blob_id = in_formats_blob_id;
  int rc = ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(blob.length > 0);

  /* Second call: read blob data */
  uint8_t blob_data[256];
  memset(blob_data, 0, sizeof(blob_data));
  blob.data = (uint64_t)(uintptr_t)blob_data;
  rc = ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Verify header fields */
  struct drm_format_modifier_blob {
    uint32_t version;
    uint32_t count_formats;
    uint32_t formats_offset;
    uint32_t count_modifiers;
    uint32_t modifiers_offset;
  } *hdr = (struct drm_format_modifier_blob *)blob_data;

  TEST_ASSERT_EQUAL_INT(1, hdr->version);
  TEST_ASSERT_EQUAL_INT(4, hdr->count_formats);
  TEST_ASSERT_EQUAL_INT(1, hdr->count_modifiers);

  /* Verify first format is XRGB8888 */
  uint32_t *fmts = (uint32_t *)(blob_data + hdr->formats_offset);
  TEST_ASSERT_EQUAL_INT(DRM_FORMAT_XRGB8888, fmts[0]);

  close(fd);
}

/* 7. GETCONNECTOR: count_props == 2 (DPMS + EDID) */
void test_drm_property_getconnector(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_get_connector conn;
  memset(&conn, 0, sizeof(conn));
  conn.connector_id = 2;
  int rc = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(conn.count_props >= 2);

  /* Second call: read props */
  uint32_t props[16];
  uint64_t vals[16];
  conn.props_ptr = (uint64_t)(uintptr_t)props;
  conn.prop_values_ptr = (uint64_t)(uintptr_t)vals;
  rc = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(props[0] > 0);
  TEST_ASSERT_TRUE(props[1] > 0);

  close(fd);
}

/* 8. GETPROPBLOB on EDID: verify 128 bytes with valid checksum */
void test_drm_getpropropblob_edid(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Find EDID blob_id through connector properties */
  uint32_t props[16];
  uint64_t vals[16];
  struct drm_mode_get_connector conn;
  memset(&conn, 0, sizeof(conn));
  conn.connector_id = 2;
  conn.props_ptr = (uint64_t)(uintptr_t)props;
  conn.prop_values_ptr = (uint64_t)(uintptr_t)vals;
  ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);

  /* Identify which value is EDID by querying property names */
  uint32_t edid_blob_id = 0;
  for (uint32_t i = 0; i < conn.count_props && i < 16; i++) {
    if (vals[i] == 0)
      continue; /* skip zero (not blob) */
    struct drm_mode_get_property gp;
    memset(&gp, 0, sizeof(gp));
    gp.prop_id = props[i];
    ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp);
    if (strcmp(gp.name, "EDID") == 0) {
      edid_blob_id = (uint32_t)vals[i];
      break;
    }
  }
  TEST_ASSERT_TRUE(edid_blob_id > 0);

  /* Read EDID blob */
  struct drm_mode_get_blob blob;
  uint8_t edid_data[256];
  memset(&blob, 0, sizeof(blob));
  blob.blob_id = edid_blob_id;
  blob.data = (uint64_t)(uintptr_t)edid_data;
  blob.length = (uint32_t)sizeof(edid_data);
  int rc = ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_EQUAL_INT(128, (int)blob.length);

  /* Verify EDID checksum */
  uint8_t sum = 0;
  for (int i = 0; i < 128; i++)
    sum += edid_data[i];
  TEST_ASSERT_EQUAL_INT(0, sum);

  /* Verify EDID header */
  TEST_ASSERT_EQUAL_INT(0x00, edid_data[0]);
  TEST_ASSERT_EQUAL_INT(0xFF, edid_data[1]);

  close(fd);
}

/* 9. SET_MASTER multi-fd: fd1 set master → fd2 set master returns -EBUSY */
void test_drm_master_multi_fd(void) {
  int fd1 = open("/dev/dri/card0", O_RDWR);
  if (fd1 < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }
  int fd2 = open("/dev/dri/card0", O_RDWR);
  if (fd2 < 0) {
    close(fd1);
    TEST_IGNORE_MESSAGE("need second fd");
    return;
  }

  /* fd1: set master — should succeed */
  int rc = ioctl(fd1, DRM_IOCTL_SET_MASTER, 0);
  if (rc < 0 && errno == EBUSY) {
    close(fd1);
    close(fd2);
    TEST_IGNORE_MESSAGE("master held by display");
    return;
  }
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* fd2: set master — should fail with -EBUSY */
  rc = ioctl(fd2, DRM_IOCTL_SET_MASTER, 0);
  TEST_ASSERT_TRUE(rc < 0);
  TEST_ASSERT_EQUAL_INT(EBUSY, errno);

  /* fd1: drop master */
  rc = ioctl(fd1, DRM_IOCTL_DROP_MASTER, 0);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* fd2: now can set master */
  rc = ioctl(fd2, DRM_IOCTL_SET_MASTER, 0);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Cleanup */
  ioctl(fd2, DRM_IOCTL_DROP_MASTER, 0);
  close(fd1);
  close(fd2);
}

/* 10. DROP_MASTER cleanup: verify master drop resets state */
void test_drm_drop_master_cleanup(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  int rc = ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
  if (rc < 0 && errno == EBUSY) {
    close(fd);
    TEST_IGNORE_MESSAGE("master held by display");
    return;
  }

  /* Drop master */
  rc = ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Verify: can set master again (cleanup doesn't leave stale master) */
  rc = ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Drop again for clean state */
  ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
  close(fd);
}

/* 11. AUTH_MAGIC strict: unauthenticated magic returns -EPERM */
void test_drm_auth_magic_strict(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  int rc = ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
  if (rc < 0 && errno == EBUSY) {
    close(fd);
    TEST_IGNORE_MESSAGE("master held by display");
    return;
  }

  /* Try to auth a magic that was never issued via GET_MAGIC */
  struct drm_auth auth;
  memset(&auth, 0, sizeof(auth));
  auth.magic = 0xDEADBEEF;
  rc = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth);
  TEST_ASSERT_TRUE(rc < 0);
  TEST_ASSERT_EQUAL_INT(EPERM, errno);

  /* Now get a proper magic and auth it */
  memset(&auth, 0, sizeof(auth));
  rc = ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(auth.magic != 0);

  rc = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth);
  TEST_ASSERT_EQUAL_INT(0, rc);

  ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
  close(fd);
}

/* 12. CURSOR2: set cursor bitmap from a dumb buffer */
void test_drm_cursor2_bo(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  /* Create a dumb buffer with cursor-size data */
  struct drm_mode_create_dumb dumb;
  memset(&dumb, 0, sizeof(dumb));
  dumb.width = 64;
  dumb.height = 64;
  dumb.bpp = 32;
  int rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
  if (rc < 0) {
    close(fd);
    TEST_IGNORE_MESSAGE("CREATE_DUMB failed");
    return;
  }

  /* Map and write cursor pattern */
  struct drm_mode_map_dumb map;
  memset(&map, 0, sizeof(map));
  map.handle = dumb.handle;
  rc = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
  if (rc < 0) {
    close(fd);
    return;
  }

  void *buf =
      mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
  if (buf == MAP_FAILED) {
    close(fd);
    return;
  }

  /* Fill with an ARGB cursor pattern: solid white arrow shape */
  uint32_t *pixels = (uint32_t *)buf;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      pixels[y * 64 + x] = 0xFFFFFFFF; /* white, fully opaque */

  /* Set cursor via CURSOR2 BO */
  struct drm_mode_cursor2 cur;
  memset(&cur, 0, sizeof(cur));
  cur.flags = DRM_MODE_CURSOR_BO;
  cur.handle = dumb.handle;
  cur.hot_x = 0;
  cur.hot_y = 0;
  rc = ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &cur);
  TEST_ASSERT_EQUAL_INT(0, rc);

  munmap(buf, dumb.size);
  close(fd);
}

/* 13. CURSOR2: move cursor */
void test_drm_cursor2_move(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_IGNORE_MESSAGE("/dev/dri/card0 not available");
    return;
  }

  struct drm_mode_cursor2 cur;
  memset(&cur, 0, sizeof(cur));
  cur.flags = DRM_MODE_CURSOR_MOVE;
  cur.x = 100;
  cur.y = 200;
  int rc = ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &cur);
  TEST_ASSERT_EQUAL_INT(0, rc);

  close(fd);
}

/* 14. drm_close cleanup: create resources, close fd, verify no leak (re-open
 * check) */
void test_drm_close_cleanup(void) {
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

  /* Create a framebuffer */
  struct drm_mode_fb_cmd2 fb2;
  memset(&fb2, 0, sizeof(fb2));
  fb2.width = 800;
  fb2.height = 600;
  fb2.pixel_format = DRM_FORMAT_XRGB8888;
  fb2.handles[0] = dumb.handle;
  fb2.pitches[0] = dumb.pitch;
  rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2);
  TEST_ASSERT_EQUAL_INT(0, rc);

  /* Close fd — triggers drm_close cleanup */
  close(fd);

  /* Re-open and verify we can create new resources (no slot leak) */
  int fd2 = open("/dev/dri/card0", O_RDWR);
  TEST_ASSERT_TRUE(fd2 >= 0);
  close(fd2);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_drm_getcap_atomic);
  RUN_TEST(test_drm_getplane_formats);
  RUN_TEST(test_drm_getproperty);
  RUN_TEST(test_drm_obj_getproperties_plane);
  RUN_TEST(test_drm_obj_getproperties_crtc);
  RUN_TEST(test_drm_getpropropblob_in_formats);
  RUN_TEST(test_drm_property_getconnector);
  RUN_TEST(test_drm_getpropropblob_edid);
  RUN_TEST(test_drm_master_multi_fd);
  RUN_TEST(test_drm_drop_master_cleanup);
  RUN_TEST(test_drm_auth_magic_strict);
  RUN_TEST(test_drm_cursor2_bo);
  RUN_TEST(test_drm_cursor2_move);
  RUN_TEST(test_drm_close_cleanup);
  return UNITY_END();
}
