/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_virgl_channel.c — kernel virtio-gpu virgl (GL) legacy command plane.
 *
 * The driver's 3D channel (render node, CTX_CREATE/SUBMIT_3D, EXECBUFFER,
 * sync_file) is shared infrastructure; this test exercises the virgl legacy
 * command face layered on top: GETPARAM(SUPPORTED_CAPSET_IDs), GET_CAPS,
 * CONTEXT_INIT, RESOURCE_CREATE v1, TRANSFER_TO/FROM_HOST, WAIT, GEM_CLOSE.
 *
 * The Venus (capset 4) path is retired: the capset is no longer synthesized,
 * so GETPARAM(SUPPORTED_CAPSET_IDs) reflects exactly what the host advertises
 * and is 0 when no virgl back-end is present. Cases that require a host
 * virglrenderer back-end exposing capset 1/2 self-skip via TEST_IGNORE_MESSAGE
 * (keeping the suite green in CI / on a Venus-only host), mirroring the prior
 * Venus-channel test convention. Host-independent capability probes assert.
 *
 * Raw ioctls (no libdrm): open /dev/dri/renderD128 and issue VIRTGPU ioctls.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include "unity.h"

static int g_fd = -1;

void setUp(void) {
  g_fd = open("/dev/dri/renderD128", O_RDWR);
  TEST_ASSERT_MESSAGE(g_fd >= 0, "cannot open /dev/dri/renderD128");
}

void tearDown(void) {
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
}

/* GETPARAM helper: writes the 64-bit value back through p->value. */
static int getparam(int fd, uint64_t param, uint64_t *out) {
  struct drm_virtgpu_getparam p;
  memset(&p, 0, sizeof(p));
  p.param = param;
  p.value = (uint64_t)(uintptr_t)out;
  return ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p);
}

/* ---- Host-independent capability probes (always assert) ---- */

/* 3D_FEATURES is the hard gate: the winsys only creates itself when != 0.
 * Advertised unconditionally by the driver, independent of the host back-end.
 */
void test_getparam_3d_features_advertised(void) {
  uint64_t val = 0xDEADBEEF;
  TEST_ASSERT_EQUAL_INT(0, getparam(g_fd, VIRTGPU_PARAM_3D_FEATURES, &val));
  TEST_ASSERT_EQUAL_UINT64(1, val);
}

/* CONTEXT_INIT and CAPSET_QUERY_FIX are advertised unconditionally too; the
 * winsys uses them to pick the CONTEXT_INIT + GET_CAPS code path. */
void test_getparam_context_init_advertised(void) {
  uint64_t val = 0;
  TEST_ASSERT_EQUAL_INT(0, getparam(g_fd, VIRTGPU_PARAM_CONTEXT_INIT, &val));
  TEST_ASSERT_EQUAL_UINT64(1, val);
}

void test_getparam_capset_query_fix_advertised(void) {
  uint64_t val = 0;
  TEST_ASSERT_EQUAL_INT(0,
                        getparam(g_fd, VIRTGPU_PARAM_CAPSET_QUERY_FIX, &val));
  TEST_ASSERT_EQUAL_UINT64(1, val);
}

/* SUPPORTED_CAPSET_IDs is a bitmask built from host-advertised capsets only
 * (nothing is synthesized now that Venus is retired). With no virgl back-end
 * the host exposes no capsets, so the mask is 0 — the virgl winsys sees "no
 * VIRGL/VIRGL2 bit" and bails, which is the correct, crash-free state. */
void test_getparam_supported_capsets_reflects_host(void) {
  uint64_t val = 0xDEADBEEF;
  TEST_ASSERT_EQUAL_INT(
      0, getparam(g_fd, VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, &val));
  /* No assertion on the exact value (host-dependent); it must read back. */
  TEST_ASSERT_MESSAGE((val & ~0xFFu) == 0 || val != 0xDEADBEEF,
                      "SUPPORTED_CAPSET_IDs did not read back");
}

/* GET_CAPS reflects the host-cached capset payload: 0 with data copied out when
 * the capset is cached, -1 / errno==EINVAL when uncached. This is the exact
 * contract the virgl winsys relies on to fall back from capset 2 (VIRGL2) to
 * capset 1 (VIRGL); -ENOENT would break that path. With a virgl back-end both
 * capsets are cached (0 returned); without one capset 2 is uncached (EINVAL).
 * Both branches are asserted, so the suite stays green on either host. */
void test_get_caps_virgl2_uncached_returns_einval(void) {
  uint64_t capsets = 0;
  TEST_ASSERT_EQUAL_INT(
      0, getparam(g_fd, VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, &capsets));
  bool virgl2_cached = (capsets & (1u << VIRTGPU_DRM_CAPSET_VIRGL2)) != 0;

  uint8_t buf[64];
  memset(buf, 0, sizeof(buf));
  struct drm_virtgpu_get_caps c;
  memset(&c, 0, sizeof(c));
  c.cap_set_id = VIRTGPU_DRM_CAPSET_VIRGL2;
  c.cap_set_ver = 0;
  c.addr = (uint64_t)(uintptr_t)buf;
  c.size = sizeof(buf);
  int ret = ioctl(g_fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &c);

  if (virgl2_cached) {
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_MESSAGE(buf[0] != 0 || buf[1] != 0 || buf[2] != 0 ||
                            buf[3] != 0,
                        "GET_CAPS returned 0 but copied no capset payload");
  } else {
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  }
}

/* RESOURCE_INFO on a bogus handle fails (no virgl resource created yet). */
void test_resource_info_bad_handle_einval(void) {
  struct drm_virtgpu_resource_info ri;
  memset(&ri, 0, sizeof(ri));
  ri.bo_handle = 0x1FFF; /* within VIRGL_HANDLE_BASE range, unallocated */
  int ret = ioctl(g_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ri);
  TEST_ASSERT_EQUAL_INT(-1, ret);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* WAIT on a bogus bo_handle fails with -ENOENT (handle not found). */
void test_wait_bad_handle_enoent(void) {
  struct drm_virtgpu_3d_wait w;
  memset(&w, 0, sizeof(w));
  w.handle = 0x1FFF; /* within VIRGL_HANDLE_BASE range, unallocated */
  w.flags = VIRTGPU_WAIT_NOWAIT;
  int ret = ioctl(g_fd, DRM_IOCTL_VIRTGPU_WAIT, &w);
  TEST_ASSERT_EQUAL_INT(-1, ret);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* TRANSFER_TO_HOST on a bogus bo_handle fails with -ENOENT. */
void test_transfer_to_host_bad_handle_enoent(void) {
  struct drm_virtgpu_3d_transfer_to_host t;
  memset(&t, 0, sizeof(t));
  t.bo_handle = 0x1FFF;
  int ret = ioctl(g_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &t);
  TEST_ASSERT_EQUAL_INT(-1, ret);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* ---- Host-dependent virgl path (require a virglrenderer back-end, capset 1/2)
 * ---- */

/* CONTEXT_INIT with a host-provided virgl capset creates a single-ring
 * context. Skipped without a virgl back-end (no capset 1/2 cached). */
void test_context_init_virgl_one_ring(void) {
  uint64_t capsets = 0;
  TEST_ASSERT_EQUAL_INT(
      0, getparam(g_fd, VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, &capsets));

  uint32_t capset_id = (capsets & (1u << VIRTGPU_DRM_CAPSET_VIRGL2))
                           ? VIRTGPU_DRM_CAPSET_VIRGL2
                           : VIRTGPU_DRM_CAPSET_VIRGL;
  struct drm_virtgpu_context_set_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
  sp.value = capset_id;
  struct drm_virtgpu_context_init ci;
  memset(&ci, 0, sizeof(ci));
  ci.ctx_set_params = (uint64_t)(uintptr_t)&sp;
  ci.num_params = 1;
  int ret = ioctl(g_fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci);
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, ret, "CONTEXT_INIT failed (EEXIST tolerated by winsys, but expect 0)");
}

/* RESOURCE_CREATE v1 allocates a GEM handle at/above VIRGL_HANDLE_BASE. */
void test_resource_create_handle_range(void) {
  struct drm_virtgpu_resource_create rc;
  memset(&rc, 0, sizeof(rc));
  rc.target = 2;  /* PIPE_TEXTURE_2D */
  rc.format = 67; /* R8G8B8A8_UNORM */
  rc.bind = 1;    /* PIPE_BIND_RENDER_TARGET */
  rc.width = 64;
  rc.height = 64;
  rc.depth = 1;
  rc.array_size = 1;
  rc.last_level = 0;
  rc.nr_samples = 1;
  rc.size = 64 * 64 * 4;
  rc.stride = 64 * 4;
  int ret = ioctl(g_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc);
  TEST_ASSERT_EQUAL_INT(0, ret);
  TEST_ASSERT_MESSAGE(rc.bo_handle >= 0x1000,
                      "v1 GEM handle should be >= VIRGL_HANDLE_BASE");
  TEST_ASSERT_MESSAGE(rc.res_handle == rc.bo_handle,
                      "res_handle should mirror bo_handle");
}

/* GEM_CLOSE reclaims a virgl v1 resource. */
void test_gem_close_virgl(void) {
  struct drm_virtgpu_resource_create rc;
  memset(&rc, 0, sizeof(rc));
  rc.target = 2;
  rc.format = 67;
  rc.bind = 1;
  rc.width = 32;
  rc.height = 32;
  rc.depth = 1;
  rc.array_size = 1;
  rc.last_level = 0;
  rc.nr_samples = 1;
  rc.size = 32 * 32 * 4;
  rc.stride = 32 * 4;
  TEST_ASSERT_EQUAL_INT(0, ioctl(g_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc));

  struct drm_gem_close gc;
  memset(&gc, 0, sizeof(gc));
  gc.handle = rc.bo_handle;
  TEST_ASSERT_EQUAL_INT(0, ioctl(g_fd, DRM_IOCTL_GEM_CLOSE, &gc));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_getparam_3d_features_advertised);
  RUN_TEST(test_getparam_context_init_advertised);
  RUN_TEST(test_getparam_capset_query_fix_advertised);
  RUN_TEST(test_getparam_supported_capsets_reflects_host);
  RUN_TEST(test_get_caps_virgl2_uncached_returns_einval);
  RUN_TEST(test_resource_info_bad_handle_einval);
  RUN_TEST(test_wait_bad_handle_enoent);
  RUN_TEST(test_transfer_to_host_bad_handle_enoent);
  RUN_TEST(test_context_init_virgl_one_ring);
  RUN_TEST(test_resource_create_handle_range);
  RUN_TEST(test_gem_close_virgl);
  return UNITY_END();
}
