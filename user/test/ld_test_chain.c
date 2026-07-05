/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// 场景 2：线性链（主→b→{a,libc}）
// 验证：加载顺序 libc 先于 b；ldb_chain/ldb_via_a 解析正确
int ldb_chain(void);
int ldb_via_a(void);
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (ldb_chain() != 42)
    return 1; // b→libc JUMP_SLOT
  if (ldb_via_a() != 42)
    return 1; // b→a→libc 跨模块
  return 0;
}
