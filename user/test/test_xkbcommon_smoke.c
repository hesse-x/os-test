/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_xkbcommon_smoke — wlroots 前置依赖 libxkbcommon 的目标端冒烟测试。
//
// 仅验证纯逻辑路径：context 创建、自包含 keymap
// 编译（xkb_keymap_new_from_string， 不依赖磁盘 XKB 规则数据——那是 WF-4
// 的范围）、keysym 名称↔数值转换、keymap 内 键的符号查询。证明
// build/libxkbcommon.so 能被 musl loader 加载、符号（含
// secure_getenv）可解析、最小 API 行为正确。参考 test_pixman_smoke.c
// 的诊断式风格。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static int fail(const char *what) {
  printf("[FAIL] xkbcommon_smoke: %s\n", what);
  _exit(1);
}

int main(void) {
  // 1. 创建 context，禁用默认 include 路径——本测试用自包含 keymap，不读取
  //    任何磁盘 XKB 规则数据（WF-4 才接入 XKB_CONFIG_ROOT）。
  struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
  if (!ctx)
    fail("xkb_context_new");
  printf("xkbcommon: created context %p\n", (void *)ctx);

  // 2. 编译一个自包含的最小 keymap：单键 <a>(keycode 24) 映射到 keysym 'a'。
  //    不含任何 include 指令，因此无需 XKB 数据目录。
  static const char keymap_str[] = "xkb_keymap {\n"
                                   "  xkb_keycodes { <a> = 24; };\n"
                                   "  xkb_compat {};\n"
                                   "  xkb_types {};\n"
                                   "  xkb_symbols { key <a> { [a] }; };\n"
                                   "};\n";
  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      ctx, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap)
    fail("xkb_keymap_new_from_string");
  printf("xkbcommon: compiled keymap %p\n", (void *)keymap);

  // 3. keysym 名称→数值：'a' 对应 XKB_KEY_a (0x61)。
  xkb_keysym_t ks = xkb_keysym_from_name("a", XKB_KEYSYM_NO_FLAGS);
  printf("xkbcommon: keysym 'a' = 0x%x\n", ks);
  if (ks != 0x61)
    fail("xkb_keysym_from_name('a') != 0x61");

  // 4. 数值→名称：反向解析 0x61 应回到 "a"。
  char name[64];
  int nlen = xkb_keysym_get_name(ks, name, sizeof(name));
  if (nlen <= 0 || (size_t)nlen >= sizeof(name))
    fail("xkb_keysym_get_name");
  printf("xkbcommon: keysym 0x%x -> \"%s\"\n", ks, name);
  if (strcmp(name, "a") != 0)
    fail("xkb_keysym_get_name did not return 'a'");

  // 5. keymap 内键的符号查询：keycode 24 (<a>) 的 Level0 符号应为 'a' (0x61)。
  //    xkb_keymap_key_get_syms_by_level 的 layout 与 level 形参均为 0 基
  //    （见 xkbcommon.h 注释及上游 test/keymap.c 用法），单 level 键的符号
  //    在 level 0；level 1 超出该键的 level 数 → 返回 0、syms 置 NULL。
  const xkb_keysym_t *syms = NULL;
  int nsyms = xkb_keymap_key_get_syms_by_level(keymap, 24, 0, 0, &syms);
  printf("xkbcommon: keycode 24 level0 -> %d syms (first 0x%x)\n", nsyms,
         nsyms > 0 ? syms[0] : 0);
  if (nsyms != 1 || !syms || syms[0] != 0x61)
    fail("xkb_keymap_key_get_syms_by_level(keycode 24) != 'a'");

  // 6. state 创建（验证 keymap 可构造 state，覆盖 xkb_state_new 路径）。
  struct xkb_state *state = xkb_state_new(keymap);
  if (!state)
    fail("xkb_state_new");
  printf("xkbcommon: created state %p\n", (void *)state);

  xkb_state_unref(state);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);
  printf("[OK] xkbcommon_smoke\n");
  return 0;
}
