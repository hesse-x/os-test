/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// 验证 Mesa 交叉编译产出的 libEGL.so/libGLESv2.so 在本 OS 上能跑通最小
// EGL/GLES2 路径，无需 KMS/窗口（用 pbuffer surface）。softpipe 后端期望
// 全步过;virgl 后端在 host 未给 virgl capset(P0)时会在 eglInitialize/
// eglCreateContext 失败——本程序明确打印哪一步失败，兼作 host-capset 诊断。
//
// 链接:-lEGL -lGLESv2(Mesa .so 在 build/ 根,经 mkdisk 三名拷进 /lib)。
// 仅在 Mesa 启用(-DMESA=1)时构建。无 Unity——每步打印标记,任一失败 _exit 非零,
// test_runner 报 [FAIL]。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static int fail(const char *what, EGLint err) {
  printf("[FAIL] egl_smoke: %s (eglError=0x%x)\n", what, (unsigned)err);
  _exit(1);
}

static void gl_fail(const char *what, GLenum err) {
  printf("[FAIL] egl_smoke: %s (glError=0x%x)\n", what, (unsigned)err);
  _exit(1);
}

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  if (!shader)
    gl_fail("glCreateShader", glGetError());
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE)
    gl_fail(type == GL_VERTEX_SHADER ? "vertex shader compile"
                                     : "fragment shader compile",
            glGetError());
  return shader;
}

int main(void) {
  // 1. eglGetDisplay(EGL_DEFAULT_DISPLAY) → surfaceless/DRM 平台。
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy == EGL_NO_DISPLAY)
    fail("eglGetDisplay", eglGetError());
  printf("egl: got display %p\n", (void *)dpy);

  // 2. eglInitialize → 探测后端。virgl 无 host capset 时此处失败(P0 诊断点)。
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

  // 3. eglBindAPI(OPENGL_ES_API)。
  if (!eglBindAPI(EGL_OPENGL_ES_API))
    fail("eglBindAPI", eglGetError());

  // 4. eglChooseConfig:pbuffer + GLES2 + RGBA8。
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE,
                             EGL_PBUFFER_BIT,
                             EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE,
                             8,
                             EGL_GREEN_SIZE,
                             8,
                             EGL_BLUE_SIZE,
                             8,
                             EGL_ALPHA_SIZE,
                             8,
                             EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1)
    fail("eglChooseConfig", eglGetError());
  printf("egl: chose config (%d match)\n", (int)ncfg);

  // 5. eglCreateContext(GLES2, CLIENT_VERSION=2)。
  const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT)
    fail("eglCreateContext", eglGetError());
  printf("egl: got context %p\n", (void *)ctx);

  // 6. pbuffer surface + eglMakeCurrent。
  const EGLint surf_attr[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surf_attr);
  if (surf == EGL_NO_SURFACE)
    fail("eglCreatePbufferSurface", eglGetError());
  if (!eglMakeCurrent(dpy, surf, surf, ctx))
    fail("eglMakeCurrent", eglGetError());
  printf("egl: made current (pbuffer 64x64)\n");

  EGLint width = 0, height = 0;
  if (!eglQuerySurface(dpy, surf, EGL_WIDTH, &width) ||
      !eglQuerySurface(dpy, surf, EGL_HEIGHT, &height) || width != 64 ||
      height != 64)
    fail("eglQuerySurface", eglGetError());

  // 7. Clear and read back one pixel: verify that rendering reaches pbuffer.
  glViewport(0, 0, 64, 64);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  GLenum gerr = glGetError();
  if (gerr != GL_NO_ERROR)
    gl_fail("glClear", gerr);
  unsigned char pixel[4] = {0};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  gerr = glGetError();
  if (gerr != GL_NO_ERROR || pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 ||
      pixel[3] != 255)
    gl_fail("clear readback", gerr);
  printf("gl: cleared color buffer (GL_NO_ERROR)\n");

  // 8. Compile/link shaders and draw a VBO triangle. This exercises the GLES2
  // command path beyond clear, including GLSL, vertex fetch and rasterization.
  static const char vertex_source[] =
      "attribute vec2 a_pos;\n"
      "void main(void) { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  static const char fragment_source[] =
      "precision mediump float;\n"
      "void main(void) { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
  GLuint program = glCreateProgram();
  if (!program)
    gl_fail("glCreateProgram", glGetError());
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "a_pos");
  glLinkProgram(program);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE)
    gl_fail("shader link", glGetError());

  static const GLfloat triangle[] = {-0.75f, -0.75f, 0.75f,
                                     -0.75f, 0.0f,   0.75f};
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
  glUseProgram(program);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  gerr = glGetError();
  if (gerr != GL_NO_ERROR || pixel[0] != 255 || pixel[1] != 0 ||
      pixel[2] != 0 || pixel[3] != 255)
    gl_fail("triangle draw/readback", gerr);
  glDisableVertexAttribArray(0);
  glDeleteBuffers(1, &vbo);
  glDeleteProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  printf("gl: shader VBO triangle readback verified\n");

  // 9. glFinish confirms command completion at the backend.
  glFinish();
  gerr = glGetError();
  if (gerr != GL_NO_ERROR)
    gl_fail("glFinish", gerr);

  // 10. Verify the normal EGL teardown path too.
  if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
    fail("eglMakeCurrent teardown", eglGetError());
  if (!eglDestroySurface(dpy, surf))
    fail("eglDestroySurface", eglGetError());
  if (!eglDestroyContext(dpy, ctx))
    fail("eglDestroyContext", eglGetError());
  if (!eglTerminate(dpy))
    fail("eglTerminate", eglGetError());

  printf("[PASS] egl_smoke\n");
  return 0;
}
