/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Verify the EGL loader and surfaceless platform without a client rendering
// API. GLESv2 is intentionally not built, so this test stops after querying
// the initialized display and exercising normal teardown.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

static int fail(const char *what, EGLint err) {
  printf("[FAIL] egl_smoke: %s (eglError=0x%x)\n", what, (unsigned)err);
  _exit(1);
}

int main(void) {
  // 1. Select surfaceless explicitly. EGL_DEFAULT_DISPLAY follows Mesa's
  // configured native platform (Wayland here) and would require a compositor
  // connection even though this test only renders to a pbuffer.
  EGLDisplay dpy =
      eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
  if (dpy == EGL_NO_DISPLAY)
    fail("eglGetPlatformDisplay(surfaceless)", eglGetError());
  printf("egl: got display %p\n", (void *)dpy);

  // eglInitialize probes the configured surfaceless backend.
  EGLint major = 0, minor = 0;
  if (!eglInitialize(dpy, &major, &minor))
    fail("eglInitialize", eglGetError());
  printf("egl: initialized %d.%d\n", (int)major, (int)minor);
  const char *vendor = eglQueryString(dpy, EGL_VENDOR);
  const char *ver = eglQueryString(dpy, EGL_VERSION);
  const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
  printf("egl: vendor=%s version=%s\n", vendor ? vendor : "(null)",
         ver ? ver : "(null)");
  printf("egl: extensions %zu bytes\n", ext ? strlen(ext) : 0);

  // Verify the normal EGL teardown path too.
  if (!eglTerminate(dpy))
    fail("eglTerminate", eglGetError());

  printf("[PASS] egl_smoke\n");
  return 0;
}
