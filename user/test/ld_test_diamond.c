/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// 场景 3：菱形去重（主→{a,libc}, a→libc）
// 验证：libc.so 只加载一次（去重），lda_answer 调 strcmp 解析到唯一 libc.so
int lda_answer(void);
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (lda_answer() != 41)
    return 1; // a→libc JUMP_SLOT，去重后 libc 唯一
  return 0;
}
